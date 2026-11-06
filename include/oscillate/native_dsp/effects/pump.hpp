#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace oscillate::native_dsp::effects::pump {

// Pump (ADR-0058): a volume shaper for sidechain pumping, drawn rather than
// dialled. One cycle's volume is a curve in the effect's document, drawn
// over the kick it answers to. Tempo restarts the cycle on the song's grid;
// Kick Audio restarts it on each kick detected in the sidechain, with
// lookahead putting the duck on time. Lows Only pumps below a
// Linkwitz-Riley crossover and leaves the top alone. Anti-click turns every
// drawn edge into an S-curve at least that long and reads the shape early
// by the smoothing's own delay, so a hard gate lands on time without
// clicking.
[[nodiscard]] const EffectEngine& engine();

// The document's curves: the volume over a cycle, x 0..1 in time, y 0
// (silent) to 1 (untouched). Slot A is "shape", B to D "shape.b" to
// "shape.d"; Three Band can give a slot's mids and highs their own curve,
// "<slot>.mid" and "<slot>.high" ("shape.mid", "shape.b.high"). A missing
// band curve follows its slot's; a missing slot follows A; an empty
// document plays Classic Pump.
inline constexpr std::string_view shape_curve = "shape";
inline constexpr std::size_t shape_slots = 4;
inline constexpr std::size_t shape_bands = 3;
[[nodiscard]] std::string curve_id(std::size_t slot, std::size_t band);

// The panel's kick and curve views: points per cycle.
inline constexpr std::size_t view_points = 256;

// Telemetry: the cycle position (0..1, or -1 while nothing plays it); the
// gain applied now (dB, <= 0); the cycle's length (seconds; 0 when the
// tempo is unknown); 1 when a sidechain key is connected; then the last
// whole cycle of the key (or, with no key, the input) as a peak per point,
// aligned to the output; the applied curve per point: the playing slot's
// drawn shape after depth and anti-click, as it actually sounds; the
// cycle's length in beats of the song's meter, for the panel's grid; the
// last whole cycle of the input and of the output as heard (peak per
// point), the before and after; the key of the last note on the keying
// tracks (any key, for Learn; -1 before one); the pattern step and the
// slot playing (0-based; the slot is -1 while morphing); and the mid and
// high bands' gain now (dB, Three Band).
namespace telemetry {
inline constexpr std::size_t phase = 0;
inline constexpr std::size_t gain_db = 1;
inline constexpr std::size_t cycle_seconds = 2;
inline constexpr std::size_t keyed = 3;
inline constexpr std::size_t kick = 4;
inline constexpr std::size_t applied = kick + view_points;
inline constexpr std::size_t cycle_beats = applied + view_points;
inline constexpr std::size_t input = cycle_beats + 1;
inline constexpr std::size_t output = input + view_points;
inline constexpr std::size_t last_note = output + view_points;
inline constexpr std::size_t step = last_note + 1;
inline constexpr std::size_t slot = step + 1;
inline constexpr std::size_t gain_mid_db = slot + 1;
inline constexpr std::size_t gain_high_db = gain_mid_db + 1;
inline constexpr std::size_t size = gain_high_db + 1;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::pump
