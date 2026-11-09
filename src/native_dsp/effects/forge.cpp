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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>

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

[[nodiscard]] int as_int(double value) noexcept { return static_cast<int>(value); }
using native_dsp::dsp::db_to_gain;

// log(cosh(u)) without overflow.
[[nodiscard]] double log_cosh(double u) noexcept {
  const double a = std::fabs(u);
  return a + std::log1p(std::exp(-2.0 * a)) - std::numbers::ln2;
}

// The triangle fold maps u to 4|s| - 1, s = (u + 1) / 4 wrapped to
// [-0.5, 0.5): slope 1 through the origin, folding at +-1, +-3, ...
[[nodiscard]] double fold_phase(double u) noexcept {
  const double t = (u + 1.0) / 4.0;
  return t - std::floor(t + 0.5);
}

// Every curve passes through the origin with unit slope.
[[nodiscard]] double curve(int model, double u) noexcept {
  switch (model) {
  case Model::tube:
    return u >= 0.0 ? std::tanh(u) : std::tanh(tube_negative * u) / tube_negative;
  case Model::transistor:
    return u / std::sqrt(1.0 + u * u);
  case Model::diode:
    return std::copysign(1.0 - std::exp(-std::fabs(u)), u);
  case Model::clip:
    return std::clamp(u, -1.0, 1.0);
  case Model::fold:
    return 4.0 * std::fabs(fold_phase(u)) - 1.0;
  case Model::sine_fold:
    return std::sin(u);
  case Model::rectify:
    return std::fabs(std::tanh(u));
  default:
    return std::tanh(u);
  }
}

// The antiderivative of each curve, zero at the origin.
[[nodiscard]] double antiderivative(int model, double u) noexcept {
  switch (model) {
  case Model::tube:
    return u >= 0.0 ? log_cosh(u) : log_cosh(tube_negative * u) / (tube_negative * tube_negative);
  case Model::transistor:
    return std::sqrt(1.0 + u * u) - 1.0;
  case Model::diode: {
    const double a = std::fabs(u);
    return a + std::exp(-a) - 1.0;
  }
  case Model::clip: {
    const double a = std::fabs(u);
    return a <= 1.0 ? 0.5 * u * u : a - 0.5;
  }
  case Model::fold: {
    const double s = fold_phase(u);
    return 8.0 * s * std::fabs(s) - 4.0 * s + 0.5;
  }
  case Model::sine_fold:
    return 1.0 - std::cos(u);
  case Model::rectify:
    return std::copysign(log_cosh(u), u);
  default:
    return log_cosh(u);
  }
}

// First-order antiderivative antialiasing: the curve's mean over the
// segment from the previous input, which suppresses the aliases of every
// harmonic by a further ~1/n^2 at the cost of a half-sample delay.
[[nodiscard]] double antialiased(int model, double u, double previous) noexcept {
  const double du = u - previous;
  if (std::fabs(du) < 1e-7) {
    return curve(model, 0.5 * (u + previous));
  }
  return (antiderivative(model, u) - antiderivative(model, previous)) / du;
}

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

// 16x through four elliptic half-band stages (about 100 dB stopband): with
// antialiasing, smooth curves keep audio-band aliases below -90 dBFS at any
// drive for fundamentals up to 5 kHz.
struct Resampler {
  std::array<synth::Upsampler2x, stages> ups;
  std::array<synth::Downsampler2x, stages> downs;

  void reset() noexcept {
    for (auto& stage : ups) {
      stage.reset();
    }
    for (auto& stage : downs) {
      stage.reset();
    }
  }
  void up(double x, std::array<double, factor>& out) noexcept {
    // Each stage takes its input in time order.
    std::array<double, factor> in{};
    out[0] = x;
    for (std::size_t stage = 0, count = 1; stage < ups.size(); ++stage, count *= 2) {
      std::copy_n(out.begin(), count, in.begin());
      for (std::size_t k = 0; k < count; ++k) {
        ups[stage].process(in[k], out[2 * k], out[2 * k + 1]);
      }
    }
  }
  [[nodiscard]] double down(std::array<double, factor> in) noexcept {
    for (std::size_t stage = downs.size(), count = factor / 2; stage-- > 0; count /= 2) {
      for (std::size_t k = 0; k < count; ++k) {
        in[k] = downs[stage].process(in[2 * k], in[2 * k + 1]);
      }
    }
    return in[0];
  }
};

struct Channel {
  Resampler wet;
  Resampler dry;
  dsp::Shelf emphasis;
  dsp::Shelf de_emphasis;
  synth::MultimodeFilter tone;
  double previous_u = 0.0;
  double previous_dry = 0.0;
  double dc_x = 0.0;
  double dc_y = 0.0;

  void reset() noexcept {
    wet.reset();
    dry.reset();
    emphasis.reset();
    de_emphasis.reset();
    tone.reset();
    previous_u = 0.0;
    previous_dry = 0.0;
    dc_x = 0.0;
    dc_y = 0.0;
  }
};

// One linear ramp per control tick.
using dsp::Ramp;

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
    std::array<double, factor> up{};
    std::array<double, factor> shaped{};
    for (auto& frame : audio) {
      const double drive = drive_.next();
      const double bias = bias_.next();
      const double makeup = makeup_.next();
      const double mix = mix_.next();
      const double gain = gain_.next();
      const double weight = fade_.active() ? fade_.next() : 0.0;
      const double offset = curve(model_, bias);
      const double fading_offset = weight > 0.0 ? curve(fading_model_, bias) : 0.0;
      std::array<double, 2> in{frame.left, frame.right};
      std::array<double, 2> out{};
      for (std::size_t c = 0; c < 2; ++c) {
        auto& channel = channels_[c];
        channel.wet.up(channel.emphasis.process(in[c]), up);
        for (std::size_t k = 0; k < factor; ++k) {
          const double u = drive * up[k] + bias;
          double y = antialiased(model_, u, channel.previous_u) - offset;
          if (weight > 0.0) {
            const double old = antialiased(fading_model_, u, channel.previous_u) - fading_offset;
            y += (old - y) * weight;
          }
          channel.previous_u = u;
          shaped[k] = y;
        }
        const double wet = channel.de_emphasis.process(channel.wet.down(shaped)) * makeup;
        // The dry path through the same resampling and the identity's
        // antialiasing (a two-point mean), so a partial mix lines up.
        channel.dry.up(in[c], up);
        for (std::size_t k = 0; k < factor; ++k) {
          const double x = up[k];
          up[k] = 0.5 * (x + channel.previous_dry);
          channel.previous_dry = x;
        }
        const double aligned = channel.dry.down(up);
        const double mixed = channel.tone.process(aligned + (wet - aligned) * mix, tone_);
        // A 5 Hz DC blocker: bias and rectification leave an offset.
        const double blocked = mixed - channel.dc_x + dc_pole_ * channel.dc_y;
        channel.dc_x = mixed;
        channel.dc_y = blocked;
        out[c] = blocked * gain;
      }
      input_power_ += meter_ * (0.5 * (in[0] * in[0] + in[1] * in[1]) - input_power_);
      output_power_ += meter_ * (0.5 * (out[0] * out[0] + out[1] * out[1]) - output_power_);
      frame.left = static_cast<float>(out[0]);
      frame.right = static_cast<float>(out[1]);
    }
  }

  void reset() noexcept override {
    for (auto& channel : channels_) {
      channel.reset();
    }
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
  // Silence sits at the bias point: start the antialiasing history there so
  // a start or a seek adds no step.
  void seed_history() noexcept {
    for (auto& channel : channels_) {
      channel.previous_u = bias_.value;
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
  synth::FilterCoefficients tone_{};
  synth::SwitchFade fade_;
  int model_ = Model::tape;
  int fading_model_ = Model::tape;
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
