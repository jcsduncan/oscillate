#include "oscillate/native_dsp/effects/gild.hpp"

#include "dsp/crossover.hpp"
#include "dsp/fft.hpp"
#include "edge_dsp.hpp"
#include "edge_table.hpp"
#include "effect_dsp.hpp"
#include "forge_curves.hpp"
#include "gild_presets.hpp"
#include "gild_table.hpp"
#include "linear_bank.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <type_traits>
#include <vector>

namespace oscillate::native_dsp::effects::gild {
namespace {

namespace base = native_dsp::dsp;
namespace curves = forge_curves;
namespace t = gild_table;
using base::db_to_gain;
using forge_table::Model;
using gild_table::index_of;

namespace p {
constexpr std::size_t bands = index_of("gild.bands");
constexpr std::size_t xover_1 = index_of("gild.xover_1");
constexpr std::size_t phase = index_of("gild.phase");
constexpr std::size_t oversampling = index_of("gild.oversampling");
constexpr std::size_t dynamic = index_of("gild.dynamic");
constexpr std::size_t match = index_of("gild.match");
constexpr std::size_t delta = index_of("gild.delta");
constexpr std::size_t output = index_of("gild.output");
constexpr std::size_t first_band = t::first_band;
} // namespace p

static_assert(t::band_count == max_bands);
static_assert(t::crossover_defaults.size() == max_crossovers);
static_assert(max_bands <= dsp::LinearBank::max_bands);
static_assert(telemetry::size <= max_telemetry_values);
static_assert(t::oversampling.size() == edge_dsp::max_stages + 1);

constexpr double milliseconds_per_second = 1'000.0;

// Under this drive gain a band adds nothing: the residue there is under
// -80 dB of the band, and its rounding would no longer be.
constexpr double least_drive = 0.01;
// The filters Gild oversamples through: Edge's linear-phase half-bands,
// whose 120 dB keep the top factor's aliases under -100 dBFS.
constexpr std::size_t oversampling_filters = edge_table::linear_phase;

// Added to both powers before Level Match takes their ratio (about
// -200 dBFS), so silence reads unity.
constexpr double power_floor = 1e-20;
// A follower or blocker state under this is taken as silent (it never
// decays into denormals).
constexpr double silent = 1e-30;

// The residue's DC blocker: Even's bias leaves an offset.
constexpr double dc_hz = 5.0;
// The blocker falls from full scale to the tail's -120 dBFS in this many
// time constants (ln 10^6).
constexpr double tail_time_constants = 13.8;

// Crossovers stay a third of an octave apart and under the Nyquist guard.
const double crossover_spacing = std::cbrt(2.0);

// After the input stops the crossovers ring this long.
constexpr double crossover_tail_seconds = 0.05;

// Meters hold their peaks 1 s, falling 60 dB over it.
constexpr double hold_seconds = 1.0;
constexpr double hold_fall = 1e-3;
constexpr double reading_floor_db = -120.0;

// The spectrum's Hann window sums to half its length, and spreads a sine's
// power over 1.5 bins (its equivalent noise bandwidth).
constexpr double hann_sum_share = 0.5;
constexpr double hann_noise_bandwidth = 1.5;

[[nodiscard]] double finite_or_zero(float x) noexcept {
  return std::isfinite(x) ? static_cast<double>(x) : 0.0;
}

[[nodiscard]] std::size_t choice(double value, std::size_t count) noexcept {
  return std::min(static_cast<std::size_t>(std::max(0.0, value)), count - 1);
}

[[nodiscard]] bool valid_rate(double sample_rate) noexcept {
  return std::isfinite(sample_rate) && sample_rate > 0.0;
}

// The one-pole coefficient whose step response reaches 1 - 1/e in `seconds`.
[[nodiscard]] double coefficient(double seconds, double rate) noexcept {
  return 1.0 - std::exp(-1.0 / (seconds * rate));
}

[[nodiscard]] double flushed(double x) noexcept { return std::fabs(x) < silent ? 0.0 : x; }

// Each curve's slope; every one is 1 at the origin.
template <int M> [[nodiscard]] double slope_of(double u) noexcept {
  if constexpr (M == Model::tube) {
    const double th = u >= 0.0 ? std::tanh(u) : std::tanh(curves::tube_negative * u);
    return 1.0 - th * th;
  } else if constexpr (M == Model::transistor) {
    const double s = 1.0 + u * u;
    return 1.0 / (s * std::sqrt(s));
  } else if constexpr (M == Model::diode) {
    return std::exp(-std::fabs(u));
  } else {
    const double th = std::tanh(u);
    return 1.0 - th * th;
  }
}

// Calls f(std::integral_constant<int, curve>{}) for Gild's four curves.
template <typename F> decltype(auto) with_curve(int curve, F&& f) {
  switch (curve) {
  case Model::tube:
    return f(std::integral_constant<int, Model::tube>{});
  case Model::transistor:
    return f(std::integral_constant<int, Model::transistor>{});
  case Model::diode:
    return f(std::integral_constant<int, Model::diode>{});
  default:
    return f(std::integral_constant<int, Model::tape>{});
  }
}

// Calls f(std::integral_constant<std::size_t, factor>{}) for Edge's factors.
template <typename F> decltype(auto) with_factor(std::size_t factor, F&& f) {
  switch (factor) {
  case std::size_t{1} << edge_table::x2:
    return f(std::integral_constant<std::size_t, std::size_t{1} << edge_table::x2>{});
  case std::size_t{1} << edge_table::x4:
    return f(std::integral_constant<std::size_t, std::size_t{1} << edge_table::x4>{});
  case std::size_t{1} << edge_table::x8:
    return f(std::integral_constant<std::size_t, std::size_t{1} << edge_table::x8>{});
  case std::size_t{1} << edge_table::x16:
    return f(std::integral_constant<std::size_t, std::size_t{1} << edge_table::x16>{});
  default:
    return f(std::integral_constant<std::size_t, 1>{});
  }
}

// A 0..1 switch that glides over dsp::switch_fade_seconds.
struct Toggle {
  double value = 0.0;
  double target = 0.0;
  double step = 1.0;
  void to(bool on, bool jump) noexcept {
    target = on ? 1.0 : 0.0;
    if (jump) {
      value = target;
    }
  }
  double next() noexcept {
    value += std::clamp(target - value, -step, step);
    return value;
  }
  [[nodiscard]] bool moving() const noexcept { return value != target; }
};

// The curve's operating point: its value and slope at the bias, for the
// curve and bias they were taken at.
struct Point {
  int curve = -1;
  double bias = 0.0;
  double offset = 0.0;
  double slope = 1.0;
};

// One side of a channel's antialiased curve: the antiderivative at the
// previous input, kept while the drive and bias stay.
struct Side {
  double previous_ad = 0.0;
  bool ad_known = false;
};

// One channel's oversampled curve: the previous top-rate input; the curve
// at the bias plus the driven input and, for the even part, minus it; and
// the even part's DC blocker.
struct Shaper {
  double previous_u = 0.0;
  Side plus{};
  Side minus{};
  double dc_x = 0.0;
  double dc_y = 0.0;
  void reset() noexcept { *this = Shaper{}; }
  // The drive, the bias or the curve moved.
  void forget() noexcept {
    plus.ad_known = false;
    minus.ad_known = false;
  }
  [[nodiscard]] bool blocking() const noexcept { return dc_x != 0.0 || dc_y != 0.0; }
  // The even part through the DC blocker.
  double block(double even, double pole) noexcept {
    const double blocked = even - dc_x + pole * dc_y;
    dc_x = even;
    dc_y = flushed(blocked);
    return dc_y;
  }
};

// The controls of one band in one frame.
struct Drive {
  double gain = 0.0; // into the curve
  double bias = 0.0;
  double offset = 0.0; // the curve at the bias
  double scale = 1.0;  // 1 / (gain * slope at the bias)
  double pole = 0.0;   // the DC blocker's, at the top rate
};

// The curve's mean over the step from previous_v to v (ad: the
// antiderivative at v), by its antiderivatives; the curve at the midpoint
// when the step is too small for them.
template <int M>
[[nodiscard]] double mean_over(Side& side, double v, double previous_v, double ad) noexcept {
  const double dv = v - previous_v;
  double y = 0.0;
  if (std::fabs(dv) < curves::antialias_min_step) {
    y = curves::curve_of<M>(0.5 * (v + previous_v));
  } else {
    if (!side.ad_known) {
      side.previous_ad = curves::antiderivative_of<M>(previous_v);
    }
    y = (ad - side.previous_ad) / dv;
  }
  side.previous_ad = ad;
  side.ad_known = true;
  return y;
}

// The residue of curve M over one frame of top-rate inputs, in place: the
// curve's output, normalised to unit slope at its bias, less its input.
// With antialiasing the output is the curve's mean over each step from the
// previous input, and so is the input it is compared with (their mean),
// half a top-rate sample late either way. With Even (a bias or an
// asymmetric curve) the residue splits into its odd and even parts in the
// input, from the curve at the bias plus and minus the driven input; only
// the even part, which carries the curve's DC, is DC-blocked, so the odd
// part's share of the fundamental keeps its phase.
template <int M, std::size_t F, bool Even>
void residue(std::array<std::array<double, edge_dsp::max_factor>, 2>& top,
             std::array<Shaper, 2>& shapers, const Drive& d) noexcept {
  constexpr bool antialiased = F >= edge_dsp::antialiased_factor;
  std::array<double, 2 * F> plus_v{};
  std::array<double, 2 * F> minus_v{};
  std::array<double, 2 * F> plus_ad{};
  std::array<double, 2 * F> minus_ad{};
  for (std::size_t k = 0; k < F; ++k) {
    for (std::size_t c = 0; c < 2; ++c) {
      plus_v[2 * k + c] = d.bias + d.gain * top[c][k];
      minus_v[2 * k + c] = d.bias - d.gain * top[c][k];
    }
  }
  if constexpr (antialiased) {
    curves::antiderivatives_of<M>(plus_v, plus_ad);
    if constexpr (Even) {
      curves::antiderivatives_of<M>(minus_v, minus_ad);
    }
  }
  for (std::size_t c = 0; c < 2; ++c) {
    auto& s = shapers[c];
    for (std::size_t k = 0; k < F; ++k) {
      const double u = top[c][k];
      const std::size_t i = 2 * k + c;
      double plus = 0.0;
      double minus = 0.0;
      double linear = u;
      if constexpr (antialiased) {
        const double travelled = d.gain * s.previous_u;
        plus = mean_over<M>(s.plus, plus_v[i], d.bias + travelled, plus_ad[i]);
        if constexpr (Even) {
          minus = mean_over<M>(s.minus, minus_v[i], d.bias - travelled, minus_ad[i]);
        }
        linear = 0.5 * (u + s.previous_u);
      } else {
        plus = curves::curve_of<M>(plus_v[i]);
        if constexpr (Even) {
          minus = curves::curve_of<M>(minus_v[i]);
        }
      }
      const double plus_residue = (plus - d.offset) * d.scale - linear;
      if constexpr (Even) {
        const double minus_residue = (minus - d.offset) * d.scale + linear;
        const double odd = 0.5 * (plus_residue - minus_residue);
        const double even = 0.5 * (plus_residue + minus_residue);
        top[c][k] = odd + s.block(even, d.pole);
      } else {
        top[c][k] = plus_residue;
      }
      s.previous_u = u;
    }
  }
}

// A frame that adds nothing: the even part's blocker lets its DC go.
void at_rest(std::array<std::array<double, edge_dsp::max_factor>, 2>& top,
             std::array<Shaper, 2>& shapers, std::size_t factor, double pole) noexcept {
  for (std::size_t c = 0; c < 2; ++c) {
    auto& s = shapers[c];
    s.forget();
    for (std::size_t k = 0; k < factor; ++k) {
      s.previous_u = top[c][k];
      top[c][k] = s.blocking() ? s.block(0.0, pole) : 0.0;
    }
  }
}

void forget(std::array<Shaper, 2>& shapers) noexcept {
  for (auto& s : shapers) {
    s.forget();
  }
}

struct Layout {
  std::size_t bands = 1;
  std::array<double, max_crossovers> crossovers{};
};

Layout layout_for(std::span<const double> v, double rate) noexcept {
  Layout layout;
  layout.bands = choice(v[p::bands], t::band_counts.size()) + 1;
  const double top = std::min(t::crossover_max, base::nyquist_guard * rate);
  const std::size_t count = layout.bands - 1;
  for (std::size_t k = 0; k < count; ++k) {
    const double room = std::pow(crossover_spacing, static_cast<double>(count - 1 - k));
    const double floor = k == 0 ? t::crossover_min : layout.crossovers[k - 1] * crossover_spacing;
    layout.crossovers[k] = std::clamp(v[p::xover_1 + k], floor, std::max(floor, top / room));
  }
  return layout;
}

// One band's state.
struct Band {
  std::array<edge_dsp::Resampler, 2> resamplers{};
  std::array<Shaper, 2> shapers{};
  base::Ramp drive{};
  base::Ramp bias{};
  base::Ramp mix{};
  int curve = Model::tape;
  int fading_curve = Model::tape;
  // The old curve's weight while a curve change fades.
  double fade = 0.0;
  Point point{};
  Point fading_point{};
  // The drive and bias the kept antiderivatives were taken at.
  double last_gain = 0.0;
  double last_bias = 0.0;
  Toggle heard{};
  // Frames since the band last added anything; once past the resampler's
  // tail it sleeps.
  std::uint64_t idle_frames = 0;
  bool asleep = true;
  double dry_power = 0.0;
  double wet_power = 0.0;
  double added_power = 0.0;
  double envelope = 0.0;
  double drive_reading = 0.0;

  void clear() noexcept {
    for (auto& resampler : resamplers) {
      resampler.reset();
    }
    for (auto& shaper : shapers) {
      shaper.reset();
    }
    fade = 0.0;
    idle_frames = 0;
    asleep = true;
    dry_power = 0.0;
    wet_power = 0.0;
    added_power = 0.0;
    envelope = 0.0;
    drive_reading = 0.0;
  }
};

// What fixes the latency.
struct Setup {
  bool linear = false;
  std::size_t oversampling = edge_table::x1;
};

class GildKernel final : public EffectKernel {
public:
  // Control only.
  GildKernel(const Setup& setup, double sample_rate)
      : rate_(sample_rate), design_(edge_dsp::design(setup.oversampling, oversampling_filters)),
        dc_pole_(std::exp(-2.0 * std::numbers::pi * dc_hz /
                          (sample_rate * static_cast<double>(design_.factor)))),
        match_coefficient_(coefficient(match_seconds, sample_rate)),
        envelope_coefficient_(coefficient(dynamic_seconds, sample_rate)),
        hold_(std::exp(std::log(hold_fall) / (hold_seconds * sample_rate))),
        spectrum_keep_(
            std::exp(-static_cast<double>(spectrum_hop) / (spectrum_seconds * sample_rate))),
        fade_step_(1.0 / (base::switch_fade_seconds * sample_rate)), fft_(spectrum_window) {
    delayed_.assign(static_cast<std::size_t>(design_.latency) + 1, Frame{});
    if (setup.linear) {
      bank_ = std::make_unique<dsp::LinearBank>(dsp::bank_frames(sample_rate), sample_rate);
      bank_latency_ = dsp::bank_frames(sample_rate).latency();
    }
    for (auto& band : bands_) {
      for (auto& resampler : band.resamplers) {
        resampler.prepare(design_);
      }
      band.heard.step = fade_step_;
    }
    delta_.step = fade_step_;
    match_.step = fade_step_;
    history_.assign(spectrum_window, {});
    work_.assign(spectrum_window, {});
    window_.resize(spectrum_window);
    for (std::size_t i = 0; i < spectrum_window; ++i) {
      window_[i] = base::hann(static_cast<double>(i), static_cast<double>(spectrum_window));
    }
    // Each display bin sums the transform's bins whose centres it holds,
    // or reads the nearest one where it holds none.
    const double bin_hz = sample_rate / static_cast<double>(spectrum_window);
    const double ratio = std::log(spectrum_high_hz / spectrum_low_hz);
    for (std::size_t b = 0; b < spectrum_bins; ++b) {
      const auto edge = [&](double at) {
        return spectrum_low_hz * std::exp(ratio * at / static_cast<double>(spectrum_bins));
      };
      const double low = edge(static_cast<double>(b)) / bin_hz;
      const double high = edge(static_cast<double>(b + 1)) / bin_hz;
      const double nyquist_bin = static_cast<double>(spectrum_window / 2 - 1);
      auto first = static_cast<std::size_t>(std::clamp(std::ceil(low), 1.0, nyquist_bin));
      auto last = static_cast<std::size_t>(std::clamp(std::ceil(high), 1.0, nyquist_bin + 1.0));
      if (last <= first) {
        first =
            static_cast<std::size_t>(std::clamp(std::round(0.5 * (low + high)), 1.0, nyquist_bin));
        last = first + 1;
      }
      bin_first_[b] = first;
      bin_last_[b] = last;
    }
    reset();
  }

  void control(std::span<const double> v, std::uint32_t frames,
               const Transport&) noexcept override {
    const bool jump = !primed_;
    const double steps = std::max<double>(1.0, frames);
    layout_ = layout_for(v, rate_);
    std::array<base::Lr4Design, max_crossovers> designs{};
    for (std::size_t k = 0; k + 1 < layout_.bands; ++k) {
      designs[k] = base::Lr4Design::at(layout_.crossovers[k], rate_);
    }
    for (auto& tree : trees_) {
      tree.set(designs, layout_.bands - 1);
    }
    if (bank_) {
      bank_->set(layout_.bands, layout_.crossovers);
    }
    bool any_solo = false;
    for (std::size_t b = 0; b < layout_.bands; ++b) {
      any_solo = any_solo || v[p::first_band + b * t::band::size + t::band::solo] != 0.0;
    }
    for (std::size_t b = 0; b < max_bands; ++b) {
      const std::size_t first = p::first_band + b * t::band::size;
      auto& band = bands_[b];
      const bool playing = b < layout_.bands;
      band.drive.settle(playing ? std::clamp(v[first + t::band::drive], 0.0, 1.0) : 0.0, steps,
                        jump);
      band.bias.settle(std::clamp(v[first + t::band::even], 0.0, 1.0) * max_bias, steps, jump);
      band.mix.settle(std::clamp(v[first + t::band::mix], 0.0, 1.0), steps, jump);
      const int curve = static_cast<int>(choice(v[first + t::band::curve], t::curves.size()));
      if (jump) {
        band.curve = curve;
      } else if (curve != band.curve && band.fade == 0.0) {
        band.fading_curve = band.curve;
        band.curve = curve;
        band.fade = 1.0;
        forget(band.shapers);
      }
      const bool solo = v[first + t::band::solo] != 0.0;
      band.heard.to(playing && (!any_solo || solo), jump);
    }
    dynamic_ = std::clamp(v[p::dynamic], 0.0, 1.0);
    match_.to(v[p::match] != 0.0, jump);
    delta_.to(v[p::delta] != 0.0, jump);
    output_.settle(db_to_gain(v[p::output]), steps, jump);
    primed_ = true;
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame>) noexcept override {
    const std::size_t n = layout_.bands;
    const std::size_t size = delayed_.size();
    for (auto& frame : audio) {
      const std::array<double, 2> in{finite_or_zero(frame.left), finite_or_zero(frame.right)};
      for (std::size_t c = 0; c < 2; ++c) {
        input_peak_[c] = std::max(input_peak_[c] * hold_, std::fabs(in[c]));
      }
      std::array<std::array<double, max_bands>, 2> split{};
      if (bank_) {
        bank_->process(in[0], in[1], split[0], split[1]);
      } else {
        trees_[0].process(in[0], split[0]);
        trees_[1].process(in[1], split[1]);
      }
      // The bands now go up to the curves; the bands the oversampler's
      // round trip ago meet what comes back down.
      delayed_[position_] = {split[0], split[1]};
      position_ = (position_ + 1) % size;
      const Frame& dry = delayed_[position_];

      const double match = match_.next();
      std::array<double, 2> out{};
      std::array<double, 2> added{};
      std::array<double, 2> heard_dry{};
      for (std::size_t b = 0; b < n; ++b) {
        auto& band = bands_[b];
        const std::array<double, 2> x{dry[0][b], dry[1][b]};
        const std::array<double, 2> h = excite(band, split[0][b], split[1][b]);
        // Level Match: the excited band's power back to the dry band's.
        const double dry_power = 0.5 * (x[0] * x[0] + x[1] * x[1]);
        const std::array<double, 2> wet{x[0] + h[0], x[1] + h[1]};
        const double wet_power = 0.5 * (wet[0] * wet[0] + wet[1] * wet[1]);
        band.dry_power =
            flushed(band.dry_power + match_coefficient_ * (dry_power - band.dry_power));
        band.wet_power =
            flushed(band.wet_power + match_coefficient_ * (wet_power - band.wet_power));
        const double ratio =
            std::sqrt((band.dry_power + power_floor) / (band.wet_power + power_floor));
        const double gain = 1.0 + match * (std::clamp(ratio, match_floor_, match_ceiling_) - 1.0);
        const double mix = band.mix.next();
        const double heard = band.heard.next();
        double added_power = 0.0;
        for (std::size_t c = 0; c < 2; ++c) {
          const double share = mix * (gain * wet[c] - x[c]);
          added_power += 0.5 * share * share;
          out[c] += heard * (x[c] + share);
          added[c] += heard * share;
          heard_dry[c] += heard * x[c];
        }
        band.added_power =
            flushed(band.added_power + match_coefficient_ * (added_power - band.added_power));
      }
      for (std::size_t b = n; b < max_bands; ++b) {
        (void)bands_[b].heard.next();
      }
      const double delta = delta_.next();
      const double output = output_.next();
      for (std::size_t c = 0; c < 2; ++c) {
        out[c] = (out[c] - delta * heard_dry[c]) * output;
        output_peak_[c] = std::max(output_peak_[c] * hold_, std::fabs(out[c]));
      }
      listen(0.5 * (heard_dry[0] + heard_dry[1]), 0.5 * (added[0] + added[1]));
      frame.left = static_cast<float>(out[0]);
      frame.right = static_cast<float>(out[1]);
    }
  }

  void reset() noexcept override {
    for (auto& tree : trees_) {
      tree.reset();
    }
    if (bank_) {
      bank_->reset();
    }
    for (auto& band : bands_) {
      band.clear();
    }
    std::ranges::fill(delayed_, Frame{});
    position_ = 0;
    input_peak_ = {};
    output_peak_ = {};
    std::ranges::fill(history_, std::complex<double>{});
    history_position_ = 0;
    hop_count_ = 0;
    input_spectrum_.fill(0.0);
    added_spectrum_.fill(0.0);
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    const auto reading = [](double gain) {
      return static_cast<float>(base::gain_to_db(gain, reading_floor_db));
    };
    const auto power_reading = [](double power) {
      return static_cast<float>(base::power_to_db(power, reading_floor_db));
    };
    for (std::size_t c = 0; c < 2; ++c) {
      out[telemetry::input_peak + c] = reading(input_peak_[c]);
      out[telemetry::output_peak + c] = reading(output_peak_[c]);
    }
    out[telemetry::latency_ms] =
        static_cast<float>(static_cast<double>(latency()) / rate_ * milliseconds_per_second);
    out[telemetry::band_count] = static_cast<float>(layout_.bands);
    for (std::size_t k = 0; k < max_crossovers; ++k) {
      out[telemetry::crossovers + k] =
          k + 1 < layout_.bands ? static_cast<float>(layout_.crossovers[k]) : 0.0F;
    }
    for (std::size_t b = 0; b < max_bands; ++b) {
      const auto& band = bands_[b];
      const bool playing = b < layout_.bands;
      out[telemetry::band_level + b] =
          playing ? power_reading(band.dry_power) : static_cast<float>(reading_floor_db);
      out[telemetry::band_added + b] =
          playing ? power_reading(band.added_power) : static_cast<float>(reading_floor_db);
      out[telemetry::band_drive + b] =
          playing ? reading(band.drive_reading) : static_cast<float>(reading_floor_db);
    }
    for (std::size_t i = 0; i < spectrum_bins; ++i) {
      out[telemetry::input_spectrum + i] = power_reading(input_spectrum_[i]);
      out[telemetry::added_spectrum + i] = power_reading(added_spectrum_[i]);
    }
    return telemetry::size;
  }

private:
  using Frame = std::array<std::array<double, max_bands>, 2>;
  using Top = std::array<std::array<double, edge_dsp::max_factor>, 2>;

  [[nodiscard]] std::uint32_t latency() const noexcept { return bank_latency_ + design_.latency; }

  // The curve's value and slope at a bias, kept while neither moves.
  static void place(Point& point, int curve, double bias) noexcept {
    if (point.curve == curve && point.bias == bias) {
      return;
    }
    point.curve = curve;
    point.bias = bias;
    with_curve(curve, [&](auto m) {
      constexpr int M = decltype(m)::value;
      point.offset = curves::curve_of<M>(bias);
      point.slope = slope_of<M>(bias);
    });
  }

  // Band b takes one frame now (left, right) and returns what its curve
  // adds to the band as it was the round trip ago (the residue, back at the
  // base rate).
  std::array<double, 2> excite(Band& band, double left, double right) noexcept {
    const double drive = band.drive.next();
    const double bias = band.bias.next();
    // Dynamic: the band's power against the reference, as more drive.
    const double power = 0.5 * (left * left + right * right);
    band.envelope = flushed(band.envelope + envelope_coefficient_ * (power - band.envelope));
    double lift = 1.0;
    if (dynamic_ > 0.0) {
      const double under = (reference_power_ + power_floor) / (band.envelope + power_floor);
      lift =
          std::clamp(std::pow(under, 0.5 * dynamic_), 1.0, db_to_gain(dynamic_ * dynamic_range_db));
    }
    const double gain = drive_gain(drive) * lift;
    band.drive_reading = drive > 0.0 ? gain : 0.0;
    const bool still = band.drive.step == 0.0 && drive == 0.0 && band.fade == 0.0;
    if (still) {
      // Asleep once the residue's last echo has left the resampler and the
      // even part's DC has gone.
      const bool blocking = band.shapers[0].blocking() || band.shapers[1].blocking();
      if (!band.asleep && ++band.idle_frames > design_.tail && !blocking) {
        band.asleep = true;
      }
    } else {
      if (band.asleep) {
        for (auto& resampler : band.resamplers) {
          resampler.reset();
        }
        for (auto& shaper : band.shapers) {
          shaper.reset();
        }
        band.asleep = false;
      }
      band.idle_frames = 0;
    }
    std::array<double, 2> h{};
    if (!band.asleep) {
      Top top{};
      band.resamplers[0].up(left, top[0].data());
      band.resamplers[1].up(right, top[1].data());
      if (still || gain < least_drive) {
        at_rest(top, band.shapers, design_.factor, dc_pole_);
      } else {
        shape(band, top, gain, bias);
      }
      h = {band.resamplers[0].down(top[0].data()), band.resamplers[1].down(top[1].data())};
    }
    band.fade = std::max(0.0, band.fade - fade_step_);
    return h;
  }

  // The top-rate frame through the band's curve (and, while a curve change
  // fades, through the old one too), in place as the residue.
  void shape(Band& band, Top& top, double gain, double bias) noexcept {
    const auto drive_for = [&](const Point& point) {
      return Drive{gain, bias, point.offset, 1.0 / (gain * point.slope), dc_pole_};
    };
    place(band.point, band.curve, bias);
    // The kept antiderivatives belong to the drive and bias they were
    // taken at.
    if (gain != band.last_gain || bias != band.last_bias) {
      forget(band.shapers);
      band.last_gain = gain;
      band.last_bias = bias;
    }
    // The old curve starts each frame from the new one's history: its own
    // antiderivatives are not kept.
    Top old{};
    std::array<Shaper, 2> old_shapers{};
    if (band.fade > 0.0) {
      old = top;
      old_shapers = band.shapers;
      forget(old_shapers);
    }
    // The even part runs while there is one, and until its DC has gone.
    const bool even = bias != 0.0 || band.curve == Model::tube ||
                      (band.fade > 0.0 && band.fading_curve == Model::tube) ||
                      band.shapers[0].blocking() || band.shapers[1].blocking();
    const Drive now = drive_for(band.point);
    run(band.curve, even, top, band.shapers, now);
    if (band.fade > 0.0) {
      place(band.fading_point, band.fading_curve, bias);
      run(band.fading_curve, even, old, old_shapers, drive_for(band.fading_point));
      for (std::size_t c = 0; c < 2; ++c) {
        for (std::size_t k = 0; k < design_.factor; ++k) {
          top[c][k] += band.fade * (old[c][k] - top[c][k]);
        }
      }
    }
  }

  void run(int curve, bool even, Top& top, std::array<Shaper, 2>& shapers,
           const Drive& d) const noexcept {
    with_curve(curve, [&](auto m) {
      with_factor(design_.factor, [&](auto f) {
        constexpr int M = decltype(m)::value;
        constexpr std::size_t F = decltype(f)::value;
        if (even) {
          residue<M, F, true>(top, shapers, d);
        } else {
          residue<M, F, false>(top, shapers, d);
        }
      });
    });
  }

  // The spectrum: the dry and added mono sums, every hop.
  void listen(double dry, double added) noexcept {
    history_[history_position_] = {dry, added};
    history_position_ = (history_position_ + 1) % spectrum_window;
    if (++hop_count_ < spectrum_hop) {
      return;
    }
    hop_count_ = 0;
    for (std::size_t i = 0; i < spectrum_window; ++i) {
      work_[i] = history_[(history_position_ + i) % spectrum_window] * window_[i];
    }
    fft_.forward(work_);
    // Two real signals in one transform: the dry as the real part, the
    // added as the imaginary.
    const double scale = 2.0 / (hann_sum_share * static_cast<double>(spectrum_window));
    for (std::size_t b = 0; b < spectrum_bins; ++b) {
      double dry_sum = 0.0;
      double added_sum = 0.0;
      for (std::size_t k = bin_first_[b]; k < bin_last_[b]; ++k) {
        const auto z = work_[k];
        const auto mirror = std::conj(work_[spectrum_window - k]);
        const auto d = 0.5 * (z + mirror) * scale;
        const auto a = std::complex<double>{0.0, -0.5} * (z - mirror) * scale;
        dry_sum += std::norm(d);
        added_sum += std::norm(a);
      }
      dry_sum /= hann_noise_bandwidth;
      added_sum /= hann_noise_bandwidth;
      input_spectrum_[b] = flushed(dry_sum + spectrum_keep_ * (input_spectrum_[b] - dry_sum));
      added_spectrum_[b] = flushed(added_sum + spectrum_keep_ * (added_spectrum_[b] - added_sum));
    }
  }

  double rate_;
  const edge_dsp::Design& design_;
  double dc_pole_;
  double match_coefficient_;
  double envelope_coefficient_;
  double hold_;
  double spectrum_keep_;
  double fade_step_;
  double reference_power_ = db_to_gain(2.0 * dynamic_reference_db);
  double match_floor_ = db_to_gain(-match_limit_db);
  double match_ceiling_ = db_to_gain(match_limit_db);
  Layout layout_{};
  std::array<base::CrossoverTree<max_crossovers>, 2> trees_{};
  std::unique_ptr<dsp::LinearBank> bank_;
  std::uint32_t bank_latency_ = 0;
  std::vector<Frame> delayed_;
  std::size_t position_ = 0;
  std::array<Band, max_bands> bands_{};
  double dynamic_ = 0.0;
  Toggle match_{};
  Toggle delta_{};
  base::Ramp output_{};
  bool primed_ = false;
  std::array<double, 2> input_peak_{};
  std::array<double, 2> output_peak_{};
  base::Fft fft_;
  std::vector<std::complex<double>> history_;
  std::vector<std::complex<double>> work_;
  std::vector<double> window_;
  std::size_t history_position_ = 0;
  std::size_t hop_count_ = 0;
  std::array<std::size_t, spectrum_bins> bin_first_{};
  std::array<std::size_t, spectrum_bins> bin_last_{};
  std::array<double, spectrum_bins> input_spectrum_{};
  std::array<double, spectrum_bins> added_spectrum_{};
};

[[nodiscard]] bool linear_phase(std::span<const double> values) noexcept {
  return choice(values[p::phase], t::phases.size()) == t::Phase::linear;
}

[[nodiscard]] std::size_t oversampling_choice(std::span<const double> values) noexcept {
  return choice(values[p::oversampling], t::oversampling.size());
}

std::unique_ptr<EffectKernel> create_kernel(std::span<const double> values, double sample_rate) {
  if (!valid_rate(sample_rate)) {
    return nullptr;
  }
  return std::make_unique<GildKernel>(Setup{linear_phase(values), oversampling_choice(values)},
                                      sample_rate);
}

std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  if (!valid_rate(sample_rate)) {
    return 0;
  }
  const std::uint64_t resampler_tail =
      edge_dsp::design(oversampling_choice(values), oversampling_filters).tail;
  const std::uint64_t bank_tail =
      linear_phase(values) ? dsp::bank_frames(sample_rate).half_length : 0;
  const double dc_seconds = tail_time_constants / (2.0 * std::numbers::pi * dc_hz);
  return static_cast<std::uint64_t>(
             std::ceil((crossover_tail_seconds + dc_seconds) * sample_rate)) +
         resampler_tail + bank_tail;
}

std::uint32_t engine_latency(std::span<const double> values, double sample_rate) {
  return latency_frames(linear_phase(values), oversampling_choice(values), sample_rate);
}

const EngineDescriptor descriptor{"gild", "Gild", 1, t::parameters, gild_presets::presets};
const EffectEngine gild_engine{&descriptor, &create_kernel, &tail_frames, &engine_latency};

} // namespace

std::uint32_t latency_frames(bool linear, std::size_t oversampling, double sample_rate) {
  if (!valid_rate(sample_rate)) {
    return 0;
  }
  const std::uint32_t bank = linear ? dsp::bank_frames(sample_rate).latency() : 0;
  return bank + edge_dsp::design(oversampling, oversampling_filters).latency;
}

const EffectEngine& engine() { return gild_engine; }

} // namespace oscillate::native_dsp::effects::gild
