#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <cstddef>
#include <string_view>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Concert's parameter table (ADR-0071), for a sampled piano: how it is
// voiced and played (Voicing), its dampers and pedals, the mechanism's
// noises recorded with the library (Mechanics), the stereo image and the
// output. Defaults are the "Concert Grand" preset. Ids are persisted and
// automated: never rename or reuse one. Every other parameter is live and
// automatable (ADR-0034/0038).
namespace oscillate::native_dsp::synth::concert_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array perspectives{"Player"sv, "Audience"sv};

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = 0.05;

// The mechanism levels are the library's own voicing at 50 % and twice it
// at 100 %, as its SFZ mapping does.
inline constexpr double library_level = 0.5;

inline constexpr std::array<ParameterDescriptor, 13> parameters{{
    // Voicing
    {"concert.tone", "Tone", "Voicing", "dB", S::linear, -6, 6, 0, {}, T::live, g},
    // Touch and Una Corda choose a blow's layers, so they apply from the next note.
    {"concert.touch", "Touch", "Voicing", "", S::logarithmic, 0.5, 2, 1, {}, T::next_note, 0,
     false},
    {"concert.dynamics", "Dynamics", "Voicing", "%", S::linear, 0, 1.5, 1, {}, T::live, g},
    // Dampers & Pedals
    {"concert.damper", "Dampers", "Dampers & Pedals", "x", S::logarithmic, 0.25, 4, 1, {},
     T::live, g},
    {"concert.una_corda", "Una Corda", "Dampers & Pedals", "%", S::linear, 0, 1, 1, {},
     T::next_note, 0, false},
    {"concert.resonance", "Resonance", "Dampers & Pedals", "%", S::linear, 0, 1, 0.3, {},
     T::live, g},
    // Mechanics
    {"concert.noise.release", "Key Release", "Mechanics", "%", S::linear, 0, 1, library_level,
     {}, T::live, g},
    {"concert.noise.strings", "String Release", "Mechanics", "%", S::linear, 0, 1,
     library_level, {}, T::live, g},
    {"concert.noise.pedal", "Pedal", "Mechanics", "%", S::linear, 0, 1, library_level, {},
     T::live, g},
    // Stereo
    {"concert.width", "Width", "Stereo", "%", S::linear, 0, 1.5, 1, {}, T::live, g},
    {"concert.perspective", "Perspective", "Stereo", "", S::choice, 0, last(perspectives.size()),
     0, perspectives, T::live, 0},
    // Output
    {"concert.tune", "Tune", "Output", "ct", S::linear, -50, 50, 0, {}, T::live, g},
    {"concert.level", "Level", "Output", "dB", S::linear, -60, 6, 0, {}, T::live, g},
}};

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

} // namespace oscillate::native_dsp::synth::concert_table
// clang-format on
