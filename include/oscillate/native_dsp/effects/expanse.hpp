#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"

#include <cstddef>

namespace oscillate::native_dsp::effects::expanse {

// Expanse (ADR-0040): an algorithmic reverb. Six spaces (room, hall, plate,
// chamber, cathedral, cloud) share one design:
//  - pre-delay, then early reflections tapped from a per-space pattern;
//  - input diffusion through Schroeder allpasses;
//  - a 16-line feedback delay network with a Hadamard mixing matrix, one
//    allpass inside each line for echo density, and slow quasi-random
//    modulation of every line so no mode rings;
//  - per-line absorption from second-order low and high shelves designed
//    (after Jot) so low, mid and high bands each decay at their own RT60,
//    the line's allpass delay included;
//  - an optional pitch-shifted shimmer path inside the loop, and freeze.
// A change of space hands the old network its tail to ring out while the
// new one takes the input, so a switch never cuts the reverb.
[[nodiscard]] const EffectEngine& engine();

inline constexpr std::size_t lines = 16;

// Telemetry: input, early and late RMS (linear, smoothed over 300 ms).
namespace telemetry {
inline constexpr std::size_t input_rms = 0;
inline constexpr std::size_t early_rms = 1;
inline constexpr std::size_t late_rms = 2;
inline constexpr std::size_t size = 3;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::expanse
