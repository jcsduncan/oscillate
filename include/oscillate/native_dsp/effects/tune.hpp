#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"

#include <cstddef>

namespace oscillate::native_dsp::effects::tune {

// Tune (ADR-0042): a tuner that passes audio untouched.
// - Instrument and Bass modes: McLeod's normalised square difference
//   function (by FFT) finds the period without octave errors, then the
//   phase advance of the strongest of the first four harmonics between two
//   windows refines it to a fraction of a cent.
// - Kick mode: onsets on the low band; then the low-passed kick's
//   frequency is measured cycle by cycle at its zero crossings, tracing the
//   glide; the settled pitch is the median of its last cycles.
[[nodiscard]] const EffectEngine& engine();

inline constexpr std::size_t trajectory_points = 80; // 0-400 ms, 5 ms apart.
inline constexpr std::size_t history = 200;          // About 4 s.

// Telemetry: the pitch now (Hz, 0 when none), as a fractional MIDI note and
// cents from the nearest note (against A4 = reference), its clarity (0..1)
// and level (dBFS); in Kick mode the last hit's settled pitch (Hz, note,
// cents), the hit count and the trajectory (Hz at 5 ms steps from the
// onset, 0 past its end); then the history of fractional notes (0 when
// none), oldest first.
namespace telemetry {
inline constexpr std::size_t hz = 0;
inline constexpr std::size_t note = 1;
inline constexpr std::size_t cents = 2;
inline constexpr std::size_t clarity = 3;
inline constexpr std::size_t level_db = 4;
inline constexpr std::size_t settled_hz = 5;
inline constexpr std::size_t settled_note = 6;
inline constexpr std::size_t settled_cents = 7;
inline constexpr std::size_t hits = 8;
inline constexpr std::size_t trajectory = 9;
inline constexpr std::size_t notes = trajectory + trajectory_points;
inline constexpr std::size_t size = notes + history;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::tune
