#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace oscillate::native_dsp::synth {

// Two channels that share coefficients, processed together. Plain element-wise
// arithmetic, which the compiler maps onto SSE2 pairs on x86-64.
struct StereoSample {
  double left = 0.0;
  double right = 0.0;
  friend bool operator==(StereoSample, StereoSample) = default;
};
[[nodiscard]] constexpr StereoSample operator+(StereoSample a, StereoSample b) noexcept {
  return {a.left + b.left, a.right + b.right};
}
[[nodiscard]] constexpr StereoSample operator-(StereoSample a, StereoSample b) noexcept {
  return {a.left - b.left, a.right - b.right};
}
[[nodiscard]] constexpr StereoSample operator*(StereoSample a, StereoSample b) noexcept {
  return {a.left * b.left, a.right * b.right};
}
[[nodiscard]] constexpr StereoSample operator/(StereoSample a, StereoSample b) noexcept {
  return {a.left / b.left, a.right / b.right};
}
[[nodiscard]] constexpr StereoSample operator+(double a, StereoSample b) noexcept {
  return {a + b.left, a + b.right};
}
[[nodiscard]] constexpr StereoSample operator+(StereoSample a, double b) noexcept {
  return {a.left + b, a.right + b};
}
[[nodiscard]] constexpr StereoSample operator-(StereoSample a, double b) noexcept {
  return {a.left - b, a.right - b};
}
[[nodiscard]] constexpr StereoSample operator*(double a, StereoSample b) noexcept {
  return {a * b.left, a * b.right};
}
[[nodiscard]] constexpr StereoSample operator*(StereoSample a, double b) noexcept {
  return {a.left * b, a.right * b};
}

[[nodiscard]] constexpr double clamp_symmetric(double x, double limit) noexcept {
  return x < -limit ? -limit : (x > limit ? limit : x);
}
[[nodiscard]] constexpr StereoSample clamp_symmetric(StereoSample x, double limit) noexcept {
  return {clamp_symmetric(x.left, limit), clamp_symmetric(x.right, limit)};
}

// Rational tanh-like saturator: unity slope at zero, exactly +/-1 with zero
// slope at |x| >= 3, so it is bounded and free of corners. Branch-free: the
// clamped rational is exactly 1 at 3.
template <typename T> [[nodiscard]] inline T soft_clip(T x) noexcept {
  x = clamp_symmetric(x, 3.0);
  const T square = x * x;
  return x * (27.0 + square) / (27.0 + 9.0 * square);
}

// Division-free cubic saturator: x - 4x^3/27 up to |x| = 1.5, where it meets
// +/-1 with zero slope. Cheaper and a little brighter than soft_clip; meant
// for inner stages that run several times per sample.
template <typename T> [[nodiscard]] inline T soft_clip_cubic(T x) noexcept {
  x = clamp_symmetric(x, 1.5);
  return x - (4.0 / 27.0) * x * x * x;
}

// sin(2 * pi * phase) for any finite phase. Odd Taylor series to degree 11
// after folding into [-pi/2, pi/2]; absolute error < 1e-7 (about -140 dB).
// floor() for finite |x| < 2^53, from SSE2 truncation. On baseline x86-64
// std::floor is a library call; this inlines to a convert and a compare and
// returns exactly the same value.
[[nodiscard]] inline double fast_floor(double x) noexcept {
  const auto truncated = static_cast<double>(static_cast<std::int64_t>(x));
  return truncated - (x < truncated ? 1.0 : 0.0);
}

// 2^52: every double of at least this magnitude is a whole number.
inline constexpr double first_whole_only_double = 4'503'599'627'370'496.0;

// std::floor, inlined for any input: the same value, the sign of a zero,
// infinities and NaN included. For hot loops whose inputs are not known to
// stay inside fast_floor's range.
[[nodiscard]] inline double floor_inline(double x) noexcept {
  if (!(std::fabs(x) < first_whole_only_double))
    return x;
  const double whole = fast_floor(x);
  return whole == 0.0 ? std::copysign(0.0, x) : whole;
}

// Branch-free fold (sin(pi - a) = sin a, odd symmetry) so sweeping phases
// never mispredict; bit-identical to folding with branches.
[[nodiscard]] inline double sin_cycle(double phase) noexcept {
  const double x = phase - fast_floor(phase + 0.5); // [-0.5, 0.5)
  const double magnitude = std::fabs(x);
  const double folded = std::copysign(std::min(magnitude, 0.5 - magnitude), x);
  const double t = 2.0 * std::numbers::pi * folded;
  const double t2 = t * t;
  return t * (1.0 +
              t2 * (-1.0 / 6.0 +
                    t2 * (1.0 / 120.0 + t2 * (-1.0 / 5040.0 +
                                              t2 * (1.0 / 362880.0 + t2 * (-1.0 / 39916800.0))))));
}

class DcBlocker {
public:
  void prepare(double sample_rate, double corner_hz = 5.0) noexcept {
    pole_ = std::exp(-2.0 * std::numbers::pi * corner_hz / sample_rate);
    reset();
  }
  void reset() noexcept {
    input_ = 0.0;
    output_ = 0.0;
  }
  [[nodiscard]] double process(double x) noexcept {
    output_ = x - input_ + pole_ * output_;
    input_ = x;
    return output_;
  }

private:
  double pole_ = 0.0;
  double input_ = 0.0;
  double output_ = 0.0;
};

enum class NoiseColour : std::uint8_t { white, pink };

// Deterministic per-seed noise, so renders and golden tests are repeatable.
// Pink uses Paul Kellet's refined filter, scaled to roughly white's level.
class NoiseSource {
public:
  void reset(std::uint32_t seed) noexcept {
    state_ = seed == 0 ? 0x9E3779B9U : seed;
    b0_ = b1_ = b2_ = b3_ = b4_ = b5_ = b6_ = 0.0;
  }
  [[nodiscard]] double white() noexcept {
    state_ ^= state_ << 13U;
    state_ ^= state_ >> 17U;
    state_ ^= state_ << 5U;
    return static_cast<double>(state_) * (2.0 / 4294967296.0) - 1.0;
  }
  [[nodiscard]] double next(NoiseColour colour) noexcept {
    const double w = white();
    if (colour == NoiseColour::white) {
      return w;
    }
    b0_ = 0.99886 * b0_ + w * 0.0555179;
    b1_ = 0.99332 * b1_ + w * 0.0750759;
    b2_ = 0.96900 * b2_ + w * 0.1538520;
    b3_ = 0.86650 * b3_ + w * 0.3104856;
    b4_ = 0.55000 * b4_ + w * 0.5329522;
    b5_ = -0.7616 * b5_ - w * 0.0168980;
    const double pink = b0_ + b1_ + b2_ + b3_ + b4_ + b5_ + b6_ + w * 0.5362;
    b6_ = w * 0.115926;
    return pink * 0.25;
  }

private:
  std::uint32_t state_ = 0x9E3779B9U;
  double b0_ = 0.0, b1_ = 0.0, b2_ = 0.0, b3_ = 0.0, b4_ = 0.0, b5_ = 0.0, b6_ = 0.0;
};

} // namespace oscillate::native_dsp::synth
