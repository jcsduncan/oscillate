#pragma once

#include "synth/nova/filter_impl.hpp"

#include <cmath>
#include <cstddef>

// Nova's delay-line family: Comb +, Comb − and Flanger, damped feedback
// combs tuned to the cutoff. Coefficients and responses are in
// filter_comb.cpp.
namespace oscillate::native_dsp::synth::nova::filter_detail {

// Third-order Lagrange read `delay` frames back (delay >= 2) from a line
// whose next write goes to `write`.
[[nodiscard]] inline double read_delay(const float* line, std::size_t mask, std::size_t write,
                                       double delay) noexcept {
  const double whole = std::floor(delay);
  const double f = delay - whole;
  const auto n = static_cast<std::size_t>(whole);
  const double newer = line[(write - n + 1U) & mask];
  const double at = line[(write - n) & mask];
  const double older = line[(write - n - 1U) & mask];
  const double oldest = line[(write - n - 2U) & mask];
  const double fm1 = f - 1.0;
  const double fm2 = f - 2.0;
  const double fp1 = f + 1.0;
  return -f * fm1 * fm2 / 6.0 * newer + fp1 * fm1 * fm2 / 2.0 * at - fp1 * f * fm2 / 2.0 * older +
         fp1 * f * fm1 / 6.0 * oldest;
}

} // namespace oscillate::native_dsp::synth::nova::filter_detail

namespace oscillate::native_dsp::synth::nova {

// Comb +/− and flanger: w = x + sign k sat(d), out = (x + sign d) * norm,
// where d is the damped, delayed w. Memory 0 is the damping state.
inline double VoiceFilter::Impl::comb(const Coefficients& c, Memory& m, double x, float* line,
                                      std::size_t mask, std::size_t write) noexcept {
  using namespace filter_detail;
  const double delayed = read_delay(line, mask, write, c.delay);
  const double d = one_pole(delayed, c.damping, m[0]);
  const double fed = comb_headroom * soft_clip(d / comb_headroom);
  line[write & mask] = static_cast<float>(x + c.sign * c.k * fed);
  return c.output_gain * (x + c.sign * d);
}

} // namespace oscillate::native_dsp::synth::nova
