#pragma once

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace oscillate::native_dsp::synth {

// Slow, smooth, seeded random wander in [-1, 1], like component drift in an
// analog voice. A new random target is drawn every 1 / rate seconds and a
// two-pole smoother follows it, so the output has no steps or corners. Equal
// seeds and call sequences give identical output, so playback and export
// agree. Step at control rate or audio rate; prepare with that rate.
class DriftSource {
public:
  void prepare(double step_rate, double drift_hz) noexcept {
    step_rate_ = step_rate;
    set_rate(drift_hz);
  }
  void set_rate(double drift_hz) noexcept {
    drift_hz = drift_hz > 0.001 ? drift_hz : 0.001;
    hold_steps_ = static_cast<std::uint32_t>(std::max(1.0, step_rate_ / drift_hz));
    smoothing_ = oscillate::native_dsp::dsp::one_pole_coefficient(drift_hz, step_rate_);
  }
  void reset(std::uint32_t seed) noexcept {
    noise_.reset(seed);
    target_ = noise_.white();
    // Start settled so the first notes are already "aged", not gliding in.
    first_ = target_;
    value_ = target_;
    remaining_ = hold_steps_;
  }
  [[nodiscard]] double next() noexcept {
    if (--remaining_ == 0) {
      target_ = noise_.white();
      remaining_ = hold_steps_;
    }
    first_ += (target_ - first_) * smoothing_;
    value_ += (first_ - value_) * smoothing_;
    return value_;
  }
  [[nodiscard]] double value() const noexcept { return value_; }

private:
  NoiseSource noise_{};
  double step_rate_ = 1'500.0;
  double smoothing_ = 0.001;
  std::uint32_t hold_steps_ = 1'500;
  std::uint32_t remaining_ = 1'500;
  double target_ = 0.0;
  double first_ = 0.0;
  double value_ = 0.0;
};

// Fixed per-slot tolerances in [-1, 1]: how a voice card differs from the
// others (detune, cutoff, envelope speed). Pure function of slot and index.
[[nodiscard]] inline double voice_tolerance(std::uint32_t slot, std::uint32_t index) noexcept {
  std::uint32_t x = (slot + 1U) * 0x9E3779B1U ^ (index + 1U) * 0x85EBCA77U;
  x ^= x >> 15U;
  x *= 0x2C1B3C6DU;
  x ^= x >> 12U;
  x *= 0x297A2D39U;
  x ^= x >> 15U;
  return static_cast<double>(x) * (2.0 / 4294967296.0) - 1.0;
}

} // namespace oscillate::native_dsp::synth
