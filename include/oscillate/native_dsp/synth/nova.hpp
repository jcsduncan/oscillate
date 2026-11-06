#pragma once

#include "oscillate/native_dsp/synth/runtime.hpp"

// Nova (ADR-0057, spec 08): the flagship wavetable synth.
namespace oscillate::native_dsp::synth::nova {

[[nodiscard]] const SynthEngine& engine();

} // namespace oscillate::native_dsp::synth::nova
