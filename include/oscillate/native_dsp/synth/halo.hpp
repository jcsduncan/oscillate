#pragma once

#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/native_dsp/synth/voice_pool.hpp"

#include <cstdint>

namespace oscillate::native_dsp::synth::halo {

inline constexpr std::uint32_t voice_limit = 16;
inline constexpr std::uint32_t unison_limit = 4;

// Halo v1: six operators over 32 classic algorithms with feedback, operator
// waveforms, per-operator envelopes, velocity and key scaling, run 2x
// oversampled; an optional multimode filter, an LFO, unison and
// ensemble -> chorus -> delay. Defaults are the "E-Piano" preset.
[[nodiscard]] const SynthEngine& engine();

} // namespace oscillate::native_dsp::synth::halo
