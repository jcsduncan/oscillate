#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <cstddef>
#include <string_view>

// Tune v1: a tuner (ADR-0042). It passes audio untouched. Ids are
// persisted: never rename or reuse one.
namespace oscillate::native_dsp::effects::tune_table {

using S = synth::ParameterScale;
using T = synth::ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array modes{"Instrument"sv, "Bass"sv, "Kick"sv};
enum Mode : int { instrument, bass, kick };
inline constexpr std::array notations{"Sharps"sv, "Flats"sv};

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }

inline constexpr std::array<synth::ParameterDescriptor, 3> parameters{{
    {"tune.mode", "Mode", "Tuner", "", S::choice, 0, last(modes.size()), 0, modes, T::live, 0},
    {"tune.reference", "A4", "Tuner", "Hz", S::linear, 415, 466, 440, {}, T::live, 0},
    {"tune.notation", "Notation", "Display", "", S::choice, 0, 1, 0, notations, T::live, 0},
}};

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

} // namespace oscillate::native_dsp::effects::tune_table
