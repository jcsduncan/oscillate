#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Forge v1: an oversampled, antialiased saturator (ADR-0040). Defaults are
// the "Warm Tape" preset. Ids are persisted and automated: never rename or
// reuse one.
namespace oscillate::native_dsp::effects::forge_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array models{"Tape"sv, "Tube"sv, "Transistor"sv, "Diode"sv,
                                   "Clip"sv, "Fold"sv, "Sine Fold"sv,  "Rectify"sv};
enum Model : std::uint8_t { tape, tube, transistor, diode, clip, fold, sine_fold, rectify };
inline constexpr std::array off_on{"Off"sv, "On"sv};

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = effect_glide_seconds;

inline constexpr std::array<ParameterDescriptor, 8> parameters{{
    {"forge.model", "Model", "Shape", "", S::choice, 0, last(models.size()), 0, models, T::live, 0},
    {"forge.drive", "Drive", "Shape", "dB", S::linear, 0, 48, 9, {}, T::live, g},
    {"forge.bias", "Bias", "Shape", "", S::linear, -1, 1, 0, {}, T::live, g},
    {"forge.colour", "Colour", "Shape", "dB", S::linear, -12, 12, 0, {}, T::live, g},
    {"forge.tone", "Tone", "Output", "Hz", S::logarithmic, 1'000, 20'000, 20'000, {}, T::live, g},
    {"forge.auto_gain", "Auto Gain", "Output", "", S::choice, 0, 1, 1, off_on, T::live, 0},
    {"forge.mix", "Mix", "Output", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"forge.output", "Output", "Output", "dB", S::linear, -24, 12, 0, {}, T::live, g},
}};

consteval std::size_t index_of(std::string_view id) { return parameter_index(parameters, id); }

} // namespace oscillate::native_dsp::effects::forge_table
