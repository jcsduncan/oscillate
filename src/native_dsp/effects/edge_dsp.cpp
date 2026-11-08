#include "edge_dsp.hpp"

#include "edge_table.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace oscillate::native_dsp::effects::edge_dsp {
namespace {

// Kaiser's design rules: the taps a transition needs for an attenuation,
// and the window shape that gives it.
constexpr double kaiser_length_offset_db = 7.95;
constexpr double kaiser_length_slope = 2.285;
constexpr double kaiser_beta_slope = 0.1102;
constexpr double kaiser_beta_offset_db = 8.7;

// The filters, as fractions of the base rate: the band they keep flat and
// where they reach full attenuation (one minus it, mirrored about Nyquist).
// Linear phase keeps 0.45 (21.6 kHz at 48 kHz) at 120 dB, enough that a
// 16x hard clip of a 10 kHz sine 12 dB over aliases under -100 dBFS; Light
// keeps 0.40 at 90 dB in well under half the taps and latency.
struct FilterSpec {
  double pass = 0.0;
  double stopband_db = 0.0;
};
constexpr std::array<FilterSpec, edge_table::filters.size()> filter_specs{{
    {0.45, 120.0},
    {0.40, 90.0},
}};

// An antiderivative step this small is taken as no step: the curve at the
// midpoint.
constexpr double antialias_epsilon = 1e-6;

// The guard's attack (its averaging) and release, and how far under the
// 0 dBFS ceiling it holds the output so the output's float rounding stays
// under it too.
constexpr double guard_attack_seconds = 0.0005;
constexpr double guard_release_seconds = 0.05;
constexpr double guard_margin_db = 0.01;

[[nodiscard]] double kaiser_beta(double stopband_db) noexcept {
  return kaiser_beta_slope * (stopband_db - kaiser_beta_offset_db);
}

// Taps for a transition of `transition` (a fraction of the filter's rate).
[[nodiscard]] double kaiser_taps(double stopband_db, double transition) noexcept {
  return (stopband_db - kaiser_length_offset_db) /
             (kaiser_length_slope * 2.0 * std::numbers::pi * transition) +
         1.0;
}

[[nodiscard]] double sinc(double x) noexcept {
  return x == 0.0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
}

// `pass` is a fraction of the half-band's output rate, under 1/4.
Halfband design_halfband(double pass, double stopband_db) {
  const double taps = kaiser_taps(stopband_db, 0.5 - 2.0 * pass);
  const auto centre = static_cast<std::size_t>(std::max(0.0, std::ceil((taps - 3.0) / 4.0)));
  const double half_width = static_cast<double>(2 * centre + 1);
  const double beta = kaiser_beta(stopband_db);
  Halfband out;
  out.centre = centre;
  out.even.resize(2 * centre + 2);
  for (std::size_t i = 0; i < out.even.size(); ++i) {
    const double offset = static_cast<double>(2 * i) - half_width;
    out.even[i] = 0.5 * sinc(offset / 2.0) * native_dsp::dsp::kaiser(offset, half_width, beta);
  }
  return out;
}

// The top decimator with antialiasing: an even-length low-pass at a quarter
// of the top rate.
std::vector<double> design_even(double pass, double stopband_db) {
  auto length = static_cast<std::size_t>(std::ceil(kaiser_taps(stopband_db, 0.5 - 2.0 * pass)));
  length += length % 2;
  const double half_width = static_cast<double>(length - 1) / 2.0;
  const double beta = kaiser_beta(stopband_db);
  std::vector<double> out(length);
  for (std::size_t i = 0; i < length; ++i) {
    const double offset = static_cast<double>(i) - half_width;
    out[i] = 0.5 * sinc(offset / 2.0) * native_dsp::dsp::kaiser(offset, half_width, beta);
  }
  return out;
}

Design build(std::uint32_t factor, const FilterSpec& spec) {
  Design d;
  d.factor = factor;
  d.antialiased = factor >= antialiased_factor;
  while ((std::uint32_t{1} << d.stages) < factor) {
    ++d.stages;
  }
  if (d.stages == 0) {
    return d;
  }
  // Stage s runs between 2^s and 2^(s + 1) times the base rate. The first
  // keeps `pass` of the base rate; later ones need only keep what the first
  // passes (up to 1 - pass) clear of their images.
  std::array<double, max_stages> pass{};
  for (std::size_t s = 0; s < d.stages; ++s) {
    const double base = s == 0 ? spec.pass : 1.0 - spec.pass;
    pass[s] = base / static_cast<double>(std::size_t{2} << s);
    d.halfbands[s] = design_halfband(pass[s], spec.stopband_db);
  }
  // The round trip in half samples at the top rate: each half-band delays
  // its centre (2K + 1 samples at its high rate) each way.
  std::uint64_t half_samples = 0;
  for (std::size_t s = 0; s < d.stages; ++s) {
    const std::uint64_t per_sample = std::uint64_t{2} * (factor >> (s + 1));
    half_samples += per_sample * (2 * d.halfbands[s].centre + 1); // up
    const bool top = s + 1 == d.stages;
    if (top && d.antialiased) {
      d.top = design_even(pass[s], spec.stopband_db);
      half_samples += 1 + (d.top.size() - 1); // the antialiasing, then the decimator
    } else {
      half_samples += per_sample * (2 * d.halfbands[s].centre + 1); // down
    }
  }
  if (half_samples % 2 != 0) {
    throw std::logic_error("edge: the oversampler's round trip is not whole samples");
  }
  const std::uint64_t samples = half_samples / 2;
  d.pad = static_cast<std::uint32_t>((factor - samples % factor) % factor);
  d.latency = static_cast<std::uint32_t>((samples + d.pad) / factor);
  // A linear-phase response spreads no further after its delay than before.
  d.tail = d.latency + 1;
  for (std::size_t s = 0; s < d.stages; ++s) {
    if (d.halfbands[s].even.size() > max_history || d.top.size() > max_history) {
      throw std::logic_error("edge: a filter outgrows its history");
    }
  }
  return d;
}

using Designs = std::array<std::array<Design, edge_table::filters.size()>, max_stages + 1>;

Designs build_all() {
  Designs out;
  for (std::size_t o = 0; o < out.size(); ++o) {
    for (std::size_t f = 0; f < filter_specs.size(); ++f) {
      out[o][f] = build(std::uint32_t{1} << o, filter_specs[f]);
    }
  }
  return out;
}

} // namespace

double log_cosh(double u) noexcept {
  const double a = std::fabs(u);
  return a + std::log1p(std::exp(-2.0 * a)) - std::numbers::ln2;
}

double Antialiased::process(double u, const Curve& curve) noexcept {
  const double previous = previous_;
  previous_ = u;
  if (std::fabs(u) <= curve.threshold && std::fabs(previous) <= curve.threshold) {
    integral_valid_ = false;
    return 0.5 * (u + previous);
  }
  if (!integral_valid_ || integral_knee_ != curve.knee) {
    previous_integral_ = curve.integral(previous);
  }
  const double integral = curve.integral(u);
  const double step = u - previous;
  const double out = std::fabs(step) < antialias_epsilon ? curve(0.5 * (u + previous))
                                                         : (integral - previous_integral_) / step;
  previous_integral_ = integral;
  integral_knee_ = curve.knee;
  integral_valid_ = true;
  return out;
}

const Design& design(std::size_t oversampling, std::size_t filters) {
  static const Designs designs = build_all();
  return designs[std::min(oversampling, designs.size() - 1)]
                [std::min(filters, filter_specs.size() - 1)];
}

void Resampler::prepare(const Design& design) noexcept {
  design_ = &design;
  for (std::size_t s = 0; s < design.stages; ++s) {
    const auto& halfband = design.halfbands[s];
    ups_[s].history.resize(halfband.even.size());
    downs_[s].firsts.resize(halfband.even.size());
    downs_[s].seconds.resize(halfband.centre + 2);
  }
  top_.resize(design.top.size());
  pad_.resize(design.pad + 1);
}

void Resampler::reset() noexcept {
  for (auto& stage : ups_) {
    stage.history.reset();
  }
  for (auto& stage : downs_) {
    stage.firsts.reset();
    stage.seconds.reset();
  }
  top_.reset();
  pad_.reset();
}

// Zero-stuffing by two through the half-band (gain 2): the even output is the
// filter's even taps over the input, the odd one the input K frames back.
void Resampler::up(double x, double* out) noexcept {
  std::array<double, max_factor> in{};
  out[0] = x;
  std::size_t count = 1;
  for (std::size_t s = 0; s < design_->stages; ++s, count *= 2) {
    std::copy_n(out, count, in.begin());
    const auto& taps = design_->halfbands[s].even;
    auto& history = ups_[s].history;
    for (std::size_t k = 0; k < count; ++k) {
      history.push(in[k]);
      const double* h = history.newest();
      double even = 0.0;
      for (std::size_t i = 0; i < taps.size(); ++i) {
        even += taps[i] * h[i];
      }
      out[2 * k] = 2.0 * even;
      out[2 * k + 1] = h[design_->halfbands[s].centre];
    }
  }
}

// Each pair (earlier first) becomes one sample: the even taps over the
// firsts and the centre's 1/2 on the second K + 1 pairs back. The top stage
// with antialiasing runs its even-length low-pass over the plain stream.
double Resampler::down(double* in) noexcept {
  if (design_->pad > 0) {
    for (std::size_t i = 0; i < design_->factor; ++i) {
      pad_.push(in[i]);
      in[i] = pad_.newest()[design_->pad];
    }
  }
  std::size_t count = design_->factor / 2;
  for (std::size_t s = design_->stages; s-- > 0; count /= 2) {
    const bool top = s + 1 == design_->stages && design_->antialiased;
    for (std::size_t k = 0; k < count; ++k) {
      const double first = in[2 * k];
      const double second = in[2 * k + 1];
      double sum = 0.0;
      if (top) {
        top_.push(first);
        const double* h = top_.newest();
        for (std::size_t i = 0; i < design_->top.size(); ++i) {
          sum += design_->top[i] * h[i];
        }
        top_.push(second);
      } else {
        const auto& halfband = design_->halfbands[s];
        auto& stage = downs_[s];
        stage.firsts.push(first);
        stage.seconds.push(second);
        const double* h = stage.firsts.newest();
        for (std::size_t i = 0; i < halfband.even.size(); ++i) {
          sum += halfband.even[i] * h[i];
        }
        sum += 0.5 * stage.seconds.newest()[halfband.centre + 1];
      }
      in[k] = sum;
    }
  }
  return in[0];
}

std::uint32_t TruePeakGuard::delay_at(double sample_rate) noexcept {
  const auto window =
      static_cast<std::uint32_t>(std::max(1.0, std::round(guard_attack_seconds * sample_rate)));
  // The averaging must not reach past the interval's interpolation on
  // either side: 2R - 1 frames beyond it.
  return window + static_cast<std::uint32_t>(2 * radius) - 1;
}

void TruePeakGuard::prepare(double sample_rate) {
  window_ =
      static_cast<std::uint32_t>(std::max(1.0, std::round(guard_attack_seconds * sample_rate)));
  delay_ = delay_at(sample_rate);
  ceiling_ = native_dsp::dsp::db_to_gain(-guard_margin_db);
  release_ = std::exp(-1.0 / (guard_release_seconds * sample_rate));
  magnitudes_.resize(radius + 1);
  // The hold spans the averaging and the interpolation on both sides.
  queue_.assign(static_cast<std::size_t>(delay_) + 2, Held{});
  attack_.assign(window_, 1.0);
  delayed_.assign(static_cast<std::size_t>(delay_) + 1, {0.0, 0.0});
  reset();
}

void TruePeakGuard::reset() noexcept {
  for (auto& meter : peaks_) {
    meter.reset();
  }
  magnitudes_.reset();
  head_ = 0;
  count_ = 0;
  std::ranges::fill(attack_, 1.0);
  attack_position_ = 0;
  attack_sum_ = static_cast<double>(window_);
  std::ranges::fill(delayed_, std::array<double, 2>{0.0, 0.0});
  delayed_position_ = 0;
  frame_ = 0;
  envelope_ = 1.0;
}

double TruePeakGuard::process(double& left, double& right) noexcept {
  // The interval R frames back, between that frame and the next.
  magnitudes_.push(std::max(std::fabs(left), std::fabs(right)));
  const double between = std::max(peaks_[0].push(left), peaks_[1].push(right));
  const double* m = magnitudes_.newest();
  const double peak = std::max({between, m[radius], m[radius - 1]});
  const double needed = peak > ceiling_ ? ceiling_ / peak : 1.0;

  // Hold the least gain needed over the last delay + 1 intervals.
  const std::size_t capacity = queue_.size();
  const std::uint64_t hold = static_cast<std::uint64_t>(delay_) + 1;
  while (count_ > 0 && queue_[(head_ + count_ - 1) % capacity].gain >= needed) {
    --count_;
  }
  queue_[(head_ + count_) % capacity] = {frame_, needed};
  ++count_;
  while (frame_ - queue_[head_].frame >= hold) {
    head_ = (head_ + 1) % capacity;
    --count_;
  }
  const double held = queue_[head_].gain;
  envelope_ = std::min(held, 1.0 - (1.0 - envelope_) * release_);

  // Average over the attack; re-summed exactly once per lap so the sum
  // cannot drift.
  attack_sum_ += envelope_ - attack_[attack_position_];
  attack_[attack_position_] = envelope_;
  if (++attack_position_ == attack_.size()) {
    attack_position_ = 0;
    attack_sum_ = 0.0;
    for (const double value : attack_) {
      attack_sum_ += value;
    }
  }
  const double gain = attack_sum_ / static_cast<double>(window_);

  delayed_[delayed_position_] = {left, right};
  delayed_position_ = (delayed_position_ + 1) % delayed_.size();
  const auto& out = delayed_[delayed_position_];
  left = out[0] * gain;
  right = out[1] * gain;
  ++frame_;
  return gain;
}

} // namespace oscillate::native_dsp::effects::edge_dsp
