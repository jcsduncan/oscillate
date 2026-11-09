#include "oscillate/native_dsp/effects/forge.hpp"

#include "effect_dsp.hpp"
#include "forge_presets.hpp"
#include "forge_table.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/crossfade.hpp"
#include "oscillate/native_dsp/synth/filter.hpp"
#include "oscillate/native_dsp/synth/oversampler.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <type_traits>

namespace oscillate::native_dsp::effects::forge {
namespace {

using forge_table::index_of;
using forge_table::Model;

namespace p {
constexpr std::size_t model = index_of("forge.model");
constexpr std::size_t drive = index_of("forge.drive");
constexpr std::size_t bias = index_of("forge.bias");
constexpr std::size_t colour = index_of("forge.colour");
constexpr std::size_t tone = index_of("forge.tone");
constexpr std::size_t auto_gain = index_of("forge.auto_gain");
constexpr std::size_t mix = index_of("forge.mix");
constexpr std::size_t output = index_of("forge.output");
} // namespace p

constexpr std::size_t stages = 4;
constexpr std::size_t factor = std::size_t{1} << stages;
// Bias in curve units at the control's extremes.
constexpr double bias_range = 0.75;
// The tube's cut-off side saturates later and more softly than its
// grid-conduction side: tanh(a u) / a there.
constexpr double tube_negative = 0.5;
// Level matching measures a -12 dBFS sine.
constexpr double reference_amplitude = 0.25;
constexpr double makeup_limit_db = 48.0;
constexpr double emphasis_hz = 700.0;
constexpr double dc_hz = 5.0;
constexpr double meter_seconds = 0.3;

// Below this input step the antialiased value is the curve at the step's
// midpoint: the antiderivatives' difference would cancel to noise.
constexpr double antialias_min_step = 1e-7;

[[nodiscard]] int as_int(double value) noexcept { return static_cast<int>(value); }
using native_dsp::dsp::db_to_gain;
using synth::StereoSample;

// log(cosh(u)) = |u| + log1p(exp(-2|u|)) - ln 2, without overflow, for a
// whole oversampled frame at once. Three of the curves spend most of their
// time here, and the library's exp and log1p take one value at a time; these
// loops have no branches or calls, so each step runs over every value
// together (two at a time on SSE2). As accurate as the library form: within
// 3e-16 of the exact value for |u| <= 1, and 3e-16 |u| beyond.
namespace log_cosh_batch {
// exp(-2|u|) only matters while it moves log1p's result: below e^-60 it adds
// nothing to |u| >= 30.
constexpr double exp_floor = -60.0;
// Cody-Waite split of ln 2 (fdlibm): k ln2_hi is exact for |k| < 2^21.
constexpr double ln2_hi = 6.93147180369123816490e-01;
constexpr double ln2_lo = 1.90821492927058770002e-10;
// Adding then subtracting 1.5 * 2^52 rounds to the nearest integer, which
// the sum's low mantissa bits then hold.
constexpr double round_shift = 6755399436066816.0;
constexpr std::uint64_t exponent_bias = 1023;
constexpr int mantissa_bits = 52;
// e^r for |r| <= ln2 / 2 through r^13 / 13!: the next term is below 1e-17.
constexpr std::size_t exp_terms = 14;
// log1p(z) = 2 atanh(s), s = z / (2 + z) <= 1/3, through s^33: the next term
// is below 2e-18.
constexpr std::size_t atanh_terms = 16;

constexpr auto exp_coefficients = [] {
  std::array<double, exp_terms> c{};
  double factorial = 1.0;
  for (std::size_t n = 0; n < exp_terms; ++n) {
    factorial *= n == 0 ? 1.0 : static_cast<double>(n);
    c[n] = 1.0 / factorial;
  }
  return c;
}();
constexpr auto atanh_coefficients = [] {
  std::array<double, atanh_terms> c{};
  for (std::size_t k = 0; k < atanh_terms; ++k) {
    c[k] = 1.0 / static_cast<double>(2 * k + 3);
  }
  return c;
}();

template <std::size_t N>
void evaluate(const std::array<double, N>& u, std::array<double, N>& out) noexcept {
  std::array<double, N> a{};
  std::array<double, N> r{};
  std::array<double, N> scale{};
  for (std::size_t j = 0; j < N; ++j) {
    a[j] = std::fabs(u[j]);
    const double x = -2.0 * a[j];
    // NaN goes to the floor too, and comes back out through a.
    const double clamped = x >= exp_floor ? x : exp_floor;
    const double shifted = clamped * std::numbers::log2e + round_shift;
    const double k = shifted - round_shift;
    r[j] = (clamped - k * ln2_hi) - k * ln2_lo;
    const std::uint64_t power =
        std::bit_cast<std::uint64_t>(shifted) - std::bit_cast<std::uint64_t>(round_shift);
    scale[j] = std::bit_cast<double>((power + exponent_bias) << mantissa_bits);
  }
  std::array<double, N> p{};
  p.fill(exp_coefficients[exp_terms - 1]);
  for (std::size_t n = exp_terms - 1; n-- > 0;) {
    for (std::size_t j = 0; j < N; ++j) {
      p[j] = p[j] * r[j] + exp_coefficients[n];
    }
  }
  // p becomes z = exp(-2|u|), then s, with r holding s^2.
  for (std::size_t j = 0; j < N; ++j) {
    const double z = p[j] * scale[j];
    p[j] = z / (2.0 + z);
    r[j] = p[j] * p[j];
  }
  std::array<double, N> q{};
  q.fill(atanh_coefficients[atanh_terms - 1]);
  for (std::size_t k = atanh_terms - 1; k-- > 0;) {
    for (std::size_t j = 0; j < N; ++j) {
      q[j] = q[j] * r[j] + atanh_coefficients[k];
    }
  }
  for (std::size_t j = 0; j < N; ++j) {
    const double s = p[j];
    out[j] = a[j] + (2.0 * s + 2.0 * s * r[j] * q[j]) - std::numbers::ln2;
  }
}
} // namespace log_cosh_batch

[[nodiscard]] double log_cosh(double u) noexcept {
  std::array<double, 1> out{};
  log_cosh_batch::evaluate(std::array<double, 1>{u}, out);
  return out[0];
}

// The triangle fold maps u to 4|s| - 1, s = (u + 1) / 4 wrapped to
// [-0.5, 0.5): slope 1 through the origin, folding at +-1, +-3, ...
[[nodiscard]] double fold_phase(double u) noexcept {
  const double t = (u + 1.0) / 4.0;
  return t - synth::floor_inline(t + 0.5);
}

// Every curve passes through the origin with unit slope.
template <int M> [[nodiscard]] double curve_of(double u) noexcept {
  if constexpr (M == Model::tube) {
    return u >= 0.0 ? std::tanh(u) : std::tanh(tube_negative * u) / tube_negative;
  } else if constexpr (M == Model::transistor) {
    return u / std::sqrt(1.0 + u * u);
  } else if constexpr (M == Model::diode) {
    return std::copysign(1.0 - std::exp(-std::fabs(u)), u);
  } else if constexpr (M == Model::clip) {
    return std::clamp(u, -1.0, 1.0);
  } else if constexpr (M == Model::fold) {
    return 4.0 * std::fabs(fold_phase(u)) - 1.0;
  } else if constexpr (M == Model::sine_fold) {
    return std::sin(u);
  } else if constexpr (M == Model::rectify) {
    return std::fabs(std::tanh(u));
  } else {
    return std::tanh(u);
  }
}

// The antiderivative of each curve, zero at the origin.
template <int M> [[nodiscard]] double antiderivative_of(double u) noexcept {
  if constexpr (M == Model::tube) {
    return u >= 0.0 ? log_cosh(u) : log_cosh(tube_negative * u) / (tube_negative * tube_negative);
  } else if constexpr (M == Model::transistor) {
    return std::sqrt(1.0 + u * u) - 1.0;
  } else if constexpr (M == Model::diode) {
    const double a = std::fabs(u);
    return a + std::exp(-a) - 1.0;
  } else if constexpr (M == Model::clip) {
    const double a = std::fabs(u);
    return a <= 1.0 ? 0.5 * u * u : a - 0.5;
  } else if constexpr (M == Model::fold) {
    const double s = fold_phase(u);
    return 8.0 * s * std::fabs(s) - 4.0 * s + 0.5;
  } else if constexpr (M == Model::sine_fold) {
    return 1.0 - std::cos(u);
  } else if constexpr (M == Model::rectify) {
    return std::copysign(log_cosh(u), u);
  } else {
    return log_cosh(u);
  }
}

// The antiderivative at every input of an oversampled frame. The log-cosh
// curves go through log_cosh_batch; every element is the value
// antiderivative_of<M> gives.
template <int M, std::size_t N>
void antiderivatives_of(const std::array<double, N>& u, std::array<double, N>& out) noexcept {
  if constexpr (M == Model::tube) {
    std::array<double, N> v{};
    for (std::size_t j = 0; j < N; ++j) {
      v[j] = u[j] >= 0.0 ? u[j] : tube_negative * u[j];
    }
    log_cosh_batch::evaluate(v, out);
    for (std::size_t j = 0; j < N; ++j) {
      out[j] = u[j] >= 0.0 ? out[j] : out[j] / (tube_negative * tube_negative);
    }
  } else if constexpr (M == Model::rectify) {
    log_cosh_batch::evaluate(u, out);
    for (std::size_t j = 0; j < N; ++j) {
      out[j] = std::copysign(out[j], u[j]);
    }
  } else if constexpr (M == Model::transistor || M == Model::diode || M == Model::clip ||
                       M == Model::fold || M == Model::sine_fold) {
    for (std::size_t j = 0; j < N; ++j) {
      out[j] = antiderivative_of<M>(u[j]);
    }
  } else {
    log_cosh_batch::evaluate(u, out);
  }
}

// Calls f(std::integral_constant<int, model>{}), so a loop over samples is
// compiled once per curve instead of switching on every sample.
template <typename F> decltype(auto) with_model(int model, F&& f) {
  switch (model) {
  case Model::tube:
    return f(std::integral_constant<int, Model::tube>{});
  case Model::transistor:
    return f(std::integral_constant<int, Model::transistor>{});
  case Model::diode:
    return f(std::integral_constant<int, Model::diode>{});
  case Model::clip:
    return f(std::integral_constant<int, Model::clip>{});
  case Model::fold:
    return f(std::integral_constant<int, Model::fold>{});
  case Model::sine_fold:
    return f(std::integral_constant<int, Model::sine_fold>{});
  case Model::rectify:
    return f(std::integral_constant<int, Model::rectify>{});
  default:
    return f(std::integral_constant<int, Model::tape>{});
  }
}

[[nodiscard]] double curve(int model, double u) noexcept {
  return with_model(model, [u](auto m) { return curve_of<decltype(m)::value>(u); });
}

[[nodiscard]] double antiderivative(int model, double u) noexcept {
  return with_model(model, [u](auto m) { return antiderivative_of<decltype(m)::value>(u); });
}

// First-order antiderivative antialiasing: the curve's mean over the
// segment from the previous input, which suppresses the aliases of every
// harmonic by a further ~1/n^2 at the cost of a half-sample delay.
[[nodiscard]] double antialiased(int model, double u, double previous) noexcept {
  const double du = u - previous;
  if (std::fabs(du) < antialias_min_step) {
    return curve(model, 0.5 * (u + previous));
  }
  return (antiderivative(model, u) - antiderivative(model, previous)) / du;
}

// One frame of a model switch: both curves, their offsets at the bias, and
// the old curve's weight.
struct Crossfade {
  int model;
  int fading;
  double offset;
  double fading_offset;
  double weight;
};

// One channel's antialiasing history. The antiderivative at the previous
// input is kept from the step that computed it, so a step evaluates the
// antiderivative once: the same value antialiased() computes, in half the
// work.
struct Antialias {
  double previous_u = 0.0;
  double previous_ad = 0.0;
  bool ad_known = false;
  // The last midpoint step: a held input (silence at the bias point) steps
  // from the same point again and again.
  double midpoint = 0.0;
  double midpoint_y = 0.0;
  bool midpoint_known = false;

  void seed(double u) noexcept {
    previous_u = u;
    forget();
  }
  // What it kept belongs to another curve.
  void forget() noexcept {
    ad_known = false;
    midpoint_known = false;
  }

  // ad: the antiderivative at u.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): an input and its antiderivative.
  template <int M> [[nodiscard]] double step(double u, double ad) noexcept {
    const double du = u - previous_u;
    double y = 0.0;
    if (std::fabs(du) < antialias_min_step) {
      const double mid = 0.5 * (u + previous_u);
      if (!midpoint_known || mid != midpoint) {
        midpoint = mid;
        midpoint_y = curve_of<M>(mid);
        midpoint_known = true;
      }
      y = midpoint_y;
      ad_known = false;
    } else {
      if (!ad_known) {
        previous_ad = antiderivative_of<M>(previous_u);
      }
      y = (ad - previous_ad) / du;
      previous_ad = ad;
      ad_known = true;
    }
    previous_u = u;
    return y;
  }

  // The crossfade between two curves after a model switch.
  [[nodiscard]] double blend(const Crossfade& fade, double u) noexcept {
    double y = antialiased(fade.model, u, previous_u) - fade.offset;
    if (fade.weight > 0.0) {
      const double old = antialiased(fade.fading, u, previous_u) - fade.fading_offset;
      y += (old - y) * fade.weight;
    }
    previous_u = u;
    forget();
    return y;
  }
};

// Makeup that gives a -12 dBFS sine its input level back, offset removed.
[[nodiscard]] double level_match(int model, double drive, double bias) noexcept {
  // Fine enough for the narrow zero-crossing dips of a hard-driven
  // rectifier; recomputed only when the curve changes.
  constexpr std::size_t points = 1'024;
  const double offset = curve(model, bias);
  double mean = 0.0;
  double power = 0.0;
  for (std::size_t i = 0; i < points; ++i) {
    const double x = reference_amplitude *
                     std::sin(2.0 * std::numbers::pi * (static_cast<double>(i) + 0.5) / points);
    const double y = curve(model, drive * x + bias) - offset;
    mean += y;
    power += y * y;
  }
  mean /= points;
  const double out_rms = std::sqrt(std::max(0.0, power / points - mean * mean));
  const double in_rms = reference_amplitude / std::numbers::sqrt2;
  const double limit = db_to_gain(makeup_limit_db);
  return std::clamp(in_rms / std::max(out_rms, 1e-12), 1.0 / limit, limit);
}

using Oversampled = std::array<StereoSample, factor>;

// 16x through four elliptic half-band stages (about 100 dB stopband): with
// antialiasing, smooth curves keep audio-band aliases below -90 dBFS at any
// drive for fundamentals up to 5 kHz. Both channels run together.
struct Resampler {
  std::array<synth::StereoUpsampler2x, stages> ups;
  std::array<synth::StereoDownsampler2x, stages> downs;

  void reset() noexcept {
    for (auto& stage : ups) {
      stage.reset();
    }
    for (auto& stage : downs) {
      stage.reset();
    }
  }
  void up(StereoSample x, Oversampled& out) noexcept {
    // Each stage takes its input in time order.
    Oversampled in{};
    out[0] = x;
    for (std::size_t stage = 0, count = 1; stage < ups.size(); ++stage, count *= 2) {
      std::copy_n(out.begin(), count, in.begin());
      for (std::size_t k = 0; k < count; ++k) {
        ups[stage].process(in[k], out[2 * k], out[2 * k + 1]);
      }
    }
  }
  [[nodiscard]] StereoSample down(Oversampled in) noexcept {
    for (std::size_t stage = downs.size(), count = factor / 2; stage-- > 0; count /= 2) {
      for (std::size_t k = 0; k < count; ++k) {
        in[k] = downs[stage].process(in[2 * k], in[2 * k + 1]);
      }
    }
    return in[0];
  }
};

struct Channel {
  dsp::Shelf emphasis;
  dsp::Shelf de_emphasis;
  synth::MultimodeFilter tone;
  Antialias antialias;
  double dc_x = 0.0;
  double dc_y = 0.0;

  void reset() noexcept {
    emphasis.reset();
    de_emphasis.reset();
    tone.reset();
    antialias = {};
    dc_x = 0.0;
    dc_y = 0.0;
  }
};

// One linear ramp per control tick.
using dsp::Ramp;

// The smoothed controls of one frame.
struct FrameControls {
  double drive;
  double bias;
  double makeup;
  double mix;
  double gain;
};

class ForgeKernel final : public EffectKernel {
public:
  explicit ForgeKernel(double sample_rate)
      : rate_(sample_rate), dc_pole_(std::exp(-2.0 * std::numbers::pi * dc_hz / sample_rate)),
        meter_(1.0 - std::exp(-1.0 / (meter_seconds * sample_rate))),
        emphasis_k_(std::tan(std::numbers::pi * emphasis_hz / sample_rate)) {
    fade_.prepare(sample_rate);
    reset();
  }

  void control(std::span<const double> v, std::uint32_t frames,
               const Transport&) noexcept override {
    const int model = as_int(v[p::model]);
    if (!primed_) {
      model_ = model;
    } else if (model != model_ && !fade_.active()) {
      fading_model_ = model_;
      model_ = model;
      fade_.start();
    }
    const double drive = db_to_gain(v[p::drive]);
    const double bias = v[p::bias] * bias_range;
    const bool auto_gain = v[p::auto_gain] != 0.0;
    if (!primed_ || model != makeup_model_ || drive != makeup_drive_ || bias != makeup_bias_ ||
        auto_gain != makeup_auto_) {
      makeup_model_ = model;
      makeup_drive_ = drive;
      makeup_bias_ = bias;
      makeup_auto_ = auto_gain;
      makeup_target_ = auto_gain ? level_match(model, drive, bias) : 1.0;
      draw_curve(model, drive, bias, makeup_target_);
    }
    const double steps = std::max<double>(1.0, frames);
    drive_.to(drive, steps, !primed_);
    bias_.to(bias, steps, !primed_);
    makeup_.to(makeup_target_, steps, !primed_);
    if (!primed_) {
      seed_history();
    }
    mix_.to(v[p::mix], steps, !primed_);
    gain_.to(db_to_gain(v[p::output]), steps, !primed_);

    const double colour = db_to_gain(v[p::colour]);
    const auto tone = synth::make_filter_coefficients(synth::FilterMode::lowpass_12,
                                                      v[p::tone] / rate_, 0.0, 1.0);
    for (auto& channel : channels_) {
      channel.emphasis.set(colour, 1.0, 1.0, 1.0, emphasis_k_);
      channel.de_emphasis.set(1.0, 1.0, colour, 1.0, emphasis_k_);
    }
    tone_ = tone;
    primed_ = true;
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame>) noexcept override {
    // A model switch fades in control() and ends here, so the frames of a
    // fade come first.
    std::size_t done = 0;
    for (; done < audio.size() && fade_.active(); ++done) {
      fading_frame(audio[done]);
    }
    const auto rest = audio.subspan(done);
    if (!rest.empty()) {
      with_model(model_, [&](auto m) { run<decltype(m)::value>(rest); });
    }
  }

  void reset() noexcept override {
    for (auto& channel : channels_) {
      channel.reset();
    }
    wet_.reset();
    dry_.reset();
    previous_dry_ = {};
    seed_history();
    fade_.stop();
    input_power_ = 0.0;
    output_power_ = 0.0;
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    out[telemetry::input_rms] = static_cast<float>(std::sqrt(input_power_));
    out[telemetry::output_rms] = static_cast<float>(std::sqrt(output_power_));
    out[telemetry::makeup_db] = static_cast<float>(20.0 * std::log10(makeup_target_));
    std::ranges::copy(curve_, out.begin() + telemetry::curve);
    return telemetry::size;
  }

private:
  [[nodiscard]] FrameControls next_controls() noexcept {
    const double drive = drive_.next();
    const double bias = bias_.next();
    const double makeup = makeup_.next();
    const double mix = mix_.next();
    const double gain = gain_.next();
    return {drive, bias, makeup, mix, gain};
  }

  // The emphasised input, at the oversampled rate.
  void oversample(const engine::StereoFrame& frame, Oversampled& up) noexcept {
    wet_.up({channels_[0].emphasis.process(frame.left), channels_[1].emphasis.process(frame.right)},
            up);
  }

  template <int M> void run(std::span<engine::StereoFrame> audio) noexcept {
    if (M != run_model_) {
      for (auto& channel : channels_) {
        channel.antialias.forget();
      }
      run_model_ = M;
    }
    Oversampled up{};
    std::array<double, 2 * factor> inputs{};
    std::array<double, 2 * factor> antiderivatives{};
    double offset_bias = 0.0;
    double offset = 0.0;
    bool offset_known = false;
    for (auto& frame : audio) {
      const auto controls = next_controls();
      if (!offset_known || controls.bias != offset_bias) {
        offset = curve_of<M>(controls.bias);
        offset_bias = controls.bias;
        offset_known = true;
      }
      oversample(frame, up);
      // Left and right interleaved.
      for (std::size_t k = 0; k < factor; ++k) {
        inputs[2 * k] = controls.drive * up[k].left + controls.bias;
        inputs[2 * k + 1] = controls.drive * up[k].right + controls.bias;
      }
      antiderivatives_of<M>(inputs, antiderivatives);
      auto& left = channels_[0].antialias;
      auto& right = channels_[1].antialias;
      for (std::size_t k = 0; k < factor; ++k) {
        up[k] = {left.step<M>(inputs[2 * k], antiderivatives[2 * k]) - offset,
                 right.step<M>(inputs[2 * k + 1], antiderivatives[2 * k + 1]) - offset};
      }
      finish(frame, controls, up);
    }
  }

  void fading_frame(engine::StereoFrame& frame) noexcept {
    const auto controls = next_controls();
    const double weight = fade_.next();
    const double offset = curve(model_, controls.bias);
    const double fading_offset = weight > 0.0 ? curve(fading_model_, controls.bias) : 0.0;
    const Crossfade fade{model_, fading_model_, offset, fading_offset, weight};
    Oversampled up{};
    oversample(frame, up);
    auto& left = channels_[0].antialias;
    auto& right = channels_[1].antialias;
    for (auto& sample : up) {
      const double u_left = controls.drive * sample.left + controls.bias;
      const double u_right = controls.drive * sample.right + controls.bias;
      sample = {left.blend(fade, u_left), right.blend(fade, u_right)};
    }
    finish(frame, controls, up);
  }

  // From the shaped oversampled frame to the output: back to the base rate,
  // the dry path, the mix, tone, DC blocking and output gain.
  void finish(engine::StereoFrame& frame, const FrameControls& controls,
              Oversampled& shaped) noexcept {
    const StereoSample in{frame.left, frame.right};
    const auto down = wet_.down(shaped);
    const std::array<double, 2> wet{channels_[0].de_emphasis.process(down.left) * controls.makeup,
                                    channels_[1].de_emphasis.process(down.right) * controls.makeup};
    // The dry path through the same resampling and the identity's
    // antialiasing (a two-point mean), so a partial mix lines up.
    Oversampled& up = shaped;
    dry_.up(in, up);
    for (auto& sample : up) {
      const StereoSample x = sample;
      sample = 0.5 * (x + previous_dry_);
      previous_dry_ = x;
    }
    const auto aligned = dry_.down(up);
    const std::array<double, 2> dry{aligned.left, aligned.right};
    std::array<double, 2> out{};
    for (std::size_t c = 0; c < channels_.size(); ++c) {
      auto& channel = channels_[c];
      const double mixed = channel.tone.process(dry[c] + (wet[c] - dry[c]) * controls.mix, tone_);
      // A 5 Hz DC blocker: bias and rectification leave an offset.
      const double blocked = mixed - channel.dc_x + dc_pole_ * channel.dc_y;
      channel.dc_x = mixed;
      channel.dc_y = blocked;
      out[c] = blocked * controls.gain;
    }
    input_power_ += meter_ * (0.5 * (in.left * in.left + in.right * in.right) - input_power_);
    output_power_ += meter_ * (0.5 * (out[0] * out[0] + out[1] * out[1]) - output_power_);
    frame.left = static_cast<float>(out[0]);
    frame.right = static_cast<float>(out[1]);
  }

  // Silence sits at the bias point: start the antialiasing history there so
  // a start or a seek adds no step.
  void seed_history() noexcept {
    for (auto& channel : channels_) {
      channel.antialias.seed(bias_.value);
    }
  }

  void draw_curve(int model, double drive, double bias, double makeup) noexcept {
    const double offset = curve(model, bias);
    for (std::size_t i = 0; i < curve_points; ++i) {
      const double x = -1.0 + 2.0 * static_cast<double>(i) / (curve_points - 1);
      curve_[i] = static_cast<float>(makeup * (curve(model, drive * x + bias) - offset));
    }
  }

  double rate_;
  double dc_pole_;
  double meter_;
  double emphasis_k_;
  std::array<Channel, 2> channels_{};
  Resampler wet_;
  Resampler dry_;
  StereoSample previous_dry_{};
  synth::FilterCoefficients tone_{};
  synth::SwitchFade fade_;
  int model_ = Model::tape;
  int fading_model_ = Model::tape;
  // The curve the last run() rendered, whose values the histories keep.
  int run_model_ = Model::tape;
  Ramp drive_;
  Ramp bias_;
  Ramp makeup_;
  Ramp mix_;
  Ramp gain_;
  int makeup_model_ = -1;
  double makeup_drive_ = 0.0;
  double makeup_bias_ = 0.0;
  bool makeup_auto_ = false;
  double makeup_target_ = 1.0;
  std::array<float, curve_points> curve_{};
  double input_power_ = 0.0;
  double output_power_ = 0.0;
  bool primed_ = false;
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double>, double sample_rate) {
  return std::make_unique<ForgeKernel>(sample_rate);
}

// The DC blocker's decay dominates: 60 dB in 6.9 time constants.
std::uint64_t tail_frames(std::span<const double>, double sample_rate) {
  const double seconds = 6.9 / (2.0 * std::numbers::pi * dc_hz) + 0.05;
  return static_cast<std::uint64_t>(std::ceil(seconds * sample_rate));
}

std::uint32_t latency_frames(std::span<const double>, double) { return 0; }

const EngineDescriptor descriptor{"forge", "Forge", 1, forge_table::parameters,
                                  forge_presets::presets};
const EffectEngine forge_engine{&descriptor, &create_kernel, &tail_frames, &latency_frames, false};

} // namespace

const EffectEngine& engine() { return forge_engine; }

namespace {
struct Shape {
  int model;
  double drive;
  double bias;
  double makeup;
};
Shape shape_of(std::span<const double> v) noexcept {
  const int model = as_int(v[p::model]);
  const double drive = db_to_gain(v[p::drive]);
  const double bias = v[p::bias] * bias_range;
  return {model, drive, bias, v[p::auto_gain] != 0.0 ? level_match(model, drive, bias) : 1.0};
}
} // namespace

double makeup_db(std::span<const double> values) noexcept {
  if (values.size() != forge_table::parameters.size()) {
    return 0.0;
  }
  return 20.0 * std::log10(shape_of(values).makeup);
}

void transfer_curve(std::span<const double> values, std::span<float> out) noexcept {
  if (values.size() != forge_table::parameters.size() || out.size() < 2) {
    return;
  }
  const auto s = shape_of(values);
  const double offset = curve(s.model, s.bias);
  for (std::size_t i = 0; i < out.size(); ++i) {
    const double x = -1.0 + 2.0 * static_cast<double>(i) / static_cast<double>(out.size() - 1);
    out[i] = static_cast<float>(s.makeup * (curve(s.model, s.drive * x + s.bias) - offset));
  }
}

} // namespace oscillate::native_dsp::effects::forge
