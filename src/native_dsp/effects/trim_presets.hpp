#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Trim presets: only values that differ from the (bit-exact) defaults.
namespace oscillate::native_dsp::effects::trim_presets {

using V = std::pair<std::string_view, double>;

inline constexpr std::array mono_check{V{"trim.mono", 1}};
inline constexpr std::array side_solo{V{"trim.solo", 4}};
inline constexpr std::array mono_panner{V{"trim.law", 1}};
inline constexpr std::array headroom{V{"trim.dc", 1}, V{"trim.rotate", 1}};
inline constexpr std::array minus_six{V{"trim.gain", -6}};

inline constexpr std::array<Preset, 6> presets{{
    {"Unity", "Utility", {}},
    {"Mono Check", "Monitor", mono_check},
    {"Side Solo", "Monitor", side_solo},
    {"Mono Source Panner", "Utility", mono_panner},
    {"Headroom Repair", "Repair", headroom},
    {"Gain -6 dB", "Utility", minus_six},
}};

} // namespace oscillate::native_dsp::effects::trim_presets
