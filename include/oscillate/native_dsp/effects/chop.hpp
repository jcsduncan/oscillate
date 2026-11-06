#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"

#include <cstddef>
#include <cstdint>

namespace oscillate::native_dsp::effects::chop {

// Chop (ADR-0075): a tempo-locked gate and stutter, for chopping bass.
// A pattern of 16 or 32 steps on the song's grid, each with a level, a
// gate length, ratchets (hits inside the step), a repeat (the step's first
// slice captured as it plays and replayed 2, 4 or 8 times, dropping in
// pitch and level) and a chance. Every edge fades over Smooth, so nothing
// clicks. Steps come from the song position, chance from (seed, pattern,
// step): playback, a seek and an export render the same. No latency.
[[nodiscard]] const EffectEngine& engine();

inline constexpr std::size_t max_steps = 32;

// Repeats replay whole slices up to the longest step (a swung 1/8) at this
// tempo; below it a repeat step plays through untouched. The kernel's two
// capture buffers are sized for it at creation.
inline constexpr double min_repeat_bpm = 20.0;

// The chance roll for a step: uniform in [0, 1), a pure function of the
// seed, the pattern's count from the song start and the step (its index in
// the pattern before Rotate). A step plays when its roll is below its
// chance.
[[nodiscard]] double chance_roll(std::uint32_t seed, std::int64_t pattern,
                                 std::size_t step) noexcept;

// The panel's one-pattern view: points per pattern.
inline constexpr std::size_t view_points = 256;

// Telemetry: the playhead in the pattern (0..1, or -1 while the tempo is
// unknown); the pattern step playing (0-based, after Rotate) and its slot
// in the pattern (where it sounds); the pattern's length in beats; the
// pattern's count from the song start; whether each step plays in this
// pattern (1 or 0, after chance); then the last whole pattern of the input
// and of the output as heard (peak per point) and the gate's gain (mean
// per point, 0..1).
namespace telemetry {
inline constexpr std::size_t phase = 0;
inline constexpr std::size_t step = 1;
inline constexpr std::size_t slot = 2;
inline constexpr std::size_t pattern_beats = 3;
inline constexpr std::size_t pattern = 4;
inline constexpr std::size_t played = 5;
inline constexpr std::size_t input = played + max_steps;
inline constexpr std::size_t output = input + view_points;
inline constexpr std::size_t gain = output + view_points;
inline constexpr std::size_t size = gain + view_points;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::chop
