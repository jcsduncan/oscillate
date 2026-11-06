#pragma once

#include "dsp/sse2_pair.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/filter.hpp"
#include "oscillate/native_dsp/synth/nova/filter.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"
#include "vowels.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>

// The parts Nova's filter families share (filter.cpp and filter_*.hpp):
// limits, the trapezoidal building blocks, and the analog axis every
// small-signal response is evaluated on. Private to the filter.
namespace oscillate::native_dsp::synth::nova::filter_detail {

using Complex = std::complex<double>;
inline constexpr double pi = std::numbers::pi;
inline constexpr double two_pi = 2.0 * pi;

inline constexpr double min_cutoff_hz = 8.0;
inline constexpr double max_cutoff_hz = 22'000.0;
// The highest cutoff over the running rate.
inline constexpr double max_normalised = native_dsp::dsp::nyquist_guard;
// The lowest normalised corner warp() sets.
inline constexpr double min_normalised = 1e-7;
inline constexpr double max_drive_db = 24.0;
inline constexpr double input_limit = 64.0; // +36 dBFS: only insane input is clamped.
inline constexpr double fade_seconds = 0.005;
inline constexpr double lowest_delay_hz = 20.0; // The longest comb period.
// About -180 dBFS of thermal noise in the analog loops, so a resonant
// ladder can start oscillating with no input, as hardware does.
inline constexpr double noise_level = 1e-9;
// Amplitude-dependent damping of the resonant SVFs: linear for small
// signals, and it bounds the band state near (1 / svf_limit)^(1/3) for a
// full-scale tone sitting on the resonance.
inline constexpr double svf_limit = 0.01;
inline constexpr double comb_headroom = 2.0;
inline constexpr double phaser_headroom = 4.0;
inline constexpr double diode_branch_headroom = 3.0;
inline constexpr std::size_t formant_bands = vowel_formants;
inline constexpr std::size_t vowel_bands = 4;
inline constexpr std::size_t phaser_stages = 8;
// The cutoff at which the formant model sings its vowels unshifted.
inline constexpr double formant_reference_hz = 800.0;
inline constexpr double butterworth_k1 = 1.0 / 0.5411961; // Fourth-order pair, first stage.
inline constexpr double butterworth_q2 = 1.3065630;       // Second stage's flat Q.
// A bell's amplitude A = 10^(dB / bell_db_per_decade): the square root of
// its gain, as each of its two sections takes half.
inline constexpr double bell_db_per_decade = 40.0;
// The rate when none (or nonsense) is given.
inline constexpr double default_rate = 48'000.0;
// The internal rates a filter supports: an 8 kHz host at base rate up to a
// 384 kHz host oversampled four times. Others (and non-finite ones) are
// brought into this range, so comb clamps keep their order and delay lines
// stay bounded.
inline constexpr double min_internal_rate = 8'000.0;
inline constexpr double max_internal_rate = 4.0 * 384'000.0;
[[nodiscard]] inline double supported_rate(double rate) noexcept {
  if (!(rate > 0.0) || !std::isfinite(rate))
    return default_rate;
  return std::clamp(rate, min_internal_rate, max_internal_rate);
}
// Responses are read up to this share of the rate (tan() stays finite).
inline constexpr double response_top = 0.4999;

using Params = VoiceFilter::Params;

// The left and right channels through the same code at once (see Pair).
using native_dsp::dsp::Pair;

using native_dsp::dsp::db_to_gain;

// The models, by the family that implements them.
enum class Family : std::uint8_t { ladder, svf, character, comb };

[[nodiscard]] constexpr Family family_of(FilterModel model) noexcept {
  switch (model) {
  case FilterModel::ladder24:
  case FilterModel::ladder12:
  case FilterModel::diode:
  case FilterModel::mg_low:
  case FilterModel::ota:
    return Family::ladder;
  case FilterModel::lp12:
  case FilterModel::lp24:
  case FilterModel::bp12:
  case FilterModel::bp24:
  case FilterModel::hp12:
  case FilterModel::hp24:
  case FilterModel::notch:
  case FilterModel::peak:
    return Family::svf;
  case FilterModel::comb_plus:
  case FilterModel::comb_minus:
  case FilterModel::flanger:
    return Family::comb;
  case FilterModel::formant:
  case FilterModel::vowel:
  case FilterModel::phaser:
  case FilterModel::morph_lbh:
  case FilterModel::dual_peak:
  case FilterModel::ring:
    return Family::character;
  }
  return Family::character;
}

// tan(pi f / fs), the trapezoidal integrator gain for a corner at f.
[[nodiscard]] inline double warp(double hz, double rate) noexcept {
  return std::tan(pi * std::clamp(hz / rate, min_normalised, max_normalised));
}

[[nodiscard]] inline double clamped(double x, double low, double high, double fallback) noexcept {
  return std::isfinite(x) ? std::clamp(x, low, high) : fallback;
}

// xorshift32 white noise in [-1, 1).
[[nodiscard]] inline double white(std::uint32_t& state) noexcept {
  state ^= state << 13U;
  state ^= state >> 17U;
  state ^= state << 5U;
  return static_cast<double>(state) * (2.0 / 4294967296.0) - 1.0;
}

// Trapezoidal one-pole low-pass with gain G = g / (1 + g).
[[nodiscard]] inline double one_pole(double x, double G, double& state) noexcept {
  const double v = (x - state) * G;
  const double y = v + state;
  state = y + v;
  return y;
}

struct SvfOut {
  double low;
  double band;
  double high;
};

// Simper's trapezoidal SVF; k may change every sample.
[[nodiscard]] inline SvfOut svf(double x, double g, double k, double& ic1, double& ic2) noexcept {
  const auto c = SvfCoefficients::make(g, k);
  const double v3 = x - ic2;
  const double v1 = c.a1 * ic1 + c.a2 * v3;
  const double v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
  ic1 = 2.0 * v1 - ic1;
  ic2 = 2.0 * v2 - ic2;
  return {v2, v1, x - k * v1 - v2};
}

// Resonant SVFs damp harder as their band state grows: bounded, stable
// nonlinear feedback that leaves small signals linear.
[[nodiscard]] inline double limited(double k, double ic1) noexcept {
  return k + svf_limit * ic1 * ic1;
}

// A frequency on each section's analog axis: every model is trapezoidal,
// so its small-signal response is its prototype's at s = j tan(w / 2) / g.
struct ResponsePoint {
  double w = 0.0; // Radians per frame.
  double t = 0.0; // tan(w / 2).

  struct Sections {
    Complex low, band, high;
  };
  [[nodiscard]] Complex s_of(double g) const noexcept { return Complex(0.0, t / g); }
  [[nodiscard]] Sections svf_at(double g, double k) const noexcept {
    const Complex s = s_of(g);
    const Complex den = s * s + k * s + 1.0;
    return Sections{1.0 / den, s / den, s * s / den};
  }
};

} // namespace oscillate::native_dsp::synth::nova::filter_detail
