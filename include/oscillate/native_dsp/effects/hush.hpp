#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"

#include <cstddef>
#include <cstdint>

namespace oscillate::native_dsp::effects::hush {

// Hush (ADR-0116): a gate and downward expander. A peak detector on the
// key (the input, or a routed sidechain), through a 24 dB/octave high-pass
// and low-pass, opens the gate at the threshold and closes it once the key
// has stayed below threshold - hysteresis for the hold time. Open, the gain
// is unity; closed, it is the range (a gate) or the expander curve down to
// the range. The gain moves in decibels at a constant rate: attack takes it
// from the range to unity, release back, so no edge ever steps. The range's
// minimum mutes exactly. Stereo link shares one detector between the
// channels; flip turns it into a ducker; listen plays the filtered key.
// Lookahead delays the audio, so the gate opens before the transient.
[[nodiscard]] const EffectEngine& engine();

// The lookahead in frames for hush.lookahead's choice at `sample_rate`
// (the engine's latency_frames).
[[nodiscard]] std::uint32_t latency_frames(std::size_t lookahead, double sample_rate);

// The detector rises at once and falls by 1/e over this long: slow enough
// that a low note's troughs do not close the gate between its peaks.
inline constexpr double detector_fall_seconds = 0.01;

// The scrolling view: this many slices, each the input's peak (linear, at
// the output's clock) and the least gain applied in it (linear, 0 to 1).
inline constexpr std::size_t history_slices = 128;
inline constexpr double history_seconds = 3.0;

// What a closed gate with a muting range reads as reduction, in dB.
inline constexpr double reduction_floor_db = 80.0;

// Telemetry: input and output peak (dBFS, held 1 s) per channel; the key's
// detector level (dBFS); the gain reduction now (dB, positive, the larger
// channel's, reduction_floor_db when muted); the gate's state (1 open,
// 0 closed, the channels' mean); the open and close thresholds (dBFS); the
// latency in milliseconds; then history_slices peaks and history_slices
// gains, oldest first.
namespace telemetry {
inline constexpr std::size_t input_peak = 0;  // left, right
inline constexpr std::size_t output_peak = 2; // left, right
inline constexpr std::size_t key_level = 4;
inline constexpr std::size_t reduction = 5;
inline constexpr std::size_t open = 6;
inline constexpr std::size_t open_threshold = 7;
inline constexpr std::size_t close_threshold = 8;
inline constexpr std::size_t latency_ms = 9;
inline constexpr std::size_t history_peak = 10;
inline constexpr std::size_t history_gain = history_peak + history_slices;
inline constexpr std::size_t size = history_gain + history_slices;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::hush
