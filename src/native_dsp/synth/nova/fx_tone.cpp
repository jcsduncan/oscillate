#include "fx_modules.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/forge.hpp"
#include "oscillate/native_dsp/synth/nova/filter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

// Nova's Distortion and Filter modules (08a §5). Distortion runs Forge
// (ADR-0040: antiderivative-antialiased curves at 16x with auto gain) for
// its eight analog-style curves and Nova's own Bit Crush, Downsample and
// X-Shaper at the chain's (oversampled) rate. The Filter module is one of
// Nova's voice filters (any of the 22 models) at the chain's rate.
namespace oscillate::native_dsp::synth::nova::fx_detail {
namespace {

// ---- Distortion ---------------------------------------------------------------------

enum Mode : std::uint8_t {
  tube,
  soft,
  hard,
  diode,
  fold,
  rectify,
  sine_fold,
  asym,
  bit_crush,
  downsample,
  x_shaper,
  overdrive,
  mode_count
};

// Forge's models: tape, tube, transistor, diode, clip, fold, sine fold,
// rectify. Nova's own modes have no Forge model.
constexpr std::array<int, mode_count> forge_model{1, 0, 4, 3, 5, 7, 6, 0, -1, -1, -1, 2};
[[nodiscard]] bool is_forge(std::size_t mode) noexcept { return forge_model[mode] >= 0; }

constexpr double tilt_hz = 700.0;
constexpr double tilt_db = 6.0;
constexpr double asym_bias = 0.4;
constexpr double fade_seconds = native_dsp::dsp::switch_fade_seconds;

// The X-Shaper: a smooth, asymmetric saturating family. Unity slope at
// zero; the positive half saturates at 1, the negative half at 1 / k, so
// k = 1 is symmetric and k != 1 adds even harmonics. Both halves have
// closed-form antiderivatives for first-order antialiasing.
[[nodiscard]] double shaper(double u, double k) noexcept {
  return u >= 0.0 ? 1.0 - std::exp(-u) : (std::exp(k * u) - 1.0) / k;
}
[[nodiscard]] double shaper_integral(double u, double k) noexcept {
  return u >= 0.0 ? u + std::exp(-u) - 1.0 : (std::exp(k * u) - 1.0) / (k * k) - u / k;
}

class DistortionModule final : public FxModule {
public:
  DistortionModule(double base_rate, std::size_t max_frames) : base_(base_rate) {
    ok_ = forge_.init(effects::forge::engine(), base_rate, max_frames);
    if (!ok_) {
      return;
    }
    model_ = forge_.index("forge.model");
    drive_ = forge_.index("forge.drive");
    bias_ = forge_.index("forge.bias");
    colour_ = forge_.index("forge.colour");
    tone_ = forge_.index("forge.tone");
    auto_gain_ = forge_.index("forge.auto_gain");
    mix_ = forge_.index("forge.mix");
    output_ = forge_.index("forge.output");
    ok_ = forge_.ok();
    copy_.assign(std::max<std::size_t>(1, max_frames) * max_factor, {0.0F, 0.0F});
    rate(base_rate);
    apply(fx_defaults(FxKind::distortion).p, {2.0, base_rate}, 0);
  }
  [[nodiscard]] bool ok() const noexcept { return ok_; }

  [[nodiscard]] FxKind kind() const noexcept override { return FxKind::distortion; }
  void reset() noexcept override {
    forge_.reset();
    own_ = {};
    tone_state_ = {};
    dc_in_ = {};
    dc_out_ = {};
    meter_.reset();
  }
  void set(const FxParams& p, const FxContext& context, std::uint32_t frames) noexcept override {
    apply(p, context, frames);
  }

  void process(Frames io) noexcept override {
    const bool fading = fade_.value > 0.0;
    const bool new_forge = is_forge(mode_);
    const bool old_forge = fading && is_forge(old_mode_);
    const bool use_forge = new_forge || old_forge;
    if (use_forge) {
      if (forge_idle_) {
        forge_.reset();
        forge_idle_ = false;
      }
      const std::size_t n = std::min(io.size(), copy_.size());
      std::copy_n(io.begin(), n, copy_.begin());
      forge_.process(Frames(copy_.data(), n));
    } else {
      forge_idle_ = true;
    }
    for (std::size_t i = 0; i < io.size(); ++i) {
      const double drive_db = drive_db_.next();
      const double bias = bias_value_.next();
      const double tilt = tilt_.next();
      const double makeup = makeup_.next();
      const double weight = fade_.next();
      const std::array<double, 2> x{io[i].left, io[i].right};
      const std::array<double, 2> forged{i < copy_.size() ? copy_[i].left : 0.0F,
                                         i < copy_.size() ? copy_[i].right : 0.0F};
      std::array<double, 2> y{};
      for (std::size_t c = 0; c < 2; ++c) {
        const double fresh = new_forge ? forged[c] : own(mode_, c, x[c], drive_db, bias, makeup);
        double out = fresh;
        if (weight > 0.0) {
          const double old =
              is_forge(old_mode_) ? forged[c] : own(old_mode_, c, x[c], drive_db, bias, makeup);
          out = fresh + weight * (old - fresh);
        }
        // Bias and asymmetry leave an offset: a 5 Hz blocker keeps it out.
        const double blocked = out - dc_in_[c] + dc_pole_ * dc_out_[c];
        dc_in_[c] = out;
        dc_out_[c] = blocked;
        // Tone: a tilt around 700 Hz, +-6 dB at the ends.
        tone_state_[c] += tilt_coefficient_ * (blocked - tone_state_[c]);
        const double low = tone_state_[c];
        const double high = blocked - low;
        y[c] = low * db_to_gain(-tilt_db * tilt) + high * db_to_gain(tilt_db * tilt);
      }
      io[i].left = static_cast<float>(y[0]);
      io[i].right = static_cast<float>(y[1]);
    }
    meter_.feed(io);
  }

  [[nodiscard]] float telemetry() const noexcept override { return meter_.db(); }
  [[nodiscard]] std::uint64_t tail_frames() const noexcept override {
    // The DC blocker's settle and Forge's resampling.
    return static_cast<std::uint64_t>(std::ceil(0.25 * rate_));
  }

private:
  struct OwnState {
    std::array<double, 2> previous{}; // X-Shaper antialiasing.
    std::array<double, 2> held{};     // Downsample.
    double phase = 1.0;
  };

  void rate(double rate) noexcept {
    rate_ = rate;
    tilt_coefficient_ = native_dsp::dsp::one_pole_coefficient(tilt_hz, rate);
    dc_pole_ = std::exp(-two_pi * 5.0 / rate);
    meter_.prepare(rate);
    fade_frames_ = static_cast<std::uint32_t>(std::lround(fade_seconds * rate));
  }

  void apply(const FxParams& p, const FxContext& context, std::uint32_t frames) noexcept {
    const std::uint32_t factor = factor_for(context.sample_rate, base_);
    if (factor != factor_) {
      factor_ = factor;
      rate(base_ * factor);
      forge_.set_factor(factor);
    }
    const std::size_t mode = choice(p[7], mode_count);
    if (mode != mode_) {
      // Forge crossfades between its own models; any other change of curve
      // crossfades here, both curves running from the same input.
      const bool forge_to_forge = is_forge(mode) && is_forge(mode_);
      if (!forge_to_forge && primed_) {
        old_mode_ = mode_;
        fade_.jump(1.0);
        fade_.to(0.0, fade_frames_);
        if (!is_forge(mode)) {
          own_[mode - bit_crush] = {};
        }
      }
      mode_ = mode;
    }
    const double drive_db = 48.0 * clamp01(p[0]);
    const double bias = 2.0 * clamp01(p[2]) - 1.0;
    const double tilt = 2.0 * clamp01(p[1]) - 1.0;
    const bool jump = !primed_ || frames == 0;
    const std::uint32_t steps = jump ? 0U : frames;
    drive_db_.to(drive_db, steps);
    bias_value_.to(bias, steps);
    tilt_.to(tilt, steps);
    makeup_.to(shaper_makeup(drive_db, bias), steps);
    primed_ = true;
    if (is_forge(mode_)) {
      forge_.value(model_, static_cast<double>(forge_model[mode_]));
      forge_.value(drive_, drive_db);
      forge_.value(bias_, std::clamp(bias + (mode_ == asym ? asym_bias : 0.0), -1.0, 1.0));
      forge_.value(colour_, mode_ == asym ? 3.0 : 0.0);
      forge_.value(tone_, 20'000.0);
      forge_.value(auto_gain_, 1.0);
      forge_.value(mix_, 1.0);
      forge_.value(output_, 0.0);
      forge_.control(frames == 0 ? 0U : std::max<std::uint32_t>(1, frames / factor_),
                     context.beats_per_second);
    }
  }

  // The X-Shaper's level match: a -12 dBFS sine comes out at its own level
  // at any drive and asymmetry, as Forge's auto gain does.
  [[nodiscard]] static double shaper_makeup(double drive_db, double bias) noexcept {
    const double g = db_to_gain(drive_db);
    const double k = std::exp(1.5 * bias);
    constexpr std::size_t points = 64;
    constexpr double amplitude = 0.25;
    double in = 0.0;
    double out = 0.0;
    double mean = 0.0;
    std::array<double, points> y{};
    for (std::size_t i = 0; i < points; ++i) {
      const double x = amplitude * std::sin(two_pi * static_cast<double>(i) / points);
      y[i] = shaper(g * x, k);
      mean += y[i] / points;
      in += x * x;
    }
    for (const double v : y) {
      out += (v - mean) * (v - mean);
    }
    return out > 1e-12 ? std::clamp(std::sqrt(in / out), 0.05, 8.0) : 1.0;
  }

  // Nova's own curves, per channel, at the chain's rate.
  [[nodiscard]] double own(std::size_t mode, std::size_t c, double x, double drive_db, double bias,
                           double makeup) noexcept {
    auto& state = own_[mode - bit_crush];
    const double amount = drive_db / 48.0;
    switch (mode) {
    case bit_crush: {
      // 16 bits down to 2, continuously.
      const double steps = std::exp2(15.0 - 14.0 * amount);
      return std::round(soft_clip(x) * steps) / steps;
    }
    case downsample: {
      // Sample and hold from the engine's rate down to 200 Hz. Both
      // channels share the clock.
      const double held_rate = base_ * std::pow(200.0 / base_, amount);
      if (c == 0) {
        state.phase += held_rate / rate_;
      }
      if (state.phase >= 1.0) {
        state.held[c] = x;
        if (c == 1) {
          state.phase -= std::floor(state.phase);
        }
      }
      return state.held[c];
    }
    default: { // X-Shaper.
      const double k = std::exp(1.5 * bias);
      const double u = db_to_gain(drive_db) * x;
      const double previous = state.previous[c];
      const double du = u - previous;
      state.previous[c] = u;
      const double y = std::fabs(du) < 1e-6
                           ? shaper(0.5 * (u + previous), k)
                           : (shaper_integral(u, k) - shaper_integral(previous, k)) / du;
      return y * makeup;
    }
    }
  }

  double base_;
  double rate_ = 48'000.0;
  std::uint32_t factor_ = 1;
  KernelDriver forge_;
  bool ok_ = false;
  bool primed_ = false;
  bool forge_idle_ = true;
  std::size_t model_ = 0, drive_ = 0, bias_ = 0, colour_ = 0, tone_ = 0, auto_gain_ = 0, mix_ = 0,
              output_ = 0;
  std::vector<engine::StereoFrame> copy_;
  std::size_t mode_ = tube;
  std::size_t old_mode_ = tube;
  Glide fade_;
  std::uint32_t fade_frames_ = 960;
  Glide drive_db_;
  Glide bias_value_;
  Glide tilt_;
  Glide makeup_;
  std::array<OwnState, 3> own_{};
  std::array<double, 2> tone_state_{};
  std::array<double, 2> dc_in_{};
  std::array<double, 2> dc_out_{};
  double tilt_coefficient_ = 0.0;
  double dc_pole_ = 0.999;
  PeakMeter meter_;
};

// ---- Filter -------------------------------------------------------------------------

class FilterModule final : public FxModule {
public:
  FilterModule(double base_rate, std::size_t) : base_(base_rate) {
    // One filter per chain factor, so a factor change never re-prepares
    // (allocates) on the callback.
    for (std::size_t i = 0; i < filters_.size(); ++i) {
      filters_[i].prepare(base_rate * static_cast<double>(1U << i));
    }
    meter_.prepare(base_rate);
    set(fx_defaults(FxKind::filter).p, {2.0, base_rate}, 0);
  }

  [[nodiscard]] FxKind kind() const noexcept override { return FxKind::filter; }
  void reset() noexcept override {
    for (auto& f : filters_) {
      f.reset();
    }
    meter_.reset();
  }
  void set(const FxParams& p, const FxContext& context, std::uint32_t frames) noexcept override {
    const std::uint32_t factor = factor_for(context.sample_rate, base_);
    const std::size_t active = factor == 4U ? 2 : (factor == 2U ? 1 : 0);
    if (active != active_) {
      filters_[active].reset();
      active_ = active;
      rate_ = base_ * factor;
      meter_.prepare(rate_);
      frames = 0;
    }
    FilterSettings settings;
    settings.model = static_cast<FilterModel>(choice(p[7], filter_model_count));
    settings.cutoff_hz =
        std::min(log_map(p[0], 8.0, 22'000.0), native_dsp::dsp::nyquist_guard * rate_);
    settings.resonance = clamp01(p[1]);
    settings.drive_db = 24.0 * clamp01(p[2]);
    settings.fat = 0.0;
    filters_[active_].set(settings, std::max<std::uint32_t>(1, frames));
  }
  void process(Frames io) noexcept override {
    auto& filter = filters_[active_];
    for (auto& frame : io) {
      float left = frame.left;
      float right = frame.right;
      filter.process(left, right);
      frame.left = std::isfinite(left) ? left : 0.0F;
      frame.right = std::isfinite(right) ? right : 0.0F;
    }
    meter_.feed(io);
  }
  [[nodiscard]] float telemetry() const noexcept override { return meter_.db(); }
  [[nodiscard]] std::uint64_t tail_frames() const noexcept override {
    // Combs, flangers and self-oscillating resonances ring on.
    return static_cast<std::uint64_t>(std::ceil(1.5 * rate_));
  }

private:
  double base_;
  double rate_ = 48'000.0;
  std::array<VoiceFilter, 3> filters_{};
  std::size_t active_ = 0;
  PeakMeter meter_;
};

} // namespace

std::unique_ptr<FxModule> make_distortion(double base_rate, std::size_t max_frames) {
  auto module = std::make_unique<DistortionModule>(base_rate, max_frames);
  if (!module->ok()) {
    return nullptr;
  }
  return module;
}

std::unique_ptr<FxModule> make_filter(double base_rate, std::size_t max_frames) {
  return std::make_unique<FilterModule>(base_rate, max_frames);
}

} // namespace oscillate::native_dsp::synth::nova::fx_detail
