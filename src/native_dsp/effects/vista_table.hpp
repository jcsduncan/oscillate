#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <cstddef>
#include <string_view>

// Vista v1: a stereo imager (ADR-0040). Defaults are the "Natural" preset.
// Ids are persisted and automated: never rename or reuse one.
namespace oscillate::native_dsp::effects::vista_table {

using S = synth::ParameterScale;
using T = synth::ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array band_counts{"1"sv, "2"sv, "3"sv, "4"sv};
inline constexpr std::array off_on{"Off"sv, "On"sv};
inline constexpr std::array stereoize_modes{"Diffuse"sv, "Haas"sv};
inline constexpr std::array polarities{"None"sv, "Left"sv, "Right"sv, "Both"sv};

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = synth::effect_glide_seconds;

inline constexpr std::array<synth::ParameterDescriptor, 21> parameters{{
    {"vista.bands", "Bands", "Bands", "", S::choice, 0, last(band_counts.size()), 2, band_counts,
     T::live, 0},
    {"vista.xover_1", "Low X", "Bands", "Hz", S::logarithmic, 40, 2'000, 180, {}, T::live, g},
    {"vista.xover_2", "Mid X", "Bands", "Hz", S::logarithmic, 200, 8'000, 2'500, {}, T::live, g},
    {"vista.xover_3",
     "High X",
     "Bands",
     "Hz",
     S::logarithmic,
     1'000,
     16'000,
     7'000,
     {},
     T::live,
     g},
    {"vista.width_1", "Low", "Width", "%", S::linear, 0, 2, 1, {}, T::live, g},
    {"vista.width_2", "Low Mid", "Width", "%", S::linear, 0, 2, 1, {}, T::live, g},
    {"vista.width_3", "High Mid", "Width", "%", S::linear, 0, 2, 1, {}, T::live, g},
    {"vista.width_4", "High", "Width", "%", S::linear, 0, 2, 1, {}, T::live, g},
    {"vista.width", "Width", "Width", "%", S::linear, 0, 2, 1, {}, T::live, g},
    {"vista.mono_bass", "Mono Bass", "Bass", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"vista.mono_freq", "Below", "Bass", "Hz", S::logarithmic, 20, 500, 120, {}, T::live, g},
    {"vista.stereoize", "Stereoize", "Stereoize", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"vista.stereoize_mode", "Mode", "Stereoize", "", S::choice, 0, 1, 0, stereoize_modes, T::live,
     0},
    {"vista.haas_time", "Time", "Stereoize", "ms", S::logarithmic, 1, 30, 12, {}, T::live, g},
    {"vista.safe", "Safe Width", "Safety", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"vista.safe_floor", "Floor", "Safety", "", S::linear, -1, 1, 0, {}, T::live, g},
    {"vista.rotation", "Rotation", "Field", "deg", S::linear, -45, 45, 0, {}, T::live, g},
    {"vista.balance", "Balance", "Field", "", S::linear, -1, 1, 0, {}, T::live, g},
    {"vista.swap", "Swap", "Field", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"vista.invert", "Invert", "Field", "", S::choice, 0, last(polarities.size()), 0, polarities,
     T::live, 0},
    {"vista.output", "Output", "Output", "dB", S::linear, -24, 12, 0, {}, T::live, g},
}};

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

} // namespace oscillate::native_dsp::effects::vista_table
