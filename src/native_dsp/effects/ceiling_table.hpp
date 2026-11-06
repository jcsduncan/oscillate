#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <cstddef>
#include <string_view>

// Ceiling v1: the classic native limiter as an engine (ADR-0047), with its
// ranges and defaults, and Gain: drive into the limiter, the way a mastering
// limiter makes a mix louder (the ceiling alone can only turn it down).
// Lookahead is the latency, so it is not automatable.
// Ids are persisted and automated: never rename or reuse one.
namespace oscillate::native_dsp::effects::ceiling_table {

using S = synth::ParameterScale;
using T = synth::ParameterTiming;

inline constexpr double g = synth::effect_glide_seconds;

inline constexpr std::array<synth::ParameterDescriptor, 4> parameters{{
    {"ceiling.gain", "Gain", "Drive", "dB", S::linear, 0, 24, 0, {}, T::live, g},
    {"ceiling.ceiling", "Ceiling", "Limit", "dB", S::linear, -30, 0, -1, {}, T::live, g},
    {"ceiling.lookahead", "Lookahead", "Limit", "ms", S::linear, 0, 10, 5, {}, T::live, 0, false},
    {"ceiling.release", "Release", "Limit", "ms", S::logarithmic, 1, 2'000, 100, {}, T::live, g},
}};

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

} // namespace oscillate::native_dsp::effects::ceiling_table
