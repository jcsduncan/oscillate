#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Edge v1: a clipper under a fixed 0 dBFS ceiling (ADR-0108). Oversampling
// and the filters set the latency, so they are not automatable: a change
// rebuilds the route. Ids are persisted and automated: never rename or
// reuse one.
namespace oscillate::native_dsp::effects::edge_table {

using S = synth::ParameterScale;
using T = synth::ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array oversampling{"1x"sv, "2x"sv, "4x"sv, "8x"sv, "16x"sv};
enum Oversampling : std::uint8_t { x1, x2, x4, x8, x16 };
inline constexpr std::array filters{"Linear Phase"sv, "Light"sv};
enum Filters : std::uint8_t { linear_phase, light };
inline constexpr std::array off_on{"Off"sv, "On"sv};

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = synth::effect_glide_seconds;

inline constexpr std::array<synth::ParameterDescriptor, 9> parameters{{
    {"edge.input", "Input", "Clip", "dB", S::linear, -12, 24, 0, {}, T::live, g},
    {"edge.shape", "Shape", "Clip", "%", S::linear, 0, 1, 0.25, {}, T::live, g},
    {"edge.output", "Output", "Output", "dB", S::linear, -24, 0, 0, {}, T::live, g},
    {"edge.oversampling", "Oversampling", "Quality", "", S::choice, 0,
     last(oversampling.size()), Oversampling::x8, oversampling, T::live, 0, false},
    {"edge.filters", "Filters", "Quality", "", S::choice, 0, last(filters.size()),
     Filters::linear_phase, filters, T::live, 0, false},
    {"edge.link", "Stereo Link", "Clip", "", S::choice, 0, 1, 1, off_on, T::live, 0},
    {"edge.delta", "Delta", "Output", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"edge.compare", "Compare", "Output", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"edge.match", "Level Match", "Output", "", S::choice, 0, 1, 1, off_on, T::live, 0},
}};

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

} // namespace oscillate::native_dsp::effects::edge_table
