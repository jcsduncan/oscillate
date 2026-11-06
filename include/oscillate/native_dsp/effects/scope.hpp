#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"

#include <cstddef>

namespace oscillate::native_dsp::effects::scope {

// Scope (ADR-0042): an oscilloscope locked to the bar and beat (or a free
// time window) with the sidechain overlaid, and an alignment meter: the
// cross-correlation of the track and the sidechain (whole band or below
// 200 Hz), taken every ~85 ms and averaged over about 0.7 s, gives the
// delay between them to a fraction of a sample, their polarity and how
// alike they are. Audio passes untouched.
[[nodiscard]] const EffectEngine& engine();

inline constexpr std::size_t columns = 480;
// The alignment search reaches +-20 ms.
inline constexpr double max_lag_seconds = 0.02;

// Telemetry: the window as (min, max) per column for the track and the
// sidechain, oldest sweep behind the write column; the column being
// written; then the alignment: the track's delay behind the sidechain (ms,
// positive when the track is late), polarity (+1 or -1), the correlation
// at that lag (0..1), and the window length (ms).
namespace telemetry {
inline constexpr std::size_t track = 0;
inline constexpr std::size_t sidechain = track + 2 * columns;
inline constexpr std::size_t write_column = sidechain + 2 * columns;
inline constexpr std::size_t delay_ms = write_column + 1;
inline constexpr std::size_t polarity = delay_ms + 1;
inline constexpr std::size_t correlation = polarity + 1;
inline constexpr std::size_t window_ms = correlation + 1;
inline constexpr std::size_t size = window_ms + 1;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::scope
