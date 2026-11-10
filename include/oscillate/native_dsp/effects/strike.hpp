#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"

#include <cstddef>
#include <cstdint>

namespace oscillate::native_dsp::effects::strike {

// Strike (ADR-0118): a transient shaper, single-band or split into up to
// four bands by phase-compensated Linkwitz-Riley crossovers (Phase:
// Minimum, no latency) or a linear-phase FIR bank (Linear: the bands sum to
// the input delayed by the bank's latency). Each band's
// detector reads the band's analytic power (a Hilbert allpass pair, so a
// steady tone reads a steady level), smoothed over level_seconds. Its
// attack weight is how far that power has risen over a follower that
// rises with Attack Time; its sustain weight how far it has fallen under a
// follower that falls with Sustain Time. Both are ratios, so they do not
// depend on the level. Each weight is 0 up to dead_db and 1 from full_db,
// and the band's gain (dB) is Attack times the attack weight plus Sustain
// times the sustain weight, smoothed over gain_seconds. Stereo link moves
// each channel's power towards the louder channel's. Mix blends with the
// dry bands; Output follows; Clip passes the sum through Edge's soft curve
// under the ceiling, four times oversampled. Lookahead delays the audio
// against the detectors. Phase, Lookahead and Clip fix the latency.
[[nodiscard]] const EffectEngine& engine();

inline constexpr std::size_t max_bands = 4;
inline constexpr std::size_t max_crossovers = max_bands - 1;

// The detector's constants.
inline constexpr double level_seconds = 0.0005;
// The attack follower falls back to the level over this long.
inline constexpr double rise_release_seconds = 0.02;
inline constexpr double gain_seconds = 0.0005;
inline constexpr double dead_db = 1.0;
inline constexpr double full_db = 7.0;

// The latency in frames for strike.lookahead's choice, a linear phase and
// strike.clip at `sample_rate` (the engine's latency_frames).
[[nodiscard]] std::uint32_t latency_frames(std::size_t lookahead, bool linear, bool clip,
                                           double sample_rate);

// The scrolling view: this many slices over history_seconds, each the
// input's and the output's peak (linear) and per band the gain furthest
// from 0 dB applied in it (dB, signed).
inline constexpr std::size_t history_slices = 256;
inline constexpr double history_seconds = 2.0;

// Telemetry: input and output peak (dBFS, held 1 s) per channel; how far
// Clip took the loudest peak down (dB, positive, held 1 s); the latency in
// milliseconds; the band count; the crossovers as they play (Hz, 0 when
// unused); per band its gain now (dB, signed, the channel furthest from
// 0 dB); then history_slices input peaks, output peaks, and per band
// history_slices gains, oldest first.
namespace telemetry {
inline constexpr std::size_t input_peak = 0;  // left, right
inline constexpr std::size_t output_peak = 2; // left, right
inline constexpr std::size_t clipped = 4;
inline constexpr std::size_t latency_ms = 5;
inline constexpr std::size_t band_count = 6;
inline constexpr std::size_t crossovers = 7;
inline constexpr std::size_t band_gain = crossovers + max_crossovers;
inline constexpr std::size_t history_input = band_gain + max_bands;
inline constexpr std::size_t history_output = history_input + history_slices;
inline constexpr std::size_t history_gain = history_output + history_slices;
inline constexpr std::size_t size = history_gain + max_bands * history_slices;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::strike
