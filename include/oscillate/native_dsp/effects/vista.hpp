#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"

#include <cstddef>

namespace oscillate::native_dsp::effects::vista {

// Vista (ADR-0040): a stereo imager built so the mid is never touched by a
// width control.
//  - Mid and side each run through the same phase-compensated Linkwitz-
//    Riley tree (crossovers at three frequencies), so at 100% the output is
//    the input through one magnitude-flat allpass shared by both channels:
//    the image is exact, at zero latency.
//  - Up to four bands scale the side only, so the mono sum never depends on
//    any width; mono bass removes the side below a frequency, continuously.
//  - Stereoize synthesises side from the mid (a diffuse allpass decorrelator
//    or a Haas delay), above 300 Hz, again side only; the amount lowers a mono
//    source's correlation evenly to 0 at 100%.
//  - Safe width holds the output's correlation at or above a floor by
//    limiting the added side, never narrowing below the input.
//  - Rotation, balance, swap and polarity follow.
[[nodiscard]] const EffectEngine& engine();

inline constexpr std::size_t bands = 4;
inline constexpr std::size_t scope_points = 256;

// Telemetry, smoothed over 300 ms: correlation in and out, side-to-mid
// level in and out (dB), the safe-width gain (dB), per-band output
// correlation and side-to-mid level, then scope_points (mid, side) pairs of
// the output for a goniometer.
namespace telemetry {
inline constexpr std::size_t correlation_in = 0;
inline constexpr std::size_t correlation_out = 1;
inline constexpr std::size_t width_in_db = 2;
inline constexpr std::size_t width_out_db = 3;
inline constexpr std::size_t safe_db = 4;
inline constexpr std::size_t band_correlation = 5;
inline constexpr std::size_t band_width_db = band_correlation + bands;
inline constexpr std::size_t scope = band_width_db + bands;
inline constexpr std::size_t size = scope + 2 * scope_points;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::vista
