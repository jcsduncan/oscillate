#pragma once

#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

// The one second-order section shared by the native DSP, with the RBJ
// designs it is used with.
namespace oscillate::native_dsp::dsp {

// Normalised coefficients (a0 = 1) of b0 + b1 z^-1 + b2 z^-2 over
// 1 + a1 z^-1 + a2 z^-2.
struct BiquadCoefficients {
  double b0 = 1.0;
  double b1 = 0.0;
  double b2 = 0.0;
  double a1 = 0.0;
  double a2 = 0.0;
};

// Transposed direct form II in double precision.
struct Biquad {
  double b0 = 1.0;
  double b1 = 0.0;
  double b2 = 0.0;
  double a1 = 0.0;
  double a2 = 0.0;
  double z1 = 0.0;
  double z2 = 0.0;

  void set(const BiquadCoefficients& c) noexcept {
    b0 = c.b0;
    b1 = c.b1;
    b2 = c.b2;
    a1 = c.a1;
    a2 = c.a2;
  }
  [[nodiscard]] double process(double x) noexcept {
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
  void reset() noexcept {
    z1 = 0.0;
    z2 = 0.0;
  }
};

// A design's corner: a frequency at a sample rate.
struct Corner {
  double hz = 0.0;
  double rate = 0.0;
};

// The RBJ second-order allpass: unity magnitude, its phase turning through
// 360 degrees around the corner. b2 is exactly 1.
[[nodiscard]] inline BiquadCoefficients rbj_allpass(Corner at, double q) noexcept {
  const double hz = at.hz;
  const double rate = at.rate;
  const double w = 2.0 * std::numbers::pi * hz / rate;
  const double alpha = std::sin(w) / (2.0 * q);
  const double a0 = 1.0 + alpha;
  const double b0 = (1.0 - alpha) / a0;
  const double b1 = -2.0 * std::cos(w) / a0;
  return {b0, b1, 1.0, b1, b0};
}

// The RBJ shelf with slope S = 1: exactly `gain` at DC (low) or Nyquist
// (high), unity at the other end. The corner is held below 0.45 of the rate.
[[nodiscard]] inline BiquadCoefficients rbj_shelf(bool high, double gain, Corner at) noexcept {
  const double hz = at.hz;
  const double rate = at.rate;
  const double a = std::sqrt(gain);
  const double w = 2.0 * std::numbers::pi * std::min(hz, nyquist_guard * rate) / rate;
  const double c = std::cos(w);
  const double alpha = std::sin(w) / 2.0 * std::numbers::sqrt2;
  const double k = 2.0 * std::sqrt(a) * alpha;
  const double sg = high ? -1.0 : 1.0;
  const double n0 = a * ((a + 1.0) - sg * (a - 1.0) * c + k);
  const double n1 = sg * 2.0 * a * ((a - 1.0) - sg * (a + 1.0) * c);
  const double n2 = a * ((a + 1.0) - sg * (a - 1.0) * c - k);
  const double d0 = (a + 1.0) + sg * (a - 1.0) * c + k;
  const double d1 = -sg * 2.0 * ((a - 1.0) + sg * (a + 1.0) * c);
  const double d2 = (a + 1.0) + sg * (a - 1.0) * c - k;
  return {n0 / d0, n1 / d0, n2 / d0, d1 / d0, d2 / d0};
}

// The RBJ peaking EQ: exactly `gain` at the corner, unity far from it, its
// bandwidth set by `q`. The corner is held below 0.45 of the rate.
[[nodiscard]] inline BiquadCoefficients rbj_peak(double gain, Corner at, double q) noexcept {
  const double a = std::sqrt(gain);
  const double w = 2.0 * std::numbers::pi * std::min(at.hz, nyquist_guard * at.rate) / at.rate;
  const double c = std::cos(w);
  const double alpha = std::sin(w) / (2.0 * q);
  const double a0 = 1.0 + alpha / a;
  return {(1.0 + alpha * a) / a0, -2.0 * c / a0, (1.0 - alpha * a) / a0, -2.0 * c / a0,
          (1.0 - alpha / a) / a0};
}

} // namespace oscillate::native_dsp::dsp
