#pragma once

#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/native_dsp/synth/voice_pool.hpp"

#include <cstddef>
#include <cstdint>

// The Drift granular engine. (drift.hpp is Ember's component drift source.)
namespace oscillate::native_dsp::synth::drift {

inline constexpr std::uint32_t voice_limit = 12;
inline constexpr double buffer_seconds = 4.0;
// A note started while frozen captures this much before it freezes.
inline constexpr double capture_seconds = 1.0;

// Telemetry layout (ADR-0037): a count, then one record per sounding grain
// (at most max_grains, oldest voices first).
namespace telemetry {
inline constexpr std::size_t count = 0;
inline constexpr std::size_t first_grain = 1;
inline constexpr std::size_t stride = 6;
inline constexpr std::size_t key = 0;       // The note's MIDI key.
inline constexpr std::size_t behind = 1;    // Seconds behind its voice's write head.
inline constexpr std::size_t semitones = 2; // Playback pitch relative to the source.
inline constexpr std::size_t reverse = 3;   // 1 when reading backwards.
inline constexpr std::size_t phase = 4;     // 0..1 through the grain's window.
inline constexpr std::size_t frozen = 5;    // 1 when its voice is frozen.
inline constexpr std::size_t max_grains = (max_telemetry_values - first_grain) / stride;
} // namespace telemetry

// Drift v1: each note writes a generated source (wavetable, partials, air)
// into a 4 s buffer and plays it as a seeded grain cloud with scan, spray,
// pitch intervals, jitter, reverse, Blur, Freeze and tempo sync; a filter
// and amp per note; two LFOs; chorus -> delay -> shimmer reverb. Defaults
// are the "Nebula" preset (ADR-0033).
[[nodiscard]] const SynthEngine& engine();

} // namespace oscillate::native_dsp::synth::drift
