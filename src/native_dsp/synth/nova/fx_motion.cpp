#include "fx_modules.hpp"

#include "oscillate/native_dsp/synth/crossfade.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>

// Nova's Flanger, Dimension and Hyper modules (08a §5): modulated delay
// lines at the chain's rate, sized at creation for 4x the engine's rate.
namespace oscillate::native_dsp::synth::nova::fx_detail {
namespace {

[[nodiscard]] std::size_t line_frames(double base_rate, double seconds) noexcept {
  return static_cast<std::size_t>(std::ceil(seconds * base_rate * max_factor)) + 16;
}
[[nodiscard]] double wrap(double phase) noexcept { return phase - std::floor(phase); }
[[nodiscard]] double triangle(double phase) noexcept {
  return 4.0 * std::fabs(wrap(phase - 0.25) - 0.5) - 1.0;
}

// ---- Flanger ------------------------------------------------------------------------

// Classic: the swept tap alone (the rack's mix adds the dry, so mix 50% is
// the deepest flange), sweeping 0.15 ms to 7 ms on a triangle. Through-zero:
// the module mixes its own reference, the input delayed 4 ms, with a tap
// sweeping either side of it, so the notches run through zero delay; its
// output is the whole flanged signal. Feedback is soft-limited in the loop.
constexpr double flanger_min_ms = 0.15;
constexpr double flanger_sweep_ms = 7.0;
constexpr double reference_ms = 4.0;

class FlangerModule final : public FxModule {
public:
  FlangerModule(double base_rate, std::size_t) : base_(base_rate) {
    for (std::size_t c = 0; c < 2; ++c) {
      loop_[c].prepare(line_frames(base_rate, 0.02));
      plain_[c].prepare(line_frames(base_rate, 0.02));
    }
    rate(base_rate);
    set(fx_defaults(FxKind::flanger).p, {2.0, base_rate}, 0);
  }
  [[nodiscard]] FxKind kind() const noexcept override { return FxKind::flanger; }
  void reset() noexcept override {
    for (std::size_t c = 0; c < 2; ++c) {
      loop_[c].reset();
      plain_[c].reset();
    }
    phase_ = 0.0;
    fade_.stop();
  }
  void set(const FxParams& p, const FxContext& context, std::uint32_t frames) noexcept override {
    const std::uint32_t factor = factor_for(context.sample_rate, base_);
    if (base_ * factor != rate_) {
      rate(base_ * factor);
    }
    hz_ = log_map(p[0], 0.01, 10.0);
    depth_.to(clamp01(p[1]), frames);
    feedback_.to(1.9 * clamp01(p[2]) - 0.95, frames);
    const bool through_zero = choice(p[7], 2) == 1;
    if (through_zero != through_zero_) {
      if (primed_) {
        old_through_zero_ = through_zero_;
        fade_.start();
      }
      through_zero_ = through_zero;
    }
    primed_ = true;
  }
  void process(Frames io) noexcept override {
    const double ms = rate_ * 1e-3;
    const double increment = hz_ / rate_;
    for (auto& frame : io) {
      const double depth = depth_.next();
      const double feedback = feedback_.next();
      const double weight = fade_.next();
      const std::array<double, 2> x{frame.left, frame.right};
      std::array<double, 2> out{};
      for (std::size_t c = 0; c < 2; ++c) {
        const double phase = phase_ + (c == 1 ? 0.25 : 0.0);
        const double classic_delay =
            (flanger_min_ms + flanger_sweep_ms * depth * (0.5 + 0.5 * triangle(phase))) * ms;
        const double zero_delay = reference_ms * (1.0 + depth * triangle(phase)) * ms;
        const double classic = loop_[c].read(std::max(1.0, classic_delay));
        const double swept = loop_[c].read(std::max(1.0, zero_delay));
        const double zero = 0.5 * (plain_[c].read(reference_ms * ms) + swept);
        const double fresh = through_zero_ ? zero : classic;
        out[c] = fresh;
        if (weight > 0.0) {
          const double old = old_through_zero_ ? zero : classic;
          out[c] = fresh + weight * (old - fresh);
        }
        const double tap = through_zero_ ? swept : classic;
        loop_[c].write(x[c] + soft_clip(feedback * tap));
        plain_[c].write(x[c]);
      }
      phase_ = wrap(phase_ + increment);
      frame.left = static_cast<float>(out[0]);
      frame.right = static_cast<float>(out[1]);
    }
  }
  [[nodiscard]] float telemetry() const noexcept override { return static_cast<float>(phase_); }
  [[nodiscard]] std::uint64_t tail_frames() const noexcept override {
    const double feedback = std::min(0.95, std::fabs(feedback_.target));
    const double repeats = feedback > 0.01 ? std::log(1e-4) / std::log(feedback) : 1.0;
    const double seconds = 0.03 + repeats * 0.009;
    return static_cast<std::uint64_t>(std::ceil(seconds * rate_));
  }

private:
  void rate(double rate) noexcept {
    rate_ = rate;
    fade_.prepare(rate);
  }

  double base_;
  double rate_ = 0.0;
  std::array<DelayLine, 2> loop_{};
  std::array<DelayLine, 2> plain_{};
  Glide depth_;
  Glide feedback_;
  SwitchFade fade_;
  double hz_ = 0.5;
  double phase_ = 0.0;
  bool through_zero_ = false;
  bool old_through_zero_ = false;
  bool primed_ = false;
};

// ---- Dimension ----------------------------------------------------------------------

// The classic stereo widener: two taps of the mono sum, modulated in
// opposition, whose difference (above 250 Hz) is added to the side. The mono
// sum is untouched, so it folds down perfectly. Its output is the whole
// widened signal; the rack's mix sets how much width is added.
constexpr double dimension_highpass_hz = 250.0;

class DimensionModule final : public FxModule {
public:
  DimensionModule(double base_rate, std::size_t) : base_(base_rate) {
    line_.prepare(line_frames(base_rate, 0.04));
    rate(base_rate);
    set(fx_defaults(FxKind::dimension).p, {2.0, base_rate}, 0);
  }
  [[nodiscard]] FxKind kind() const noexcept override { return FxKind::dimension; }
  void reset() noexcept override {
    line_.reset();
    highpass_.reset();
    phase_ = 0.0;
    meter_.reset();
  }
  void set(const FxParams& p, const FxContext& context, std::uint32_t frames) noexcept override {
    const std::uint32_t factor = factor_for(context.sample_rate, base_);
    if (base_ * factor != rate_) {
      rate(base_ * factor);
    }
    const double size = clamp01(p[0]);
    const double motion = clamp01(p[2]);
    first_ms_.to(6.0 + 10.0 * size, frames);
    second_ms_.to(9.0 + 14.0 * size, frames);
    swing_ms_.to(0.15 + 1.6 * motion, frames);
    width_.to(1.4 * clamp01(p[1]), frames);
    hz_ = 0.2 + 0.8 * motion;
  }
  void process(Frames io) noexcept override {
    const double ms = rate_ * 1e-3;
    const double increment = hz_ / rate_;
    for (auto& frame : io) {
      const double first = first_ms_.next() * ms;
      const double second = second_ms_.next() * ms;
      const double swing = swing_ms_.next() * ms;
      const double width = width_.next();
      const double mono = 0.5 * (static_cast<double>(frame.left) + frame.right);
      line_.write(mono);
      const double a = line_.read(first + swing * (1.0 + sin_cycle(phase_)));
      const double b = line_.read(second + swing * (1.0 + sin_cycle(phase_ + 0.5)));
      const double side = highpass_.process(0.5 * (a - b), highpass_c_).high * width;
      phase_ = wrap(phase_ + increment);
      frame.left = static_cast<float>(frame.left + side);
      frame.right = static_cast<float>(frame.right - side);
    }
    meter_.feed(io);
  }
  [[nodiscard]] float telemetry() const noexcept override { return meter_.db(); }
  [[nodiscard]] std::uint64_t tail_frames() const noexcept override {
    return static_cast<std::uint64_t>(std::ceil(0.06 * rate_));
  }

private:
  void rate(double rate) noexcept {
    rate_ = rate;
    highpass_c_ = SvfCoefficients::make(prewarp(dimension_highpass_hz / rate), std::numbers::sqrt2);
    meter_.prepare(rate);
  }

  double base_;
  double rate_ = 0.0;
  DelayLine line_;
  ZdfSvf highpass_;
  SvfCoefficients highpass_c_{};
  Glide first_ms_;
  Glide second_ms_;
  Glide swing_ms_;
  Glide width_;
  double hz_ = 0.5;
  double phase_ = 0.0;
  PeakMeter meter_;
};

// ---- Hyper --------------------------------------------------------------------------

// A unison doubler: 2..7 copies of the input, each a two-head pitch shifter
// (30 ms crossfaded windows) detuned symmetrically up to +-50 cents, with a
// slow drift on each voice's detune and a constant-power pan spread.
constexpr std::size_t max_voices = 7;
constexpr double window_ms = 30.0;
constexpr double head_ms = 1.5;
constexpr double drift = 0.25;

class HyperModule final : public FxModule {
public:
  HyperModule(double base_rate, std::size_t) : base_(base_rate) {
    line_.prepare(line_frames(base_rate, (window_ms + head_ms + 5.0) * 1e-3));
    rate(base_rate);
    reset();
    set(fx_defaults(FxKind::hyper).p, {2.0, base_rate}, 0);
  }
  [[nodiscard]] FxKind kind() const noexcept override { return FxKind::hyper; }
  void reset() noexcept override {
    line_.reset();
    lfo_ = 0.0;
    for (std::size_t v = 0; v < max_voices; ++v) {
      voices_[v].window = static_cast<double>(v) / static_cast<double>(max_voices);
    }
  }
  void set(const FxParams& p, const FxContext& context, std::uint32_t frames) noexcept override {
    const std::uint32_t factor = factor_for(context.sample_rate, base_);
    if (base_ * factor != rate_) {
      rate(base_ * factor);
    }
    hz_ = log_map(p[1], 0.05, 5.0);
    detune_.to(50.0 * clamp01(p[0]), frames);
    const std::size_t count = 2 + choice(p[7], 6);
    const double width = clamp01(p[2]);
    const std::uint32_t fade = primed_ ? static_cast<std::uint32_t>(0.02 * rate_) : 0U;
    // Equal power over the sounding voices.
    const double level = std::sqrt(2.0 / static_cast<double>(count));
    for (std::size_t v = 0; v < max_voices; ++v) {
      auto& voice = voices_[v];
      const bool on = v < count;
      const double spread =
          on ? -1.0 + 2.0 * static_cast<double>(v) / static_cast<double>(count - 1) : 0.0;
      voice.spread.to(spread, fade);
      // Alternate the pan against the detune so sharp and flat voices sit
      // on both sides.
      const double pan = width * (v % 2 == 0 ? spread : -spread);
      const double angle = (pan + 1.0) * 0.25 * std::numbers::pi;
      voice.left.to(on ? level * std::cos(angle) : 0.0, fade);
      voice.right.to(on ? level * std::sin(angle) : 0.0, fade);
    }
    primed_ = true;
  }
  void process(Frames io) noexcept override {
    const double ms = rate_ * 1e-3;
    const double window = window_ms * ms;
    const double head = head_ms * ms;
    const double increment = hz_ / rate_;
    for (auto& frame : io) {
      line_.write(0.5 * (static_cast<double>(frame.left) + frame.right));
      const double detune = detune_.next();
      double left = 0.0;
      double right = 0.0;
      for (std::size_t v = 0; v < max_voices; ++v) {
        auto& voice = voices_[v];
        const double spread = voice.spread.next();
        const double gl = voice.left.next();
        const double gr = voice.right.next();
        if (gl == 0.0 && gr == 0.0) {
          continue;
        }
        const double wobble = 1.0 + drift * sin_cycle(lfo_ + static_cast<double>(v) /
                                                                 static_cast<double>(max_voices));
        const double ratio = std::exp2(detune * spread * wobble / 1'200.0);
        voice.window = wrap(voice.window + (1.0 - ratio) / window);
        const double other = wrap(voice.window + 0.5);
        const double a = std::sin(pi * voice.window);
        const double gain_a = a * a;
        const double y = gain_a * line_.read(head + voice.window * window) +
                         (1.0 - gain_a) * line_.read(head + other * window);
        left += gl * y;
        right += gr * y;
      }
      lfo_ = wrap(lfo_ + increment);
      frame.left = static_cast<float>(left);
      frame.right = static_cast<float>(right);
    }
  }
  [[nodiscard]] float telemetry() const noexcept override { return static_cast<float>(lfo_); }
  [[nodiscard]] std::uint64_t tail_frames() const noexcept override {
    return static_cast<std::uint64_t>(std::ceil((window_ms + head_ms + 10.0) * 1e-3 * rate_));
  }

private:
  struct Voice {
    double window = 0.0; // Read-window phase 0..1.
    Glide spread;
    Glide left;
    Glide right;
  };
  void rate(double rate) noexcept { rate_ = rate; }

  double base_;
  double rate_ = 0.0;
  DelayLine line_;
  std::array<Voice, max_voices> voices_{};
  Glide detune_;
  double hz_ = 0.5;
  double lfo_ = 0.0;
  bool primed_ = false;
};

} // namespace

std::unique_ptr<FxModule> make_flanger(double base_rate, std::size_t max_frames) {
  return std::make_unique<FlangerModule>(base_rate, max_frames);
}
std::unique_ptr<FxModule> make_dimension(double base_rate, std::size_t max_frames) {
  return std::make_unique<DimensionModule>(base_rate, max_frames);
}
std::unique_ptr<FxModule> make_hyper(double base_rate, std::size_t max_frames) {
  return std::make_unique<HyperModule>(base_rate, max_frames);
}

} // namespace oscillate::native_dsp::synth::nova::fx_detail
