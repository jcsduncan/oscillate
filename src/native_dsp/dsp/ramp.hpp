#pragma once

#include <cmath>
#include <cstddef>

// The one linear parameter ramp shared by the native synths and effects.
namespace oscillate::native_dsp::dsp {

// Moves `value` by `step` each frame. to() aims at a target over `steps`
// frames (a jump when asked, or when `steps` is not positive); the ramp then
// keeps its step until the next to(), so callers re-aim it every control
// tick. settle() is to() that lands exactly on the target once within
// 1e-12 of it, so a stage at rest stays bit-exact after a move.
struct Ramp {
  double value = 0.0;
  double step = 0.0;

  void to(double target, double steps, bool jump) noexcept {
    if (jump || steps <= 0.0) {
      value = target;
      step = 0.0;
    } else {
      step = (target - value) / steps;
    }
  }
  void settle(double target, double steps, bool jump) noexcept {
    if (jump || std::fabs(target - value) < 1e-12) {
      value = target;
      step = 0.0;
      return;
    }
    to(target, steps, false);
  }
  double next() noexcept {
    value += step;
    return value;
  }
  void fill(double* out, std::size_t frames) noexcept {
    for (std::size_t i = 0; i < frames; ++i) {
      out[i] = next();
    }
  }
  void skip(std::size_t frames) noexcept { value += step * static_cast<double>(frames); }
  [[nodiscard]] bool silent() const noexcept { return value == 0.0 && step == 0.0; }
};

} // namespace oscillate::native_dsp::dsp
