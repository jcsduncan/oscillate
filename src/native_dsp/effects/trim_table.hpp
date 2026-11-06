#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <cstddef>
#include <string_view>

// Trim v1: a utility (ADR-0042). At its defaults it is bit-exact. Ids are
// persisted: never rename or reuse one.
namespace oscillate::native_dsp::effects::trim_table {

using S = synth::ParameterScale;
using T = synth::ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array laws{"0 dB (Balance)"sv, "-3 dB"sv, "-4.5 dB"sv, "-6 dB"sv};
enum Law : int { balance, constant_power, compromise, linear };
inline constexpr std::array polarities{"None"sv, "Left"sv, "Right"sv, "Both"sv};
inline constexpr std::array solos{"Off"sv, "Left"sv, "Right"sv, "Mid"sv, "Side"sv};
enum Solo : int { none, left, right, mid, side };
inline constexpr std::array off_on{"Off"sv, "On"sv};

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = synth::effect_glide_seconds;

inline constexpr std::array<synth::ParameterDescriptor, 10> parameters{{
    {"trim.gain", "Gain", "Level", "dB", S::linear, -36, 24, 0, {}, T::live, g},
    {"trim.pan", "Pan", "Level", "", S::linear, -1, 1, 0, {}, T::live, g},
    {"trim.law", "Pan Law", "Level", "", S::choice, 0, last(laws.size()), 0, laws, T::live, 0},
    {"trim.width", "Width", "Stereo", "%", S::linear, 0, 2, 1, {}, T::live, g},
    {"trim.mono", "Mono", "Stereo", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"trim.swap", "Swap", "Stereo", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"trim.invert", "Invert", "Stereo", "", S::choice, 0, last(polarities.size()), 0, polarities,
     T::live, 0},
    {"trim.solo", "Solo", "Monitor", "", S::choice, 0, last(solos.size()), 0, solos, T::live, 0},
    {"trim.dc", "DC Block", "Repair", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"trim.rotate", "Phase Rotate", "Repair", "", S::choice, 0, 1, 0, off_on, T::live, 0},
}};

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

} // namespace oscillate::native_dsp::effects::trim_table
