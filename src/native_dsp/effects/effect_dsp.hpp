#pragma once

#include "dsp/ramp.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

// Small building blocks shared by the native effects (ADR-0040).
namespace oscillate::native_dsp::effects::dsp {

// A double-precision delay line read at fractional delays through a
// four-point (third-order) Lagrange interpolator.
class Delay {
public:
  // Control only.
  void prepare(std::size_t max_frames) {
    std::size_t size = 4;
    while (size < max_frames + 4) {
      size *= 2;
    }
    buffer_.assign(size, 0.0);
    mask_ = size - 1;
    write_ = 0;
  }
  void reset() noexcept {
    std::ranges::fill(buffer_, 0.0);
    write_ = 0;
  }
  void write(double x) noexcept {
    buffer_[write_ & mask_] = x;
    ++write_;
  }
  // A read position: the integer delay and its Lagrange weights. A read
  // that stays put can keep one and skip the weights' arithmetic.
  struct Tap {
    std::size_t whole = 1;
    double cm1 = 0.0, c0 = 1.0, c1 = 0.0, c2 = 0.0;
  };
  [[nodiscard]] Tap tap(double frames) const noexcept {
    const double limit = static_cast<double>(mask_ - 3);
    frames = frames < 1.0 ? 1.0 : (frames > limit ? limit : frames);
    const auto whole = static_cast<std::size_t>(frames);
    const double t = frames - static_cast<double>(whole);
    return {whole, -t * (t - 1.0) * (t - 2.0) / 6.0, (t + 1.0) * (t - 1.0) * (t - 2.0) / 2.0,
            -(t + 1.0) * t * (t - 2.0) / 2.0, (t + 1.0) * t * (t - 1.0) / 6.0};
  }
  [[nodiscard]] double read(const Tap& tap) const noexcept {
    // Points at offsets -1, 0, 1, 2 around the integer delay, newest first.
    const std::size_t newest = write_ - 1;
    const double ym1 = buffer_[(newest - tap.whole + 1) & mask_];
    const double y0 = buffer_[(newest - tap.whole) & mask_];
    const double y1 = buffer_[(newest - tap.whole - 1) & mask_];
    const double y2 = buffer_[(newest - tap.whole - 2) & mask_];
    return tap.cm1 * ym1 + tap.c0 * y0 + tap.c1 * y1 + tap.c2 * y2;
  }
  // The input `frames` samples before the most recent write; at least one.
  [[nodiscard]] double read(double frames) const noexcept { return read(tap(frames)); }

  // The input exactly `frames` samples before the most recent write.
  [[nodiscard]] double sample(std::size_t frames) const noexcept {
    return buffer_[(write_ - 1 - frames) & mask_];
  }

private:
  std::vector<double> buffer_;
  std::size_t mask_ = 0;
  std::size_t write_ = 0;
};

// A first-order allpass (Thiran) fractional read: unity magnitude at every
// frequency, so a delay inside a feedback loop loses nothing however often
// the signal passes; for slowly varying delays. The fraction sits in
// [0.5, 1.5) to keep the coefficient small.
struct AllpassReader {
  double previous = 0.0;
  [[nodiscard]] double read(const Delay& line, double frames) noexcept {
    frames = frames < 1.0 ? 1.0 : frames;
    const auto whole = static_cast<std::size_t>(frames - 0.5);
    const double fraction = frames - static_cast<double>(whole);
    const double eta = (1.0 - fraction) / (1.0 + fraction);
    const double y = eta * line.sample(whole) + line.sample(whole + 1) - eta * previous;
    previous = y;
    return y;
  }
};

// One linear ramp per control tick.
using native_dsp::dsp::Ramp;

// A bilinear first-order shelf (n1 s + n0) / (d1 s + d0), s normalised to
// the corner. Emphasis and de-emphasis are exact inverses.
struct Shelf {
  double b0 = 1.0;
  double b1 = 0.0;
  double a1 = 0.0;
  double x1 = 0.0;
  double y1 = 0.0;

  void set(double n1, double n0, double d1, double d0, double k) noexcept {
    const double a0 = d1 / k + d0;
    b0 = (n1 / k + n0) / a0;
    b1 = (n0 - n1 / k) / a0;
    a1 = (d0 - d1 / k) / a0;
  }
  [[nodiscard]] double process(double x) noexcept {
    const double y = b0 * x + b1 * x1 - a1 * y1;
    x1 = x;
    y1 = y;
    return y;
  }
  void reset() noexcept {
    x1 = 0.0;
    y1 = 0.0;
  }
};

// The phase's fraction, as phase - std::floor(phase) to the bit, without
// the library call baseline x86-64 makes for floor.
[[nodiscard]] inline double wrap(double phase) noexcept {
  return phase - synth::floor_inline(phase);
}

// Bipolar LFO shapes of a phase in cycles, both starting at 0 and rising.
[[nodiscard]] inline double sine(double phase) noexcept {
  return std::sin(2.0 * std::numbers::pi * phase);
}
[[nodiscard]] inline double triangle(double phase) noexcept {
  return 4.0 * std::fabs(wrap(phase - 0.25) - 0.5) - 1.0;
}

using native_dsp::dsp::db_to_gain;

// The classic processors (ADR-0047) take whole-hertz rates; 0 for any other,
// which they refuse.
[[nodiscard]] inline std::uint32_t classic_rate(double sample_rate) noexcept {
  return sample_rate > 0.0 && sample_rate <= 1e6 && sample_rate == std::floor(sample_rate)
             ? static_cast<std::uint32_t>(sample_rate)
             : 0;
}

} // namespace oscillate::native_dsp::effects::dsp
