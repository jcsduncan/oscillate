#pragma once

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"
#include "oscillate/native_dsp/synth/wavetable.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

namespace oscillate::native_dsp::synth {

enum class WarpMode : std::uint8_t { off, bend_plus, bend_minus, sync, mirror, asym, fm };

// Largest instantaneous speed-up a warp applies to the read phase, so the
// oscillator can pick a band-limit that stays below Nyquist while warped.
[[nodiscard]] inline double warp_speed(WarpMode mode, double amount) noexcept {
  amount = std::clamp(amount, 0.0, 1.0);
  switch (mode) {
  case WarpMode::bend_plus:
  case WarpMode::bend_minus:
    return 1.0 + 0.9 * amount;
  case WarpMode::sync:
    return 1.0 + 7.0 * amount;
  case WarpMode::mirror:
    return amount > 0.0 ? std::numbers::pi : 1.0;
  case WarpMode::asym:
    return 1.0 + 0.3 * std::numbers::pi * amount;
  case WarpMode::fm:
    return 1.0 + 4.0 * amount;
  case WarpMode::off:
    break;
  }
  return 1.0;
}

// Reads a generated wavetable with 4-point cubic interpolation, crossfades
// adjacent frames by position and, when the band-limit changes, the old and
// new mip levels over one control interval. Warps reshape the read phase.
class WavetableOscillator {
public:
  static constexpr std::uint32_t level_fade_frames = 32;

  void reset(double phase) noexcept {
    phase_ = phase - std::floor(phase);
    level_ = wavetable_levels;
    previous_ = wavetable_levels;
    fade_ = 0.0;
  }
  [[nodiscard]] double phase() const noexcept { return phase_; }

  // Control rate. increment is cycles per frame at the oscillator's rate.
  // For WarpMode::fm the speed-up depends on the modulator's own rate:
  // callers that know it use set_band_speed() instead.
  void set_band(const Wavetable& table, double increment, WarpMode mode, double amount) noexcept {
    set_band_speed(table, std::fabs(increment) * warp_speed(mode, amount));
  }

  // Control rate. speed is the read phase's peak advance in cycles per frame,
  // warps and phase modulation included: a phase offset m(t) adds
  // max |dm/dt|, which for a modulator of increment f and depth d cycles is
  // 2 pi f d.
  void set_band_speed(const Wavetable& table, double speed) noexcept {
    table_ = &table;
    speed = std::max(1e-9, speed);
    const double allowed = oscillate::native_dsp::dsp::nyquist_guard / speed;
    std::size_t level = wavetable_level_for(allowed);
    if (level_ == wavetable_levels) {
      level_ = level;
      return;
    }
    // Hysteresis: drop harmonics at once when needed (aliasing safety), but
    // only add them back with a 1.5x margin, so modulated pitch or warp near
    // a boundary does not flap between levels.
    if (level<level_&& static_cast<double>(wavetable_level_harmonics(level)) * 1.5> allowed) {
      level = level_;
    }
    if (level != level_) {
      previous_ = level_;
      level_ = level;
      fade_ = 1.0;
    }
  }

  // One frame. position in [0, 1]; fm is the other oscillator's output for
  // WarpMode::fm; offset (cycles) phase-modulates the read before any warp,
  // so an oscillator can be phase-modulated and warped at once. Returns the
  // sample at the new phase.
  [[nodiscard]] double next(double increment, double position, WarpMode mode, double amount,
                            double fm = 0.0, double offset = 0.0) noexcept {
    phase_ += increment;
    phase_ -= fast_floor(phase_);
    amount = std::clamp(amount, 0.0, 1.0);
    position = std::clamp(position, 0.0, 1.0);
    double value = warped(level_, position, mode, amount, fm, offset);
    if (fade_ > 0.0) {
      value += fade_ * (warped(previous_, position, mode, amount, fm, offset) - value);
      fade_ = std::max(0.0, fade_ - 1.0 / level_fade_frames);
    }
    return value;
  }

  // A whole chunk with one warp mode: invariants hoisted out of the loop.
  // increment advances by slope before each frame (like a Ramp); position
  // and amount are per frame; fm is constant over the chunk. Output is
  // written to out. A pending mip crossfade uses the per-frame path.
  void process(double* out, std::size_t frames, double& increment, double slope,
               const double* position, const double* amount, WarpMode mode, double fm) noexcept {
    if (fade_ > 0.0) {
      for (std::size_t i = 0; i < frames; ++i) {
        increment += slope;
        out[i] = next(increment, position[i], mode, amount[i], fm);
      }
      return;
    }
    switch (mode) {
    case WarpMode::off:
      run<WarpMode::off>(out, frames, increment, slope, position, amount, fm);
      return;
    case WarpMode::bend_plus:
      run<WarpMode::bend_plus>(out, frames, increment, slope, position, amount, fm);
      return;
    case WarpMode::bend_minus:
      run<WarpMode::bend_minus>(out, frames, increment, slope, position, amount, fm);
      return;
    case WarpMode::sync:
      run<WarpMode::sync>(out, frames, increment, slope, position, amount, fm);
      return;
    case WarpMode::mirror:
      run<WarpMode::mirror>(out, frames, increment, slope, position, amount, fm);
      return;
    case WarpMode::asym:
      run<WarpMode::asym>(out, frames, increment, slope, position, amount, fm);
      return;
    case WarpMode::fm:
      run<WarpMode::fm>(out, frames, increment, slope, position, amount, fm);
      return;
    }
  }

private:
  // Same arithmetic as warped() with the mode fixed at compile time.
  template <WarpMode Mode>
  void run(double* out, std::size_t frames, double& increment, double slope, const double* position,
           const double* amount, double fm) noexcept {
    constexpr double two_pi = 2.0 * std::numbers::pi;
    const auto& band = table_->levels[level_];
    double phase = phase_;
    for (std::size_t i = 0; i < frames; ++i) {
      increment += slope;
      phase += increment;
      if (phase >= 1.0) {
        phase -= fast_floor(phase);
      }
      const double frame_position = std::clamp(position[i], 0.0, 1.0) * (wavetable_frames - 1);
      const auto first = static_cast<std::size_t>(frame_position);
      const std::size_t second = std::min(first + 1, wavetable_frames - 1);
      const double mix = frame_position - static_cast<double>(first);
      const double a = std::clamp(amount[i], 0.0, 1.0);
      const auto wrap = [](double x) { return x - fast_floor(x); };
      if constexpr (Mode == WarpMode::off) {
        out[i] = read_band(band, phase, first, second, mix);
      } else if constexpr (Mode == WarpMode::bend_plus) {
        out[i] =
            read_band(band, wrap(phase - 0.9 * a / two_pi * sin_cycle(phase)), first, second, mix);
      } else if constexpr (Mode == WarpMode::bend_minus) {
        out[i] =
            read_band(band, wrap(phase + 0.9 * a / two_pi * sin_cycle(phase)), first, second, mix);
      } else if constexpr (Mode == WarpMode::sync) {
        const double window = 1.0 - std::min(1.0, 2.0 * a) * (1.0 - sin_cycle(0.5 * phase));
        out[i] = window * read_band(band, wrap(phase * (1.0 + 7.0 * a)), first, second, mix);
      } else if constexpr (Mode == WarpMode::mirror) {
        const double plain = read_band(band, phase, first, second, mix);
        const double mirrored = 0.5 - 0.5 * sin_cycle(phase + 0.25);
        out[i] = plain + a * (read_band(band, wrap(mirrored), first, second, mix) - plain);
      } else if constexpr (Mode == WarpMode::asym) {
        const double s = sin_cycle(0.5 * phase);
        out[i] = read_band(band, wrap(phase + 0.3 * a * s * s), first, second, mix);
      } else {
        out[i] = read_band(band, wrap(phase + 2.0 * a * fm), first, second, mix);
      }
    }
    phase_ = phase;
  }

  [[nodiscard]] double warped(std::size_t level, double position, WarpMode mode, double amount,
                              double fm, double offset = 0.0) const noexcept {
    constexpr double two_pi = 2.0 * std::numbers::pi;
    const double p = offset == 0.0 ? phase_ : phase_ + offset - fast_floor(phase_ + offset);
    switch (mode) {
    case WarpMode::bend_plus:
      return read(level, p - 0.9 * amount / two_pi * sin_cycle(p), position);
    case WarpMode::bend_minus:
      return read(level, p + 0.9 * amount / two_pi * sin_cycle(p), position);
    case WarpMode::sync: {
      const double ratio = 1.0 + 7.0 * amount;
      // The window closes the slave's jump at the master wrap; its strength
      // grows with the jump, so a small amount barely colours the tone.
      const double window =
          1.0 - std::min(1.0, 2.0 * amount) * (1.0 - std::sin(std::numbers::pi * p));
      return window * read(level, p * ratio, position);
    }
    case WarpMode::mirror: {
      // Forward then back through the cycle with a smooth turnaround: a
      // raised-cosine read phase has no corner to alias.
      const double mirrored = 0.5 - 0.5 * sin_cycle(p + 0.25);
      const double plain = read(level, p, position);
      return plain + amount * (read(level, mirrored, position) - plain);
    }
    case WarpMode::asym: {
      // Shifts the cycle's midpoint earlier with a smooth, periodic remap,
      // so there is no knee to alias and the slope matches across the wrap.
      const double s = sin_cycle(0.5 * p);
      return read(level, p + 0.3 * amount * s * s, position);
    }
    case WarpMode::fm:
      return read(level, p + 2.0 * amount * fm, position);
    case WarpMode::off:
      break;
    }
    return read(level, p, position);
  }

  [[nodiscard]] double read(std::size_t level, double p, double position) const noexcept {
    const auto& band = table_->levels[level];
    p -= fast_floor(p);
    const double frame_position = position * (wavetable_frames - 1);
    const auto first = static_cast<std::size_t>(frame_position);
    const std::size_t second = std::min(first + 1, wavetable_frames - 1);
    const double mix = frame_position - static_cast<double>(first);
    return read_band(band, p, first, second, mix);
  }

  // The cubic is linear in its samples, so blending the two frames' samples
  // first and interpolating once equals crossfading two cubic reads.
  [[nodiscard]] static double read_band(const WavetableLevel& band, double p, std::size_t first,
                                        std::size_t second, double mix) noexcept {
    const double x = p * band.length;
    // A phase within rounding of 1.0 can round up to length; stay in guard.
    const auto index = std::min(static_cast<std::size_t>(x), std::size_t{band.length} - 1);
    const double t = x - static_cast<double>(index);
    const float* a = band.frame(first) + index;
    const float* b = band.frame(second) + index;
    const double y0 = a[-1] + mix * (b[-1] - a[-1]);
    const double y1 = a[0] + mix * (b[0] - a[0]);
    const double y2 = a[1] + mix * (b[1] - a[1]);
    const double y3 = a[2] + mix * (b[2] - a[2]);
    const double c1 = 0.5 * (y2 - y0);
    const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
    const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
  }

  const Wavetable* table_ = nullptr;
  double phase_ = 0.0;
  std::size_t level_ = wavetable_levels;
  std::size_t previous_ = wavetable_levels;
  double fade_ = 0.0;
};

} // namespace oscillate::native_dsp::synth
