#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Vista factory presets: only values that differ from the table defaults
// (the "Natural"). Choice values are 0-based (see vista_table).
namespace oscillate::native_dsp::effects::vista_presets {

using V = std::pair<std::string_view, double>;

inline constexpr std::array wide_master{
    V{"vista.bands", 3},       V{"vista.width_1", 0.9},  V{"vista.width_2", 1.1},
    V{"vista.width_3", 1.25},  V{"vista.width_4", 1.35}, V{"vista.mono_bass", 1},
    V{"vista.mono_freq", 110}, V{"vista.safe", 1},       V{"vista.safe_floor", 0.1}};
inline constexpr std::array tight_low_end{V{"vista.mono_bass", 1}, V{"vista.mono_freq", 160}};
inline constexpr std::array huge_pads{V{"vista.width_1", 1}, V{"vista.width_2", 1.5},
                                      V{"vista.width_3", 1.8}, V{"vista.safe", 1},
                                      V{"vista.safe_floor", 0}};
inline constexpr std::array stereoize_vocal{V{"vista.stereoize", 0.4}, V{"vista.width_1", 1},
                                            V{"vista.mono_bass", 1}};
inline constexpr std::array haas_guitar{V{"vista.stereoize", 0.65}, V{"vista.stereoize_mode", 1},
                                        V{"vista.haas_time", 14}};
inline constexpr std::array narrow_lows_open_air{V{"vista.width_1", 0.5}, V{"vista.width_2", 1},
                                                 V{"vista.width_3", 1.3}};
inline constexpr std::array mono_check{V{"vista.width", 0}};
inline constexpr std::array safe_widen{V{"vista.width", 1.6}, V{"vista.safe", 1},
                                       V{"vista.safe_floor", 0.2}};
inline constexpr std::array lean_left{V{"vista.rotation", -12}};
inline constexpr std::array init{V{"vista.bands", 0}};

inline constexpr std::array<Preset, 11> presets{{
    {"Natural", "Width", {}},
    {"Wide Master", "Mastering", wide_master},
    {"Tight Low End", "Mastering", tight_low_end},
    {"Huge Pads", "Width", huge_pads},
    {"Stereoize Vocal", "Stereoize", stereoize_vocal},
    {"Haas Guitar", "Stereoize", haas_guitar},
    {"Narrow Lows, Open Air", "Width", narrow_lows_open_air},
    {"Mono Check", "Utility", mono_check},
    {"Safe Widen", "Width", safe_widen},
    {"Lean Left", "Field", lean_left},
    {"Init", "Init", init},
}};

} // namespace oscillate::native_dsp::effects::vista_presets
