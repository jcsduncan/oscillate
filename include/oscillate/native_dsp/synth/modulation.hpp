#pragma once

#include "oscillate/native_dsp/synth/shaping.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

namespace oscillate::native_dsp::synth {

// Reaches its target in exactly the requested number of frames, so automation
// and live edits are deterministic across block splits.
class LinearRamp {
public:
  void reset(double value) noexcept {
    value_ = value;
    target_ = value;
    step_ = 0.0;
    remaining_ = 0;
  }
  void set_target(double target, std::uint32_t frames) noexcept {
    if (target == target_ && (remaining_ > 0 || value_ == target)) {
      return;
    }
    target_ = target;
    if (frames == 0) {
      value_ = target;
      remaining_ = 0;
      return;
    }
    remaining_ = frames;
    step_ = (target - value_) / frames;
  }
  [[nodiscard]] double next() noexcept {
    if (remaining_ > 0) {
      value_ = --remaining_ == 0 ? target_ : value_ + step_;
    }
    return value_;
  }
  [[nodiscard]] double value() const noexcept { return value_; }
  [[nodiscard]] double target() const noexcept { return target_; }
  [[nodiscard]] bool settled() const noexcept { return remaining_ == 0; }

private:
  double value_ = 0.0;
  double target_ = 0.0;
  double step_ = 0.0;
  std::uint32_t remaining_ = 0;
};

inline constexpr double min_envelope_seconds = 0.0005;
inline constexpr double max_envelope_seconds = 20.0;

struct AdsrSettings {
  double attack_seconds = 0.005;
  double decay_seconds = 0.3;
  double sustain = 0.7; // 0..1
  double release_seconds = 0.3;
  double curve = 0.5; // 0 is linear; 1 is strongly exponential (RC-like).
};

// Each timed segment reaches its end value in exactly round(seconds * rate)
// frames for every curve. Attack and release start from the current level,
// so retriggers and early releases never step.
class Adsr {
public:
  enum class Stage : std::uint8_t { idle, attack, decay, sustain, release };

  void prepare(double sample_rate) noexcept {
    rate_ = sample_rate;
    sustain_glide_ = 1.0 - std::exp(-1.0 / (0.005 * sample_rate));
  }

  void reset() noexcept {
    stage_ = Stage::idle;
    value_ = 0.0;
    remaining_ = 0;
  }

  // Live edit. A running segment restarts from the current level with the new
  // time, so the change is smooth and still lands on its end value.
  void set(const AdsrSettings& settings) noexcept {
    const bool timing_changed = settings.attack_seconds != settings_.attack_seconds ||
                                settings.decay_seconds != settings_.decay_seconds ||
                                settings.release_seconds != settings_.release_seconds ||
                                settings.curve != settings_.curve;
    const bool sustain_changed = settings.sustain != settings_.sustain;
    settings_ = settings;
    if (stage_ == Stage::attack && timing_changed) {
      begin(Stage::attack, 1.0, settings_.attack_seconds);
    } else if (stage_ == Stage::decay && (timing_changed || sustain_changed)) {
      begin(Stage::decay, settings_.sustain, settings_.decay_seconds);
    } else if (stage_ == Stage::release && timing_changed) {
      begin(Stage::release, 0.0, settings_.release_seconds);
    }
  }

  void gate_on() noexcept { begin(Stage::attack, 1.0, settings_.attack_seconds); }
  void gate_off() noexcept {
    if (stage_ != Stage::idle && stage_ != Stage::release) {
      begin(Stage::release, 0.0, settings_.release_seconds);
    }
  }

  [[nodiscard]] double next() noexcept {
    switch (stage_) {
    case Stage::idle:
      return 0.0;
    case Stage::sustain:
      value_ += (settings_.sustain - value_) * sustain_glide_;
      return value_;
    default:
      break;
    }
    if (--remaining_ == 0) {
      value_ = end_;
      if (stage_ == Stage::attack) {
        begin(Stage::decay, settings_.sustain, settings_.decay_seconds);
      } else if (stage_ == Stage::decay) {
        stage_ = Stage::sustain;
      } else {
        stage_ = Stage::idle;
      }
      return value_;
    }
    value_ = linear_ ? value_ + step_ : target_ + (value_ - target_) * coefficient_;
    return value_;
  }

  [[nodiscard]] Stage stage() const noexcept { return stage_; }
  [[nodiscard]] double value() const noexcept { return value_; }
  [[nodiscard]] bool active() const noexcept { return stage_ != Stage::idle; }

  [[nodiscard]] static double clamp_seconds(double seconds) noexcept {
    return seconds > min_envelope_seconds
               ? (seconds < max_envelope_seconds ? seconds : max_envelope_seconds)
               : min_envelope_seconds;
  }

private:
  void begin(Stage stage, double end, double seconds) noexcept {
    stage_ = stage;
    end_ = end;
    const double frames = std::round(clamp_seconds(seconds) * rate_);
    remaining_ = frames > 1.0 ? static_cast<std::uint32_t>(frames) : 1U;
    const double range = end - value_;
    linear_ = settings_.curve < 0.001;
    if (linear_) {
      step_ = range / remaining_;
      return;
    }
    // Approach an overshoot target so the curve lands on end after exactly
    // remaining_ frames: |end - target| = r * |range| and c^N = r / (1 + r).
    const double overshoot = std::pow(10.0, 1.0 - 4.0 * settings_.curve);
    target_ = end + overshoot * range;
    coefficient_ = std::pow(overshoot / (1.0 + overshoot), 1.0 / remaining_);
  }

  AdsrSettings settings_{};
  Stage stage_ = Stage::idle;
  double rate_ = 48'000.0;
  double sustain_glide_ = 0.004;
  double value_ = 0.0;
  double end_ = 0.0;
  double target_ = 0.0;
  double coefficient_ = 0.0;
  double step_ = 0.0;
  bool linear_ = true;
  std::uint32_t remaining_ = 0;
};

// Value of a step sequence at a phase in [0, 1): `count` steps from `steps`.
// glide (0..1) ramps into the next step over that fraction of each step, so
// sequences can be hard-stepped or slurred. The last step glides to the first.
[[nodiscard]] inline double step_value(const double* steps, std::size_t count, double phase,
                                       double glide) noexcept {
  if (count == 0) {
    return 0.0;
  }
  phase -= std::floor(phase);
  const double position = phase * static_cast<double>(count);
  const auto index = std::min(static_cast<std::size_t>(position), count - 1);
  const double within = position - static_cast<double>(index);
  const double current = steps[index];
  if (glide <= 0.0 || within < 1.0 - glide) {
    return current;
  }
  const double next = steps[(index + 1) % count];
  const double t = (within - (1.0 - glide)) / glide;
  return current + (next - current) * t * t * (3.0 - 2.0 * t);
}

enum class LfoShape : std::uint8_t { sine, triangle, saw_up, saw_down, square, sample_hold };

// Bipolar LFO in [-1, 1]. A ~1 ms slew removes the steps of discontinuous
// shapes so they never click when they modulate amplitude or cutoff.
class Lfo {
public:
  void prepare(double sample_rate) noexcept {
    slew_ = 1.0 - std::exp(-1.0 / (0.001 * sample_rate));
  }
  void reset(double phase, std::uint32_t seed) noexcept {
    phase_ = phase - std::floor(phase);
    noise_.reset(seed);
    held_ = noise_.white();
    output_ = shape_value();
    primed_ = false;
  }
  void set_shape(LfoShape shape) noexcept { shape_ = shape; }
  [[nodiscard]] double phase() const noexcept { return phase_; }

  // increment is cycles per frame.
  [[nodiscard]] double next(double increment) noexcept {
    phase_ += increment;
    if (phase_ >= 1.0) {
      phase_ -= std::floor(phase_);
      held_ = noise_.white();
    }
    return slewed();
  }

  // Position-locked step: cycles is an absolute cycle count (for example song
  // beats over the division), so the LFO follows the transport exactly.
  [[nodiscard]] double next_at(double cycles) noexcept {
    const double whole = std::floor(cycles);
    if (whole != cycle_) {
      cycle_ = whole;
      held_ = noise_.white();
    }
    phase_ = cycles - whole;
    return slewed();
  }

private:
  [[nodiscard]] double slewed() noexcept {
    const double raw = shape_value();
    if (!primed_) {
      output_ = raw;
      primed_ = true;
    } else {
      output_ += (raw - output_) * slew_;
    }
    return output_;
  }

  [[nodiscard]] double shape_value() const noexcept {
    switch (shape_) {
    case LfoShape::sine:
      return sin_cycle(phase_);
    case LfoShape::triangle:
      return phase_ < 0.25 ? 4.0 * phase_
                           : (phase_ < 0.75 ? 2.0 - 4.0 * phase_ : 4.0 * phase_ - 4.0);
    case LfoShape::saw_up:
      return 2.0 * phase_ - 1.0;
    case LfoShape::saw_down:
      return 1.0 - 2.0 * phase_;
    case LfoShape::square:
      return phase_ < 0.5 ? 1.0 : -1.0;
    case LfoShape::sample_hold:
      return held_;
    }
    return 0.0;
  }

  LfoShape shape_ = LfoShape::sine;
  NoiseSource noise_;
  double phase_ = 0.0;
  double held_ = 0.0;
  double cycle_ = -1.0;
  double output_ = 0.0;
  double slew_ = 0.02;
  bool primed_ = false;
};

} // namespace oscillate::native_dsp::synth
