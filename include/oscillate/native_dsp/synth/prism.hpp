#pragma once

#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/native_dsp/synth/voice_pool.hpp"

#include <cstdint>

namespace oscillate::native_dsp::synth::prism {

inline constexpr std::uint32_t voice_limit = 16;
inline constexpr std::uint32_t unison_limit = 8;

// Prism v1: two band-limited oscillators with unison, sub, noise and ring,
// a 2x-oversampled multimode filter, two envelopes, two LFOs, a four-slot
// matrix, drive and chorus. Defaults are the "Prism Default" keys patch.
[[nodiscard]] const SynthEngine& engine();

} // namespace oscillate::native_dsp::synth::prism
