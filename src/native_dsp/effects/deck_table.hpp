#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <cstddef>
#include <string_view>

// Deck v1: a DJ mixer's three-band isolator (ADR-0055). Ids are persisted:
// never rename or reuse one.
namespace oscillate::native_dsp::effects::deck_table {

using S = synth::ParameterScale;
using T = synth::ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array off_on{"Off"sv, "On"sv};

// A band at its bottom stop is silent: a full kill, as on a DJ mixer.
inline constexpr double kill_db = -40.0;
inline constexpr double boost_db = 6.0;
inline constexpr double g = synth::effect_glide_seconds;

inline constexpr std::array<synth::ParameterDescriptor, 9> parameters{{
    {"deck.low", "Low", "Bands", "dB", S::linear, kill_db, boost_db, 0, {}, T::live, g},
    {"deck.mid", "Mid", "Bands", "dB", S::linear, kill_db, boost_db, 0, {}, T::live, g},
    {"deck.high", "High", "Bands", "dB", S::linear, kill_db, boost_db, 0, {}, T::live, g},
    {"deck.low_kill", "Low Kill", "Kills", "", S::choice, 0, 1, 0, off_on, T::live, 0, false},
    {"deck.mid_kill", "Mid Kill", "Kills", "", S::choice, 0, 1, 0, off_on, T::live, 0, false},
    {"deck.high_kill", "High Kill", "Kills", "", S::choice, 0, 1, 0, off_on, T::live, 0, false},
    {"deck.low_x", "Low X", "Crossovers", "Hz", S::logarithmic, 60, 600, 250, {}, T::live, g},
    {"deck.high_x",
     "High X",
     "Crossovers",
     "Hz",
     S::logarithmic,
     1'000,
     8'000,
     2'500,
     {},
     T::live,
     g},
    {"deck.gain", "Gain", "Output", "dB", S::linear, -24, 12, 0, {}, T::live, g},
}};

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

} // namespace oscillate::native_dsp::effects::deck_table
