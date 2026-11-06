#pragma once

#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/native_dsp/synth/voice_pool.hpp"

#include <cstddef>
#include <cstdint>

namespace oscillate::native_dsp::synth::pulse {

inline constexpr std::uint8_t first_key = 36; // C1
inline constexpr std::uint32_t pad_count = 12;

// Telemetry layout (ADR-0037).
namespace telemetry {
inline constexpr std::size_t gain_reduction = 0; // Glue compressor, dB (>= 0).
inline constexpr std::size_t first_pad = 1;      // Then each pad's envelope 0..1, key order.
inline constexpr std::size_t size = first_pad + pad_count;
} // namespace telemetry

// Pulse v1: an analog-modelled drum machine on keys C1..B1 — kick, 909 kick,
// snare, rim, clap, three toms, closed and open hats (the closed hat chokes
// the open one), cowbell and cymbal — each a one-shot mono voice with level,
// pan, tune, decay, tone, drive, velocity and a character control, into a
// glue compressor and saturation. It has no reverb of its own: send it to a
// return with a reverb. A kick's character is its beater click, noise in a
// knock band. Note-offs are ignored; other keys are silent. Defaults are the
// "808 Classic" kit.
[[nodiscard]] const SynthEngine& engine();

} // namespace oscillate::native_dsp::synth::pulse
