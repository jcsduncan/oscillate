#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/reduction_bands.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace oscillate::native_dsp::effects::ceiling {

// Ceiling (ADR-0117): the mastering limiter. Gain drives the mix into a
// lookahead gain stage that reads true (4x interpolated) peaks; its fast
// envelope releases to a slow loudness envelope, so short peaks recover
// fast and dense passages hold steady. Transients lets detected onsets and
// Clip lets every peak past the gain stage by up to their room, into an 8x
// oversampled, antialiased soft clipper; a true-peak guard after it keeps
// every BS.1770 4x point of the output at or under the ceiling. Link and
// Transient Link share the gain between the channels. The lookahead fixes
// the latency, so a change of it takes the route's rebuild path.
[[nodiscard]] const EffectEngine& engine();

// The latency in frames for a lookahead (ms) at `sample_rate`: the engine's
// latency_frames for those values.
[[nodiscard]] std::uint32_t latency_frames(double lookahead_ms, double sample_rate);

// The loudest onset room Transients gives at 100%, in dB over the ceiling.
inline constexpr double onset_room_db = 6.0;

// A loudness reading with nothing to read (silence, or too short).
inline constexpr float no_loudness = -70.0F;

// Telemetry: the input's peak before Gain (dBFS) and the output's true peak
// (dBTP) per channel, both held 1 s; the gain stage's reduction (dB,
// positive, held 1 s), then where it comes from: each reduction band's
// share of the detector signal (the driven mix), low to high
// (reduction_bands.hpp). Then the most the clipper and the guard took off
// (dB, positive, held 1 s); the output's momentary, short-term and
// integrated loudness (LUFS; no_loudness until there is a reading; the
// integrated one since the last reset); the latency in milliseconds.
namespace telemetry {
inline constexpr std::size_t input_peak = 0;  // left, right
inline constexpr std::size_t output_peak = 2; // left, right
inline constexpr std::size_t reduction = 4;
inline constexpr std::size_t reduction_bands = 5;
inline constexpr std::size_t clipped = reduction_bands + effects::reduction_bands::count;
inline constexpr std::size_t guarded = clipped + 1;
inline constexpr std::size_t momentary = guarded + 1;
inline constexpr std::size_t short_term = momentary + 1;
inline constexpr std::size_t integrated = short_term + 1;
inline constexpr std::size_t latency_ms = integrated + 1;
inline constexpr std::size_t size = latency_ms + 1;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::ceiling
