#pragma once

#include "effects/dust/parts.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

// Dust's age (ADR-0069): the input on its way through. A worn record's band
// and drive, wow and flutter about the delay the effect reports as its
// latency, tape's dropouts darkening it, and lows folded to mono.
namespace oscillate::native_dsp::effects::dust::detail {

// A worn record's band: a high-pass rising from age_high_hz and a low-pass
// falling from age_low_hz with Age, blended in over the first
// 1 / age_blend of the knob so Age 0 is the input exactly. The same blend
// brings in a gentle tanh drive.
inline constexpr double age_high_hz = 20.0;
inline constexpr double age_high_ratio = 12.0;
inline constexpr double age_low_hz = 20'000.0;
inline constexpr double age_low_ratio = 0.17;
inline constexpr double age_blend = 4.0;
inline constexpr double age_drive = 3.0;
// Wow: a sine at wow_hz (wow_sine of it) and a slow random drift; flutter:
// two sines (6.3 and 9.7 Hz) whose depth wanders by flutter_wander.
inline constexpr double wow_sine = 0.8;
inline constexpr double wow_drift = 0.2;
inline constexpr double wow_drift_seconds = 3.0;
inline constexpr std::array<double, 2> flutter_hz{6.3, 9.7};
inline constexpr std::array<double, 2> flutter_share{0.6, 0.4};
inline constexpr double flutter_wander = 0.3;
inline constexpr double flutter_wander_seconds = 0.4;
// Tape dropouts take the input's highs down to this.
inline constexpr double dropout_dark_hz = 2'500.0;
// The delay holds twice the latency (the base delay and the deepest wow and
// flutter about it) and this many frames more for its interpolator.
inline constexpr std::size_t delay_spare_frames = 8;

class Age {
public:
  // Control only.
  void prepare(double rate, std::uint32_t latency);
  void reseed(std::uint64_t key) noexcept { rng_.seed(key); }
  void reset() noexcept;
  void set(std::span<const double> v, double steps, bool jump) noexcept;

  // One frame of the input, aged in place. `frame` (the project frame) sets
  // the wow and flutter phases; `dropout` is tape's dip (0 outside one).
  void next(std::array<double, channels>& x, std::int64_t frame, double dropout) noexcept {
    // Wow and flutter about the base delay.
    const double wow_phase = std::fmod(static_cast<double>(frame) * wow_hz, rate_) / rate_;
    double modulation = wow_.next() * (wow_sine * std::sin(two_pi * wow_phase) +
                                       wow_drift * wow_drift_.next(rng_, wow_drift_seconds, rate_));
    const double flutter = flutter_.next();
    if (flutter > 0.0) {
      double sum = 0.0;
      for (std::size_t k = 0; k < flutter_hz.size(); ++k) {
        const double phase = std::fmod(static_cast<double>(frame) * flutter_hz[k], rate_) / rate_;
        sum += flutter_share[k] * std::sin(two_pi * phase + static_cast<double>(k));
      }
      modulation +=
          flutter * sum *
          (1.0 + flutter_wander * flutter_wander_.next(rng_, flutter_wander_seconds, rate_));
    }
    const double delay = static_cast<double>(latency_) + modulation;
    const double blend = blend_.next();
    for (std::size_t c = 0; c < channels; ++c) {
      delay_[c].write(x[c]);
      double y = modulation == 0.0 ? delay_[c].sample(latency_) : delay_[c].read(delay);
      if (blend > 0.0) {
        const double band = low_[c].low(high_[c].high(y));
        const double driven = std::tanh(drive_ * band) / drive_;
        y += blend * (driven - y);
      }
      if (dropout > 0.0) {
        y += dropout * (dark_[c].low(y) - y);
        y *= 1.0 - dropout;
      } else {
        dark_[c].state = y; // Kept in step, so a dip starts without a step.
      }
      x[c] = y;
    }
    if (mono_on_) {
      const double mid = 0.5 * (x[0] + x[1]);
      const double side = mono_high_[1].high(mono_high_[0].high(0.5 * (x[0] - x[1])));
      x[0] = mid + side;
      x[1] = mid - side;
    }
  }

private:
  double rate_ = nominal_rate;
  std::uint32_t latency_ = 0;
  std::array<dsp::Delay, channels> delay_{};
  std::array<Svf, channels> high_{};
  std::array<Svf, channels> low_{};
  std::array<Svf, channels> mono_high_{};
  std::array<OnePole, channels> dark_{};
  dsp::Ramp blend_;
  dsp::Ramp wow_;
  dsp::Ramp flutter_;
  Drift wow_drift_;
  Drift flutter_wander_;
  Rng rng_;
  double amount_set_ = -1.0;
  double drive_ = 1.0;
  double wow_frames_ = 0.0;
  double flutter_frames_ = 0.0;
  double mono_set_ = -1.0;
  bool mono_on_ = false;
};

} // namespace oscillate::native_dsp::effects::dust::detail
