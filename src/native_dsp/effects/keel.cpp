#include "oscillate/native_dsp/effects/keel.hpp"

#include "dsp/bs1770.hpp"
#include "dsp/fft.hpp"
#include "keel_presets.hpp"
#include "keel_table.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

namespace oscillate::native_dsp::effects::keel {
namespace {

namespace base = native_dsp::dsp;
namespace t = keel_table;
using Complex = std::complex<double>;
using t::index_of;

namespace p {
constexpr std::size_t amount = index_of("keel.amount");
constexpr std::size_t speed = index_of("keel.speed");
constexpr std::size_t max_boost = index_of("keel.max_boost");
constexpr std::size_t max_cut = index_of("keel.max_cut");
constexpr std::size_t target = index_of("keel.target");
constexpr std::size_t tilt = index_of("keel.tilt");
constexpr std::size_t low = index_of("keel.low");
constexpr std::size_t mid = index_of("keel.mid");
constexpr std::size_t high = index_of("keel.high");
constexpr std::size_t low_split = index_of("keel.low_split");
constexpr std::size_t high_split = index_of("keel.high_split");
constexpr std::size_t match = index_of("keel.match");
constexpr std::size_t output = index_of("keel.output");
constexpr std::size_t delta = index_of("keel.delta");
constexpr std::size_t first_curve = t::first_curve;
} // namespace p

static_assert(t::curve_count == bands);
// The tonal grid (ADR-0120) is Keel's less one band at each end.
static_assert(tonal::bands + 2 == bands);
static_assert(tonal::first_band_step + static_cast<int>(reference_band) == 1);
static_assert(telemetry::size <= max_telemetry_values);

constexpr std::size_t parameter_count = t::parameters.size();
using Values = std::array<double, parameter_count>;

constexpr double decibels_per_bel = 10.0;
constexpr double decibels_per_gain = 20.0;
constexpr double milliseconds_per_second = 1'000.0;

// Analysis frames at 48 kHz; they double with each doubling of the rate,
// so a band holds the same bins at any rate. The window resolves the
// 20 Hz band (17.5-22 Hz) in one and a half bins.
constexpr double base_rate = 48'000.0;
constexpr std::size_t max_scale = 4;
// A rate this far past a doubling takes the next scale.
constexpr double scale_step = 1.5;
constexpr std::size_t base_window = 16'384;
constexpr std::size_t base_hop = 2'048;
// The analysis finishes this many stretches of coefficient_frames before
// the hop ends.
constexpr std::size_t stretch_margin = 8;

// A third of an octave each way: the band edges are the centre times
// 2^(-1/6) and 2^(1/6).
constexpr double half_band_octaves = 0.5 / bands_per_octave;
// Spectral smoothing: each band with half of each neighbour's weight.
constexpr double smooth_centre = 0.5;
constexpr double smooth_side = 0.25;
// Bands this much under the K-weighted mean are absent; full presence
// returns over this span.
constexpr double presence_fade_db = 15.0;
// Analysis input is held within this (+24 dBFS), so one wild sample cannot
// own the estimate.
constexpr double analysis_limit = 16.0;
// What an empty band or an empty estimate reads.
constexpr double empty_db = base::silent_db;
constexpr double power_floor = 1e-30;

// The filter bank (the digital responses of the analogue prototypes at
// warped frequencies: a state-variable filter is that bilinear map).
enum class Kind : std::uint8_t { low_shelf, bell, high_shelf };
struct Design {
  Kind kind;
  double hz;
  double q;
};
constexpr double shelf_q = base::butterworth_q;
// Bells this wide (more than two octaves) overlap smoothly an octave
// apart: fitted to a 3 dB/oct tilt held within 18 dB, the bank ripples
// 0.25 dB from 40 Hz to 16 kHz, where narrower bells ripple more.
constexpr double bell_q = 0.6;
constexpr std::array<Design, filters> designs{{
    {Kind::low_shelf, 20.0, shelf_q},
    {Kind::bell, 31.25, bell_q},
    {Kind::bell, 62.5, bell_q},
    {Kind::bell, 125.0, bell_q},
    {Kind::bell, 250.0, bell_q},
    {Kind::bell, 500.0, bell_q},
    {Kind::bell, 1'000.0, bell_q},
    {Kind::bell, 2'000.0, bell_q},
    {Kind::bell, 4'000.0, bell_q},
    {Kind::bell, 8'000.0, bell_q},
    {Kind::high_shelf, 12'000.0, shelf_q},
}};
// The fit weighs the outermost bands (20 and 25 Hz, 20 kHz) at a quarter,
// so the shelves serve the bands inside them first.
constexpr std::array<std::size_t, 3> edge_bands{0, 1, bands - 1};
constexpr double edge_weight = 0.25;
// The least-squares fit: its ridge (per dB of gain, squared), how many
// refinements each hop takes from where the last left off, and the gain no
// filter passes.
constexpr double fit_ridge = 0.01;
constexpr std::size_t fit_iterations = 2;
constexpr double filter_limit_db = 24.0;

// Region weights cross over an octave, centred on each split.
constexpr double region_octaves = 1.0;

// Meters hold their peaks 1 s, falling 60 dB over it.
constexpr double hold_seconds = 1.0;
constexpr double hold_fall = 1e-3;
// A gain this close to where it glides to lands on it, so a correction at
// rest is exactly at rest.
constexpr double settle_db = 1e-9;
// Output glides over this.
constexpr double output_seconds = 0.01;
// After the input stops the shelves ring this long.
constexpr double tail_seconds = 0.25;

[[nodiscard]] bool valid_rate(double sample_rate) noexcept {
  return std::isfinite(sample_rate) && sample_rate > 0.0;
}

[[nodiscard]] double finite_or_zero(float x) noexcept {
  return std::isfinite(x) ? static_cast<double>(x) : 0.0;
}

[[nodiscard]] std::size_t scale_for(double rate) noexcept {
  std::size_t scale = 1;
  while (scale < max_scale && rate >= base_rate * static_cast<double>(scale) * scale_step) {
    scale *= 2;
  }
  return scale;
}

[[nodiscard]] double to_db(double power) noexcept {
  return base::power_to_db(power, empty_db, power_floor);
}

// The bilinear-transform squared magnitude of normalised biquad
// coefficients at `hz`.
[[nodiscard]] double biquad_power(const base::BiquadCoefficients& c, double hz,
                                  double rate) noexcept {
  const Complex z = std::polar(1.0, -2.0 * std::numbers::pi * hz / rate);
  const Complex num = c.b0 + c.b1 * z + c.b2 * z * z;
  const Complex den = 1.0 + c.a1 * z + c.a2 * z * z;
  return std::norm(num / den);
}

// A filter's squared magnitude at warped frequency x (the band's warped
// frequency over the filter's), with a = 10^(gain / 40) and, for the
// shelves, k = sqrt(a) / q.
[[nodiscard]] double power_ratio(Kind kind, double q, double x, double a, double k) noexcept {
  const double x2 = x * x;
  switch (kind) {
  case Kind::bell: {
    const double re = (1.0 - x2) * (1.0 - x2);
    return (re + (x * a / q) * (x * a / q)) / (re + (x / (a * q)) * (x / (a * q)));
  }
  case Kind::low_shelf: {
    const double nr = a - x2;
    const double dr = 1.0 - a * x2;
    return a * a * (nr * nr + (x * k) * (x * k)) / (dr * dr + (x * k) * (x * k));
  }
  case Kind::high_shelf: {
    const double nr = 1.0 - a * x2;
    const double dr = a - x2;
    return a * a * (nr * nr + (x * k) * (x * k)) / (dr * dr + (x * k) * (x * k));
  }
  }
  return 1.0;
}

// A filter's response in dB at warped frequency x at gain_db.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named at every call.
[[nodiscard]] double response_db(Kind kind, double q, double x, double gain_db) noexcept {
  if (gain_db == 0.0) {
    return 0.0;
  }
  const double a = std::pow(10.0, gain_db / (2.0 * decibels_per_gain));
  return decibels_per_bel * std::log10(power_ratio(kind, q, x, a, std::sqrt(a) / q));
}

// One trapezoidal state-variable filter (A. Simper's form): at 0 dB its
// output is its input exactly.
struct Svf {
  double a1 = 1.0;
  double a2 = 0.0;
  double a3 = 0.0;
  double m0 = 1.0;
  double m1 = 0.0;
  double m2 = 0.0;

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named at every call.
  void set(Kind kind, double warped, double q, double gain_db) noexcept {
    const double a = std::pow(10.0, gain_db / (2.0 * decibels_per_gain));
    double g = warped;
    double k = 1.0 / q;
    switch (kind) {
    case Kind::bell:
      k = 1.0 / (q * a);
      m0 = 1.0;
      m1 = k * (a * a - 1.0);
      m2 = 0.0;
      break;
    case Kind::low_shelf:
      g = warped / std::sqrt(a);
      m0 = 1.0;
      m1 = k * (a - 1.0);
      m2 = a * a - 1.0;
      break;
    case Kind::high_shelf:
      g = warped * std::sqrt(a);
      m0 = a * a;
      m1 = k * (1.0 - a) * a;
      m2 = 1.0 - a * a;
      break;
    }
    a1 = 1.0 / (1.0 + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
  }
  // The per-frame step from this filter to `to` over `frames`.
  [[nodiscard]] Svf step_to(const Svf& to, double frames) const noexcept {
    return {(to.a1 - a1) / frames, (to.a2 - a2) / frames, (to.a3 - a3) / frames,
            (to.m0 - m0) / frames, (to.m1 - m1) / frames, (to.m2 - m2) / frames};
  }
  void advance(const Svf& step) noexcept {
    a1 += step.a1;
    a2 += step.a2;
    a3 += step.a3;
    m0 += step.m0;
    m1 += step.m1;
    m2 += step.m2;
  }
};

struct SvfState {
  double ic1 = 0.0;
  double ic2 = 0.0;
  [[nodiscard]] double process(const Svf& f, double v0) noexcept {
    const double v3 = v0 - ic2;
    const double v1 = f.a1 * ic1 + f.a2 * v3;
    const double v2 = ic2 + f.a2 * ic1 + f.a3 * v3;
    ic1 = 2.0 * v1 - ic1;
    ic2 = 2.0 * v2 - ic2;
    return f.m0 * v0 + f.m1 * v1 + f.m2 * v2;
  }
};

// A long-term average of band powers over Speed seconds, gated by the
// programme's own recent loudness.
class Tracker {
public:
  void reset() noexcept {
    power_.fill(0.0);
    loudness_ = 0.0;
    heard_ = 0;
    accepted_ = 0;
  }
  // One hop's band powers; `weights` K-weight them into a loudness.
  void update(std::span<double, bands> frame, std::span<const double, bands> weights,
              double coefficient) noexcept {
    double loud = 0.0;
    for (std::size_t b = 0; b < bands; ++b) {
      loud += weights[b] * frame[b];
    }
    if (to_db(loud) < absolute_gate_db) {
      return;
    }
    ++heard_;
    loudness_ += std::max(coefficient, 1.0 / static_cast<double>(heard_)) * (loud - loudness_);
    const double recent_db = to_db(loudness_);
    const double frame_db = to_db(loud);
    if (frame_db < recent_db - relative_gate_db) {
      return;
    }
    const double over = frame_db - (recent_db + transient_db);
    const double scale = over > 0.0 ? std::pow(10.0, -over / decibels_per_bel) : 1.0;
    ++accepted_;
    const double step = std::max(coefficient, 1.0 / static_cast<double>(accepted_));
    for (std::size_t b = 0; b < bands; ++b) {
      power_[b] += step * (frame[b] * scale - power_[b]);
    }
  }
  [[nodiscard]] bool tracked() const noexcept { return accepted_ > 0; }
  [[nodiscard]] const std::array<double, bands>& power() const noexcept { return power_; }

private:
  std::array<double, bands> power_{};
  double loudness_ = 0.0;
  std::uint64_t heard_ = 0;
  std::uint64_t accepted_ = 0;
};

class KeelKernel final : public EffectKernel {
public:
  KeelKernel(std::span<const double> values, double rate)
      : rate_(rate), window_size_(base_window * scale_for(rate)), hop_(base_hop * scale_for(rate)),
        ring_size_(2 * window_size_), fft_(window_size_),
        hold_(std::exp(std::log(hold_fall) / (hold_seconds * rate))),
        output_step_(1.0 - std::exp(-1.0 / (output_seconds * rate))),
        glide_step_(1.0 -
                    std::exp(-static_cast<double>(coefficient_frames) / static_cast<double>(hop_))),
        slice_frames_(std::max<std::size_t>(
            1, static_cast<std::size_t>(std::lround(history_seconds * rate)))) {
    std::ranges::copy(values, values_.begin());
    window_.resize(window_size_);
    double energy = 0.0;
    for (std::size_t i = 0; i < window_size_; ++i) {
      window_[i] = base::hann(static_cast<double>(i), static_cast<double>(window_size_));
      energy += window_[i] * window_[i];
    }
    // Both halves of the spectrum, over the window's energy: a band's
    // reading is the signal's mean-square power within it.
    power_scale_ = 2.0 / (static_cast<double>(window_size_) * energy);
    // The rings hold a window and the hop after it, so the analysis of a
    // window can run through the next hop while new samples arrive.
    for (auto& ring : ring_) {
      ring.assign(ring_size_, 0.0);
    }
    for (auto& ring : key_ring_) {
      ring.assign(ring_size_, 0.0);
    }
    for (auto& job : jobs_) {
      job.data.resize(window_size_);
    }
    // Each analysis's units (see advance), spread over the hop's stretches.
    const std::size_t per_analysis = window_size_ * 2 + fft_.stages() * (window_size_ / 2);
    const std::size_t stretches = hop_ / coefficient_frames - stretch_margin;
    units_per_analysis_ = (per_analysis + stretches - 1) / stretches;
    prepare_bands();
    prepare_filters();
    reset_state();
  }

  void control(std::span<const double> v, std::uint32_t, const Transport&) noexcept override {
    std::ranges::copy(v, values_.begin());
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame> sidechain) noexcept override {
    keyed_ = !sidechain.empty();
    const double output_target = base::db_to_gain(values_[p::output]);
    const bool delta = values_[p::delta] != 0.0;
    for (std::size_t i = 0; i < audio.size(); ++i) {
      const std::array<double, 2> in{finite_or_zero(audio[i].left), finite_or_zero(audio[i].right)};
      const std::size_t at = written_ & (ring_size_ - 1);
      for (std::size_t c = 0; c < 2; ++c) {
        input_peak_[c] = std::max(input_peak_[c] * hold_, std::fabs(in[c]));
        ring_[c][at] = std::clamp(in[c], -analysis_limit, analysis_limit);
      }
      if (keyed_) {
        key_ring_[0][at] =
            std::clamp(finite_or_zero(sidechain[i].left), -analysis_limit, analysis_limit);
        key_ring_[1][at] =
            std::clamp(finite_or_zero(sidechain[i].right), -analysis_limit, analysis_limit);
      }
      ++written_;
      if (position_ % coefficient_frames == 0) {
        update_coefficients();
        advance(units_per_stretch_);
      }
      const double u = static_cast<double>(position_) / static_cast<double>(hop_);
      const double match = match_from_ + (match_to_ - match_from_) * u;
      output_ += (output_target - output_) * output_step_;
      const double gain = output_ * match;
      for (std::size_t c = 0; c < 2; ++c) {
        double y = in[c];
        for (std::size_t j = 0; j < filters; ++j) {
          y = state_[c][j].process(svf_[j], y);
        }
        if (delta) {
          y -= in[c];
        }
        y *= gain;
        output_peak_[c] = std::max(output_peak_[c] * hold_, std::fabs(y));
        if (c == 0) {
          audio[i].left = static_cast<float>(y);
        } else {
          audio[i].right = static_cast<float>(y);
        }
      }
      if (gliding_) {
        for (std::size_t j = 0; j < filters; ++j) {
          svf_[j].advance(svf_step_[j]);
        }
      }
      if (++position_ == hop_) {
        position_ = 0;
        hop();
      }
    }
  }

  void reset() noexcept override { reset_state(); }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    namespace tm = telemetry;
    if (out.size() < tm::size) {
      return 0;
    }
    for (std::size_t c = 0; c < 2; ++c) {
      out[tm::input_peak + c] = static_cast<float>(base::gain_to_db(input_peak_[c]));
      out[tm::output_peak + c] = static_cast<float>(base::gain_to_db(output_peak_[c]));
    }
    out[tm::latency_ms] = 0.0F;
    out[tm::match_db] = static_cast<float>(base::gain_to_db(match_to_));
    out[tm::tracked] = input_.tracked() ? 1.0F : 0.0F;
    out[tm::referenced] = reference_.tracked() ? 1.0F : 0.0F;
    out[tm::target_source] = static_cast<float>(source_);
    for (std::size_t b = 0; b < bands; ++b) {
      out[tm::target + b] = static_cast<float>(shown_target_[b]);
      out[tm::spectrum + b] = static_cast<float>(shown_spectrum_[b]);
      out[tm::correction + b] = static_cast<float>(realised_[b]);
    }
    for (std::size_t s = 0; s < history_slices; ++s) {
      const auto& slice = history_[(history_head_ + s) % history_slices];
      for (std::size_t b = 0; b < bands; ++b) {
        out[tm::history + s * bands + b] = slice[b];
      }
    }
    return tm::size;
  }

private:
  // One window's transform and its band powers.
  struct Analysis {
    std::vector<Complex> data;
    std::array<double, bands> frame{};
  };

  // Per band: the bins it sums (each weighted by how much of it lies inside
  // the band), its K-weighting and whether it is under the Nyquist guard.
  void prepare_bands() {
    const double bin_hz = rate_ / static_cast<double>(window_size_);
    const double guard = base::nyquist_guard * rate_;
    const auto shelf = base::bs1770::k_shelf(rate_);
    const auto high_pass = base::bs1770::k_high_pass(rate_);
    for (std::size_t b = 0; b < bands; ++b) {
      const double centre = band_hz(b);
      live_[b] = centre < guard;
      k_weight_[b] =
          live_[b] ? biquad_power(shelf, centre, rate_) * biquad_power(high_pass, centre, rate_)
                   : 0.0;
      const double lo = centre * std::exp2(-half_band_octaves) / bin_hz;
      const double hi = std::min(centre * std::exp2(half_band_octaves) / bin_hz,
                                 static_cast<double>(window_size_) / 2.0 - 1.0);
      auto& range = bins_[b];
      range.first = static_cast<std::size_t>(std::max(1.0, std::floor(lo + 0.5)));
      range.last = static_cast<std::size_t>(std::max(1.0, std::floor(hi + 0.5)));
      range.first_weight = std::min(1.0, static_cast<double>(range.first) + 0.5 - lo);
      range.last_weight = std::min(1.0, hi - (static_cast<double>(range.last) - 0.5));
      if (range.first == range.last) {
        range.first_weight = std::max(0.0, hi - lo);
      }
    }
  }

  // The bank's warped corners, its responses on the bands at 1 dB, and the
  // weighted least-squares solve (A^T W A + ridge)^-1 A^T W over the live
  // bands.
  void prepare_filters() {
    const double guard = base::nyquist_guard * rate_;
    for (std::size_t j = 0; j < filters; ++j) {
      const double hz = std::min(designs[j].hz, guard);
      warped_[j] = std::tan(std::numbers::pi * hz / rate_);
      for (std::size_t b = 0; b < bands; ++b) {
        x_[j][b] = live_[b] ? std::tan(std::numbers::pi * band_hz(b) / rate_) / warped_[j] : 0.0;
      }
    }
    std::array<std::array<double, filters>, bands> basis{};
    std::array<double, bands> weight{};
    for (std::size_t b = 0; b < bands; ++b) {
      weight[b] = live_[b] ? 1.0 : 0.0;
    }
    for (const auto b : edge_bands) {
      weight[b] *= edge_weight;
    }
    for (std::size_t b = 0; b < bands; ++b) {
      for (std::size_t j = 0; j < filters; ++j) {
        basis[b][j] = live_[b] ? response_db(designs[j].kind, designs[j].q, x_[j][b], 1.0) : 0.0;
      }
    }
    // Gauss-Jordan on [A^T W A + ridge | A^T W].
    std::vector<std::vector<double>> m(filters, std::vector<double>(filters + bands, 0.0));
    for (std::size_t r = 0; r < filters; ++r) {
      for (std::size_t c = 0; c < filters; ++c) {
        double sum = r == c ? fit_ridge : 0.0;
        for (std::size_t b = 0; b < bands; ++b) {
          sum += weight[b] * basis[b][r] * basis[b][c];
        }
        m[r][c] = sum;
      }
      for (std::size_t b = 0; b < bands; ++b) {
        m[r][filters + b] = weight[b] * basis[b][r];
      }
    }
    for (std::size_t pivot = 0; pivot < filters; ++pivot) {
      const double d = m[pivot][pivot];
      for (auto& x : m[pivot]) {
        x /= d;
      }
      for (std::size_t r = 0; r < filters; ++r) {
        if (r == pivot) {
          continue;
        }
        const double f = m[r][pivot];
        for (std::size_t c = 0; c < filters + bands; ++c) {
          m[r][c] -= f * m[pivot][c];
        }
      }
    }
    for (std::size_t j = 0; j < filters; ++j) {
      for (std::size_t b = 0; b < bands; ++b) {
        solve_[j][b] = m[j][filters + b];
      }
    }
  }

  void reset_state() noexcept {
    for (auto& ring : ring_) {
      std::ranges::fill(ring, 0.0);
    }
    for (auto& ring : key_ring_) {
      std::ranges::fill(ring, 0.0);
    }
    for (auto& channel : state_) {
      channel.fill({});
    }
    input_.reset();
    reference_.reset();
    gain_from_.fill(0.0);
    gain_to_.fill(0.0);
    gain_now_.fill(0.0);
    next_.fill(0.0);
    realised_.fill(0.0);
    shown_target_.fill(empty_db);
    shown_spectrum_.fill(empty_db);
    for (auto& slice : history_) {
      slice.fill(0.0F);
    }
    history_head_ = 0;
    slice_elapsed_ = 0;
    match_from_ = 1.0;
    match_to_ = 1.0;
    output_ = base::db_to_gain(values_[p::output]);
    input_peak_ = {};
    output_peak_ = {};
    position_ = 0;
    written_ = 0;
    job_active_ = false;
    job_ready_ = false;
    source_ = target_source();
    moving_ = true;
    gliding_ = false;
    svf_.fill({});
    svf_step_.fill({0.0, 0.0, 0.0, 0.0, 0.0, 0.0});
  }

  [[nodiscard]] t::Target target_source() const noexcept {
    const auto index = static_cast<std::size_t>(std::max(0.0, values_[p::target]));
    return static_cast<t::Target>(std::min(index, t::targets.size() - 1));
  }

  // The gains at the end of this stretch of coefficient_frames, smoothed
  // over a hop (so the ramps' corners at each hop do not modulate the audio
  // at the hop rate), and the filters' per-frame steps toward them: the
  // coefficients move every frame, so nothing zips.
  void update_coefficients() noexcept {
    if (!moving_) {
      return;
    }
    const double u =
        static_cast<double>(position_ + coefficient_frames) / static_cast<double>(hop_);
    bool settled = gain_from_ == gain_to_;
    for (std::size_t j = 0; j < filters; ++j) {
      const double ramp = gain_from_[j] + (gain_to_[j] - gain_from_[j]) * u;
      double& g = gain_now_[j];
      g += glide_step_ * (ramp - g);
      if (std::fabs(ramp - g) < settle_db) {
        g = ramp;
      }
      settled = settled && g == gain_to_[j];
      targets_[j].set(designs[j].kind, warped_[j], designs[j].q, g);
    }
    for (std::size_t j = 0; j < filters; ++j) {
      if (settled) {
        svf_[j] = targets_[j];
        svf_step_[j] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
      } else {
        svf_step_[j] = svf_[j].step_to(targets_[j], static_cast<double>(coefficient_frames));
      }
    }
    gliding_ = !settled;
    // Once the gains have landed on the hop's, they hold.
    if (settled) {
      moving_ = false;
    }
  }

  // ---- analysis --------------------------------------------------------------------------

  // The analysis of the window that ended at the last hop runs through the
  // hop after it, a share at every stretch of coefficient_frames, so no
  // callback carries a whole transform: the input's window (and the
  // sidechain's when keyed) is windowed, transformed stage by stage and
  // summed into band powers (both channels' mean), ready by the next hop.
  void start_analysis() noexcept {
    job_step_ = 0;
    job_done_ = 0;
    job_start_ = written_ - window_size_;
    job_keyed_ = keyed_;
    job_active_ = true;
    units_per_stretch_ = units_per_analysis_ * (job_keyed_ ? jobs_.size() : 1);
  }

  // Runs `units` of the analysis: one unit is one sample windowed, one
  // index reordered or one butterfly per transform; the band sums run
  // whole.
  void advance(std::size_t units) noexcept {
    const std::size_t count = job_keyed_ ? jobs_.size() : 1;
    const std::size_t n = window_size_;
    const std::size_t mask = ring_size_ - 1;
    const std::size_t stages = fft_.stages();
    const std::size_t sum_step = stages + 2;
    while (job_active_ && units > 0) {
      if (job_step_ == 0) {
        const std::size_t take = std::min(n - job_done_, std::max<std::size_t>(units / count, 1));
        for (std::size_t j = 0; j < count; ++j) {
          const auto& ring = j == 0 ? ring_ : key_ring_;
          auto& data = jobs_[j].data;
          for (std::size_t i = job_done_; i < job_done_ + take; ++i) {
            const std::size_t at = (job_start_ + i) & mask;
            data[i] = Complex(ring[0][at] * window_[i], ring[1][at] * window_[i]);
          }
        }
        job_done_ += take;
        units -= std::min(units, take * count);
      } else if (job_step_ == 1) {
        const std::size_t take = std::min(n - job_done_, std::max<std::size_t>(units / count, 1));
        for (std::size_t j = 0; j < count; ++j) {
          fft_.reorder(jobs_[j].data, job_done_, job_done_ + take);
        }
        job_done_ += take;
        units -= std::min(units, take * count);
      } else if (job_step_ < sum_step) {
        const std::size_t butterflies = n / 2;
        const std::size_t take =
            std::min(butterflies - job_done_, std::max<std::size_t>(units / count, 1));
        for (std::size_t j = 0; j < count; ++j) {
          fft_.stage(jobs_[j].data, job_step_ - 2, job_done_, job_done_ + take);
        }
        job_done_ += take;
        units -= std::min(units, take * count);
        if (job_done_ == butterflies) {
          job_done_ = n;
        }
      } else {
        for (std::size_t j = 0; j < count; ++j) {
          sum_bands(jobs_[j]);
        }
        job_active_ = false;
        job_ready_ = true;
        units = 0;
      }
      if (job_active_ && job_done_ == n) {
        ++job_step_;
        job_done_ = 0;
      }
    }
  }

  // A transformed window's band powers.
  void sum_bands(Analysis& job) const noexcept {
    const std::size_t n = window_size_;
    const auto& z = job.data;
    const auto bin_power = [&](std::size_t k) {
      const Complex zk = z[k];
      const Complex zm = std::conj(z[n - k]);
      const Complex a = 0.5 * (zk + zm);
      const Complex b = Complex(0.0, -0.5) * (zk - zm);
      return 0.5 * (std::norm(a) + std::norm(b)) * power_scale_;
    };
    for (std::size_t b = 0; b < bands; ++b) {
      const auto& range = bins_[b];
      double sum = range.first_weight * bin_power(range.first);
      if (range.last > range.first) {
        for (std::size_t k = range.first + 1; k < range.last; ++k) {
          sum += bin_power(k);
        }
        sum += range.last_weight * bin_power(range.last);
      }
      job.frame[b] = live_[b] ? sum : 0.0;
    }
  }

  // A tracker's balance: smoothed across bands, in dB.
  [[nodiscard]] std::array<double, bands> balance(const Tracker& tracker) const noexcept {
    return smoothed(tracker.power());
  }

  // Band powers smoothed a third of an octave each way, in dB.
  [[nodiscard]] std::array<double, bands>
  smoothed(const std::array<double, bands>& power) const noexcept {
    std::array<double, bands> out{};
    for (std::size_t b = 0; b < bands; ++b) {
      double sum = smooth_centre * power[b];
      double weight = smooth_centre;
      if (b > 0 && live_[b - 1]) {
        sum += smooth_side * power[b - 1];
        weight += smooth_side;
      }
      if (b + 1 < bands && live_[b + 1]) {
        sum += smooth_side * power[b + 1];
        weight += smooth_side;
      }
      out[b] = live_[b] ? to_db(sum / weight) : empty_db;
    }
    return out;
  }

  // The curve less its mean, K-weighted over the bands `presence` holds.
  void centre(std::array<double, bands>& curve,
              const std::array<double, bands>& presence) const noexcept {
    double sum = 0.0;
    double weight = 0.0;
    for (std::size_t b = 0; b < bands; ++b) {
      sum += k_weight_[b] * presence[b] * curve[b];
      weight += k_weight_[b] * presence[b];
    }
    const double mean = weight > 0.0 ? sum / weight : 0.0;
    for (std::size_t b = 0; b < bands; ++b) {
      curve[b] = live_[b] ? curve[b] - mean : empty_db;
    }
  }

  // Each band's region weight: Low, Mid and High crossing over an octave
  // about each split.
  [[nodiscard]] double region(double hz) const noexcept {
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named at every call.
    const auto blend = [](double octaves_past, double below, double above) {
      const double u = std::clamp(octaves_past / region_octaves + 0.5, 0.0, 1.0);
      const double k = 0.5 - 0.5 * std::cos(std::numbers::pi * u);
      return below + (above - below) * k;
    };
    const double low_to_mid =
        blend(std::log2(hz / values_[p::low_split]), values_[p::low], values_[p::mid]);
    return blend(std::log2(hz / values_[p::high_split]), low_to_mid, values_[p::high]);
  }

  // The correction (dB per band) toward the target.
  void correct(std::array<double, bands>& wanted) noexcept {
    wanted.fill(0.0);
    source_ = target_source();
    if (!input_.tracked()) {
      return;
    }
    // The input's balance, less its mean over the bands that hold content:
    // those within absent_db of its K-weighted power mean (which the loud
    // bands set, not the empty ones).
    auto spectrum = balance(input_);
    double power = 0.0;
    double weight = 0.0;
    for (std::size_t b = 0; b < bands; ++b) {
      if (live_[b]) {
        power += k_weight_[b] * std::pow(10.0, spectrum[b] / decibels_per_bel);
        weight += k_weight_[b];
      }
    }
    const double level = to_db(weight > 0.0 ? power / weight : 0.0);
    std::array<double, bands> presence{};
    for (std::size_t b = 0; b < bands; ++b) {
      presence[b] = live_[b]
                        ? std::clamp((spectrum[b] - level + absent_db) / presence_fade_db, 0.0, 1.0)
                        : 0.0;
    }
    centre(spectrum, presence);
    shown_spectrum_ = spectrum;
    // The target, smoothed as the balance is (so a steep curve is compared
    // like with like) and centred over the same bands.
    std::array<double, bands> drawn_power{};
    const auto drawn = target_curve(values_);
    for (std::size_t b = 0; b < bands; ++b) {
      drawn_power[b] = std::pow(10.0, drawn[b] / decibels_per_bel);
    }
    auto target = smoothed(drawn_power);
    if (source_ == t::Target::reference) {
      if (!reference_.tracked()) {
        shown_target_.fill(empty_db);
        return;
      }
      const auto heard = balance(reference_);
      for (std::size_t b = 0; b < bands; ++b) {
        target[b] += heard[b];
      }
    }
    centre(target, presence);
    shown_target_ = target;
    const double amount = values_[p::amount];
    const double boost = values_[p::max_boost];
    const double cut = values_[p::max_cut];
    for (std::size_t b = 0; b < bands; ++b) {
      if (!live_[b]) {
        continue;
      }
      const double want = -amount * region(band_hz(b)) * (spectrum[b] - target[b]);
      wanted[b] = std::clamp(want, -cut, boost * presence[b]);
    }
  }

  // The bank's response (dB) on the bands at these gains: the product of
  // the filters' power ratios, one logarithm per band.
  void respond(const std::array<double, filters>& gains,
               std::array<double, bands>& out) const noexcept {
    std::array<double, filters> a{};
    std::array<double, filters> shelf_k{};
    for (std::size_t j = 0; j < filters; ++j) {
      a[j] = std::pow(10.0, gains[j] / (2.0 * decibels_per_gain));
      shelf_k[j] = std::sqrt(a[j]) / designs[j].q;
    }
    for (std::size_t b = 0; b < bands; ++b) {
      if (!live_[b]) {
        out[b] = 0.0;
        continue;
      }
      double ratio = 1.0;
      for (std::size_t j = 0; j < filters; ++j) {
        if (gains[j] != 0.0) {
          ratio *= power_ratio(designs[j].kind, designs[j].q, x_[j][b], a[j], shelf_k[j]);
        }
      }
      out[b] = ratio == 1.0 ? 0.0 : decibels_per_bel * std::log10(ratio);
    }
  }

  // The gains whose response fits `wanted`, refined from the last hop's.
  void fit(const std::array<double, bands>& wanted) noexcept {
    if (std::ranges::all_of(wanted, [](double x) { return x == 0.0; })) {
      next_.fill(0.0);
      return;
    }
    std::array<double, bands> response{};
    for (std::size_t iteration = 0; iteration < fit_iterations; ++iteration) {
      respond(next_, response);
      for (std::size_t j = 0; j < filters; ++j) {
        double step = 0.0;
        for (std::size_t b = 0; b < bands; ++b) {
          step += solve_[j][b] * (wanted[b] - response[b]);
        }
        next_[j] = std::clamp(next_[j] + step, -filter_limit_db, filter_limit_db);
      }
    }
  }

  // Level Match: the gain that takes out the loudness the response adds
  // to the tracked spectrum.
  [[nodiscard]] double match_gain() const noexcept {
    if (values_[p::match] == 0.0 || !input_.tracked()) {
      return 1.0;
    }
    const auto& power = input_.power();
    double before = 0.0;
    double after = 0.0;
    for (std::size_t b = 0; b < bands; ++b) {
      before += k_weight_[b] * power[b];
      after += k_weight_[b] * power[b] * std::pow(10.0, realised_[b] / decibels_per_bel);
    }
    if (!(before > 0.0) || !(after > 0.0)) {
      return 1.0;
    }
    return std::sqrt(before / after);
  }

  void hop() noexcept {
    // The last hop's window, finished if a stretch was left; then the
    // next. Until the window has filled since the start or a seek, its
    // zeros would read as a fade-in: the trackers wait for it.
    advance(std::numeric_limits<std::size_t>::max());
    if (job_ready_) {
      const double coefficient =
          1.0 - std::exp(-static_cast<double>(hop_) / (values_[p::speed] * rate_));
      input_.update(jobs_[0].frame, k_weight_, coefficient);
      if (job_keyed_) {
        reference_.update(jobs_[1].frame, k_weight_, coefficient);
      }
      job_ready_ = false;
    }
    if (written_ >= window_size_) {
      start_analysis();
    }
    std::array<double, bands> wanted{};
    correct(wanted);
    fit(wanted);
    gain_from_ = gain_to_;
    gain_to_ = next_;
    moving_ = true;
    respond(gain_to_, realised_);
    match_from_ = match_to_;
    match_to_ = match_gain();
    slice_elapsed_ += hop_;
    while (slice_elapsed_ >= slice_frames_) {
      slice_elapsed_ -= slice_frames_;
      auto& slice = history_[history_head_];
      for (std::size_t b = 0; b < bands; ++b) {
        slice[b] = static_cast<float>(realised_[b]);
      }
      history_head_ = (history_head_ + 1) % history_slices;
    }
  }

  struct BinRange {
    std::size_t first = 1;
    std::size_t last = 1;
    double first_weight = 0.0;
    double last_weight = 0.0;
  };

  double rate_;
  std::size_t window_size_;
  std::size_t hop_;
  std::size_t ring_size_;
  base::Fft fft_;
  double hold_;
  double output_step_;
  double glide_step_;
  std::size_t slice_frames_;
  Values values_{};

  std::vector<double> window_;
  double power_scale_ = 1.0;
  std::array<std::vector<double>, 2> ring_;
  std::array<std::vector<double>, 2> key_ring_;
  // The input's analysis, then the sidechain's.
  std::array<Analysis, 2> jobs_;
  std::size_t units_per_analysis_ = 0;
  std::size_t units_per_stretch_ = 0;
  std::size_t job_step_ = 0;
  std::size_t job_done_ = 0;
  std::size_t job_start_ = 0;
  bool job_keyed_ = false;
  bool job_active_ = false;
  bool job_ready_ = false;
  std::array<BinRange, bands> bins_{};
  std::array<bool, bands> live_{};
  std::array<double, bands> k_weight_{};

  std::array<double, filters> warped_{};
  std::array<std::array<double, bands>, filters> x_{};
  std::array<std::array<double, bands>, filters> solve_{};
  std::array<Svf, filters> svf_{};
  std::array<Svf, filters> svf_step_{};
  std::array<Svf, filters> targets_{};
  std::array<std::array<SvfState, filters>, 2> state_{};

  Tracker input_;
  Tracker reference_;
  std::array<double, filters> gain_from_{};
  std::array<double, filters> gain_to_{};
  std::array<double, filters> next_{};
  std::array<double, filters> gain_now_{};
  std::array<double, bands> realised_{};
  std::array<double, bands> shown_target_{};
  std::array<double, bands> shown_spectrum_{};
  std::array<std::array<float, bands>, history_slices> history_{};
  std::size_t history_head_ = 0;
  std::size_t slice_elapsed_ = 0;
  double match_from_ = 1.0;
  double match_to_ = 1.0;
  double output_ = 1.0;
  std::array<double, 2> input_peak_{};
  std::array<double, 2> output_peak_{};
  std::size_t position_ = 0;
  std::size_t written_ = 0;
  t::Target source_ = t::Target::tilt;
  bool keyed_ = false;
  bool moving_ = true;
  bool gliding_ = false;
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double> values, double sample_rate) {
  if (values.size() != parameter_count || !valid_rate(sample_rate)) {
    return nullptr;
  }
  return std::make_unique<KeelKernel>(values, sample_rate);
}

std::uint64_t tail_frames(std::span<const double>, double sample_rate) {
  return valid_rate(sample_rate) ? static_cast<std::uint64_t>(std::ceil(tail_seconds * sample_rate))
                                 : 0;
}

std::uint32_t latency_frames(std::span<const double>, double) { return 0; }

const EngineDescriptor descriptor{"keel", "Keel", 1, t::parameters, keel_presets::presets};
const EffectEngine keel_engine{&descriptor, &create_kernel, &tail_frames, &latency_frames, true};

} // namespace

double band_hz(std::size_t band) noexcept {
  return reference_hz *
         std::exp2((static_cast<double>(band) - static_cast<double>(reference_band)) /
                   bands_per_octave);
}

Curve target_curve(std::span<const double> values) noexcept {
  Curve out{};
  if (values.size() != parameter_count) {
    return out;
  }
  const bool drawn = static_cast<std::size_t>(std::max(0.0, values[p::target])) == t::Target::curve;
  for (std::size_t b = 0; b < bands; ++b) {
    out[b] = values[p::tilt] * std::log2(band_hz(b) / reference_hz) +
             (drawn ? values[p::first_curve + b] : 0.0);
  }
  return out;
}

Curve curve_from(const tonal::Curve& levels) noexcept {
  Curve out{};
  for (std::size_t b = 0; b < tonal::bands; ++b) {
    out[b + 1] = levels[b];
  }
  out.front() = levels.front();
  out.back() = levels.back();
  // Only the shape counts: centred, so the curve's range holds it.
  double mean = 0.0;
  for (const double db : out) {
    mean += db;
  }
  mean /= static_cast<double>(bands);
  for (double& db : out) {
    db -= mean;
  }
  return out;
}

void set_curve(std::span<double> values, const Curve& curve) noexcept {
  if (values.size() != parameter_count) {
    return;
  }
  for (std::size_t b = 0; b < bands; ++b) {
    values[p::first_curve + b] = std::clamp(curve[b], -t::curve_limit, t::curve_limit);
  }
}

Curve learned_curve(std::span<const float> telemetry, std::span<const double> values) noexcept {
  Curve out{};
  if (telemetry.size() < telemetry::size || values.size() != parameter_count ||
      telemetry[telemetry::tracked] == 0.0F) {
    return out;
  }
  for (std::size_t b = 0; b < bands; ++b) {
    const double heard = telemetry[telemetry::spectrum + b];
    const double tilt = values[p::tilt] * std::log2(band_hz(b) / reference_hz);
    out[b] = heard <= empty_db ? 0.0 : std::clamp(heard - tilt, -t::curve_limit, t::curve_limit);
  }
  return out;
}

const EffectEngine& engine() { return keel_engine; }

} // namespace oscillate::native_dsp::effects::keel
