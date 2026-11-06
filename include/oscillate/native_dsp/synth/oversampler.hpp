#pragma once

#include "oscillate/native_dsp/synth/shaping.hpp"

#include <array>
#include <cstddef>

namespace oscillate::native_dsp::synth {

// Polyphase IIR half-band filter: two chains of first-order allpasses whose
// coefficients follow the elliptic design used by Laurent de Soras' HIIR.
inline constexpr std::size_t halfband_coefficient_count = 8;
inline constexpr double halfband_transition = 0.04; // Fraction of the base rate.

using HalfbandCoefficients = std::array<double, halfband_coefficient_count>;

// Built once on first use; call from control before the first callback.
[[nodiscard]] const HalfbandCoefficients& halfband_coefficients();

// Designs coefficients for any count and transition, for tests and tuning.
[[nodiscard]] HalfbandCoefficients design_halfband(double transition);

namespace detail {
template <typename T> struct AllpassChain {
  std::array<T, halfband_coefficient_count / 2> x{};
  std::array<T, halfband_coefficient_count / 2> y{};

  // Runs the stages that use coefficients start, start + 2, ...
  [[nodiscard]] T process(T sample, const HalfbandCoefficients& c, std::size_t start) noexcept {
    for (std::size_t i = start, slot = 0; i < c.size(); i += 2, ++slot) {
      const T previous_input = x[slot];
      x[slot] = sample;
      sample = (sample - y[slot]) * c[i] + previous_input;
      y[slot] = sample;
    }
    return sample;
  }
};
} // namespace detail

// T is double or StereoSample.
template <typename T> class BasicUpsampler2x {
public:
  BasicUpsampler2x() : coefficients_(&halfband_coefficients()) {}
  void reset() noexcept {
    even_ = {};
    odd_ = {};
  }
  // Writes two frames at twice the rate, earlier frame first.
  void process(T input, T& first, T& second) noexcept {
    first = even_.process(input, *coefficients_, 0);
    second = odd_.process(input, *coefficients_, 1);
  }

private:
  const HalfbandCoefficients* coefficients_;
  detail::AllpassChain<T> even_;
  detail::AllpassChain<T> odd_;
};

template <typename T> class BasicDownsampler2x {
public:
  BasicDownsampler2x() : coefficients_(&halfband_coefficients()) {}
  void reset() noexcept {
    even_ = {};
    odd_ = {};
  }
  // Takes two frames at twice the rate, earlier frame first.
  [[nodiscard]] T process(T first, T second) noexcept {
    return 0.5 *
           (even_.process(second, *coefficients_, 0) + odd_.process(first, *coefficients_, 1));
  }

private:
  const HalfbandCoefficients* coefficients_;
  detail::AllpassChain<T> even_;
  detail::AllpassChain<T> odd_;
};

using Upsampler2x = BasicUpsampler2x<double>;
using Downsampler2x = BasicDownsampler2x<double>;
using StereoUpsampler2x = BasicUpsampler2x<StereoSample>;
using StereoDownsampler2x = BasicDownsampler2x<StereoSample>;

} // namespace oscillate::native_dsp::synth
