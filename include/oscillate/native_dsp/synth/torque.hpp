#pragma once

#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/native_dsp/synth/voice_pool.hpp"

#include <cstddef>
#include <cstdint>

namespace oscillate::native_dsp::synth::torque {

inline constexpr std::uint32_t max_reese_voices = 7;

// Telemetry layout (ADR-0037): one value each.
namespace telemetry {
inline constexpr std::size_t running = 0; // 1 while the sequencer runs.
inline constexpr std::size_t step = 1;    // Playing step 0..15, or -1.
inline constexpr std::size_t key = 2;     // Sounding pitch (MIDI, fractional while sliding), or -1.
inline constexpr std::size_t sweep = 3;   // Accent sweep 0..1.
inline constexpr std::size_t cutoff = 4;  // Filter cutoff now, Hz.
inline constexpr std::size_t gate = 5;    // 1 while the note's gate is open.
inline constexpr std::size_t size = 6;
} // namespace telemetry

// Torque v1: a mono bass voice with four models (Acid diode ladder, Moog
// ladder, Reese detuned saws with comb, Sub/808 sine with pitch drop),
// accent with the 303 sweep, exponential slide, a built-in 16-step
// sequencer that runs while a note is held, and distortion -> chorus ->
// delay. Defaults are the "Acid Classic" preset.
[[nodiscard]] const SynthEngine& engine();

} // namespace oscillate::native_dsp::synth::torque
