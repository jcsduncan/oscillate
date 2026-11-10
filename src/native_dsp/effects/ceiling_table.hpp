#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <cstddef>
#include <string_view>

// Ceiling: the mastering limiter (ADR-0117). Gain drives the mix into a
// true-peak lookahead gain stage whose fast envelope releases to a slow
// loudness envelope (Release, Character); Transients lets onsets past it and
// Clip lets every peak past it by up to its room, for the oversampled soft
// clipper (Shape) to round; the true-peak guard holds the ceiling. Link and
// Transient Link set how far the channels share their sustain and transient
// gain. Lookahead is the latency, so it is not automatable; Target is the
// loudness the panel reads against and learns to. Ids are persisted and
// automated: never rename or reuse one.
namespace oscillate::native_dsp::effects::ceiling_table {

using S = ParameterScale;
using T = ParameterTiming;

inline constexpr double g = effect_glide_seconds;

inline constexpr std::array<ParameterDescriptor, 11> parameters{{
    {"ceiling.gain", "Gain", "Drive", "dB", S::linear, 0, 24, 0, {}, T::live, g},
    {"ceiling.ceiling", "Ceiling", "Limit", "dB", S::linear, -30, 0, -1, {}, T::live, g},
    {"ceiling.lookahead", "Lookahead", "Limit", "ms", S::linear, 0.5, 10, 4, {}, T::live, 0, false},
    {"ceiling.release", "Release", "Limit", "ms", S::logarithmic, 5, 1'000, 100, {}, T::live, g},
    {"ceiling.character", "Character", "Limit", "%", S::linear, 0, 1, 0.6, {}, T::live, g},
    {"ceiling.transients", "Transients", "Clip", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    {"ceiling.clip", "Clip", "Clip", "dB", S::linear, 0, 6, 0, {}, T::live, g},
    {"ceiling.shape", "Shape", "Clip", "%", S::linear, 0, 1, 0.1, {}, T::live, g},
    {"ceiling.link", "Link", "Stereo", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"ceiling.transient_link", "Transient Link", "Stereo", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"ceiling.target",
     "Target",
     "Loudness",
     "LUFS",
     S::linear,
     -30,
     -4,
     -14,
     {},
     T::live,
     0,
     false},
}};

consteval std::size_t index_of(std::string_view id) { return parameter_index(parameters, id); }

} // namespace oscillate::native_dsp::effects::ceiling_table
