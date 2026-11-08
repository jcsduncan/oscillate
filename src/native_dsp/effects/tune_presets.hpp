#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Tune presets: only values that differ from the table defaults
// (Instrument, A4 = 440 Hz, sharps).
namespace oscillate::native_dsp::effects::tune_presets {

using V = std::pair<std::string_view, double>;

inline constexpr std::array bass{V{"tune.mode", 1}};
inline constexpr std::array kick{V{"tune.mode", 2}};
inline constexpr std::array verdi{V{"tune.reference", 432}};
inline constexpr std::array baroque{V{"tune.reference", 415}};

inline constexpr std::array<Preset, 5> presets{{
    {"Instrument", "Tuner", {}},
    {"Bass", "Tuner", bass},
    {"Kick", "Tuner", kick},
    {"A = 432 Hz", "Reference", verdi},
    {"Baroque (A = 415 Hz)", "Reference", baroque},
}};

} // namespace oscillate::native_dsp::effects::tune_presets
