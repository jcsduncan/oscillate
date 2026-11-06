#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <cstddef>
#include <string_view>

// Loudness v1: an EBU R128 / ITU-R BS.1770-4 meter (ADR-0042). It passes
// audio untouched. Ids are persisted: never rename or reuse one.
namespace oscillate::native_dsp::effects::loudness_table {

using S = synth::ParameterScale;
using T = synth::ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array off_on{"Off"sv, "On"sv};
inline constexpr std::array scales{"EBU +9"sv, "EBU +18"sv};

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }

inline constexpr std::array<synth::ParameterDescriptor, 4> parameters{{
    {"loudness.target", "Target", "Target", "LUFS", S::linear, -36, -5, -14, {}, T::live, 0},
    {"loudness.ceiling", "Ceiling", "Target", "dBTP", S::linear, -6, 0, -1, {}, T::live, 0},
    // Off clears the integrated loudness and range; On measures afresh.
    {"loudness.integrate", "Integrate", "Measure", "", S::choice, 0, 1, 1, off_on, T::live, 0},
    {"loudness.scale", "Scale", "Display", "", S::choice, 0, last(scales.size()), 0, scales,
     T::live, 0},
}};

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

} // namespace oscillate::native_dsp::effects::loudness_table
