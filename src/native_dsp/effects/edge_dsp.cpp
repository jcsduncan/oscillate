#include "edge_dsp.hpp"

#include "edge_table.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <emmintrin.h>
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

// Outputs a convolution computes at once, each its own sum in tap order, in
// SSE2 pairs held in registers. Baseline x86-64 never contracts SSE2
// double arithmetic, so every output is the plain dot product's, bit for
// bit.
constexpr std::size_t pair = 2;
constexpr std::size_t lanes = 4 * pair;

struct Sums {
  __m128d a = _mm_setzero_pd();
  __m128d b = _mm_setzero_pd();
  __m128d c = _mm_setzero_pd();
  __m128d d = _mm_setzero_pd();
  void add(double tap, const double* x) noexcept {
    const __m128d t = _mm_set1_pd(tap);
    a = _mm_add_pd(a, _mm_mul_pd(t, _mm_loadu_pd(x)));
    b = _mm_add_pd(b, _mm_mul_pd(t, _mm_loadu_pd(x + pair)));
    c = _mm_add_pd(c, _mm_mul_pd(t, _mm_loadu_pd(x + 2 * pair)));
    d = _mm_add_pd(d, _mm_mul_pd(t, _mm_loadu_pd(x + 3 * pair)));
  }
  void store(double* y) const noexcept {
    _mm_storeu_pd(y, a);
    _mm_storeu_pd(y + pair, b);
    _mm_storeu_pd(y + 2 * pair, c);
    _mm_storeu_pd(y + 3 * pair, d);
  }
};

// The even-length top decimator over a stream held in pairs: tap 2j reads
// firsts[k - j] and tap 2j + 1 seconds[k - j - 1], summed in tap order.
void convolve_pairs(std::span<const double> taps, const double* firsts, const double* seconds,
                    std::size_t n, double* y) noexcept {
  const std::size_t pairs = taps.size() / 2;
  std::size_t k = 0;
  for (; k + lanes <= n; k += lanes) {
    Sums sums;
    for (std::size_t j = 0; j < pairs; ++j) {
      sums.add(taps[2 * j], firsts + k - j);
      sums.add(taps[2 * j + 1], seconds + k - j - 1);
    }
    sums.store(y + k);
  }
  for (; k < n; ++k) {
    double sum = 0.0;
    for (std::size_t j = 0; j < pairs; ++j) {
      sum += taps[2 * j] * firsts[k - j];
      sum += taps[2 * j + 1] * seconds[k - j - 1];
    }
    y[k] = sum;
  }
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

void convolve(std::span<const double> taps, const double* x, std::size_t n, double* y) noexcept {
  const std::size_t length = taps.size();
  std::size_t k = 0;
  for (; k + lanes <= n; k += lanes) {
    Sums sums;
    for (std::size_t i = 0; i < length; ++i) {
      sums.add(taps[i], x + k - i);
    }
    sums.store(y + k);
  }
  for (; k < n; ++k) {
    double sum = 0.0;
    for (std::size_t i = 0; i < length; ++i) {
      sum += taps[i] * x[k - i];
    }
    y[k] = sum;
  }
}

void Line::advance(std::size_t n) noexcept {
  std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(n), history_, data_.begin());
}

void Resampler::prepare(const Design& design) noexcept {
  design_ = &design;
  for (std::size_t s = 0; s < design.stages; ++s) {
    const auto& halfband = design.halfbands[s];
    ups_[s].resize(halfband.even.size() - 1);
    downs_[s].firsts.resize(halfband.even.size() - 1);
    downs_[s].seconds.resize(halfband.centre + 1);
  }
  // The top decimator's even taps read the firsts back half its length less
  // one; its odd taps the seconds back half its length.
  top_firsts_.resize(design.top.empty() ? 0 : design.top.size() / 2 - 1);
  top_seconds_.resize(design.top.size() / 2);
  reset();
}

void Resampler::reset() noexcept {
  for (auto& line : ups_) {
    line.reset();
  }
  for (auto& stage : downs_) {
    stage.firsts.reset();
    stage.seconds.reset();
  }
  top_firsts_.reset();
  top_seconds_.reset();
  pad_.fill(0.0);
}

// Zero-stuffing by two through the half-band (gain 2): the even output is the
// filter's even taps over the input, the odd one the input K frames back.
void Resampler::up(std::span<const double> frames, double* top) noexcept {
  std::copy(frames.begin(), frames.end(), top);
  std::size_t count = frames.size();
  for (std::size_t s = 0; s < design_->stages; ++s, count *= 2) {
    const auto& halfband = design_->halfbands[s];
    auto& line = ups_[s];
    double* const x = line.fresh();
    std::copy_n(top, count, x);
    convolve(halfband.even, x, count, sums_.data());
    const double* const centre = x - halfband.centre;
    for (std::size_t k = 0; k < count; ++k) {
      top[2 * k] = 2.0 * sums_[k];
      top[2 * k + 1] = centre[k];
    }
    line.advance(count);
  }
}

// Each pair (earlier first) becomes one sample: the even taps over the
// firsts and the centre's 1/2 on the second K + 1 pairs back. The top stage
// with antialiasing runs its even-length low-pass over the plain stream:
// its even taps over the firsts, its odd ones over the seconds a pair back.
void Resampler::down(double* top, std::span<double> out) noexcept {
  const std::size_t frames = out.size();
  if (frames == 0) {
    return;
  }
  const std::size_t total = frames * design_->factor;
  const std::size_t pad = design_->pad;
  if (pad > 0) {
    std::array<double, max_factor> newest{};
    std::copy_n(top + total - pad, pad, newest.begin());
    std::copy_backward(top, top + total - pad, top + total);
    std::copy_n(pad_.begin(), pad, top);
    pad_ = newest;
  }
  std::size_t count = total / 2;
  for (std::size_t s = design_->stages; s-- > 0; count /= 2) {
    const bool antialiased_top = s + 1 == design_->stages && design_->antialiased;
    Line& firsts_line = antialiased_top ? top_firsts_ : downs_[s].firsts;
    Line& seconds_line = antialiased_top ? top_seconds_ : downs_[s].seconds;
    double* const firsts = firsts_line.fresh();
    double* const seconds = seconds_line.fresh();
    for (std::size_t k = 0; k < count; ++k) {
      firsts[k] = top[2 * k];
      seconds[k] = top[2 * k + 1];
    }
    if (antialiased_top) {
      convolve_pairs(design_->top, firsts, seconds, count, top);
    } else {
      const auto& halfband = design_->halfbands[s];
      convolve(halfband.even, firsts, count, top);
      const double* const centre = seconds - (halfband.centre + 1);
      for (std::size_t k = 0; k < count; ++k) {
        top[k] += 0.5 * centre[k];
      }
    }
    firsts_line.advance(count);
    seconds_line.advance(count);
  }
  std::copy_n(top, frames, out.begin());
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
