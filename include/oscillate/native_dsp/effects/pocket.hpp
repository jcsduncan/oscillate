#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"

#include <cstddef>

namespace oscillate::native_dsp::effects::pocket {

// Pocket (ADR-0041): put on the bass with the kick as sidechain. A bank of
// third-octave bands analyses both; where the kick is present, each band of
// the bass is ducked by a dynamic bell. In Unmask mode the cut is how far
// the bass sits above the kick plus a margin, only where a cut can reveal
// it; in Duck mode each band follows the kick's level there; in Pump mode
// the whole focus ducks together, the classic sidechain pump. Each follows
// the kick's own decay or a release.
// With nothing to resolve the bell cascade is exactly the identity. The
// mono sub removes the side below a frequency without filtering the mid, so
// what sums with the kick is never phase-shifted. Lookahead (a fixed
// latency) completes phase 1.
[[nodiscard]] const EffectEngine& engine();

inline constexpr std::size_t bands = 24;
// Band centres: log-spaced, 30 Hz to 8 kHz.
[[nodiscard]] double band_hz(std::size_t band) noexcept;
// How late a band's analysis follows the sidechain, in seconds. Lookahead
// at least this long brings that band's cut exactly into time.
[[nodiscard]] double band_delay_seconds(std::size_t band) noexcept;

// Waveform history for the panel: wave_points columns of wave_frames each
// (about 0.7 s at 48 kHz), oldest first.
inline constexpr std::size_t wave_points = 256;
inline constexpr std::size_t wave_frames = 128;

// Telemetry for the masking map and meters: per band the kick and bass
// levels (dBFS) and the target cut (dB, <= 0); the overlap energy of kick
// and bass in the focus before and after Pocket (dB, smoothed 300 ms); then
// three waveform histories as (min, max) pairs per column, all aligned
// with the delayed audio: the kick, the bass coming in, the bass going out.
namespace telemetry {
inline constexpr std::size_t kick_db = 0;
inline constexpr std::size_t bass_db = kick_db + bands;
inline constexpr std::size_t reduction_db = bass_db + bands;
inline constexpr std::size_t overlap_before_db = reduction_db + bands;
inline constexpr std::size_t overlap_after_db = overlap_before_db + 1;
inline constexpr std::size_t wave_kick = overlap_after_db + 1;
inline constexpr std::size_t wave_bass_in = wave_kick + 2 * wave_points;
inline constexpr std::size_t wave_bass_out = wave_bass_in + 2 * wave_points;
inline constexpr std::size_t size = wave_bass_out + 2 * wave_points;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::pocket
