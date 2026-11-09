#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Shape presets: only values that differ from the (all bands off) defaults.
namespace oscillate::native_dsp::effects::shape_presets {

using V = std::pair<std::string_view, double>;

inline constexpr std::array low_cut{V{"shape.on_1", 1}, V{"shape.freq_1", 80}};
// Brick-wall ends: 48 dB an octave below 30 Hz and above 18 kHz.
inline constexpr std::array clean_ends{V{"shape.on_1", 1}, V{"shape.slope_1", 2},
                                       V{"shape.on_8", 1}, V{"shape.slope_8", 2}};
inline constexpr std::array presence{V{"shape.on_5", 1}, V{"shape.freq_5", 3'500},
                                     V{"shape.gain_5", 3}};
inline constexpr std::array air{V{"shape.on_7", 1}, V{"shape.freq_7", 12'000},
                                V{"shape.gain_7", 4}};
inline constexpr std::array mud_cut{V{"shape.on_3", 1}, V{"shape.freq_3", 300}, V{"shape.q_3", 1.4},
                                    V{"shape.gain_3", -3}};
inline constexpr std::array warmth{V{"shape.on_2", 1}, V{"shape.freq_2", 120}, V{"shape.gain_2", 2},
                                   V{"shape.on_8", 1}, V{"shape.freq_8", 16'000}};
inline constexpr std::array vocal{
    V{"shape.on_1", 1},    V{"shape.freq_1", 90}, V{"shape.on_3", 1},       V{"shape.freq_3", 300},
    V{"shape.gain_3", -2}, V{"shape.on_5", 1},    V{"shape.freq_5", 3'000}, V{"shape.gain_5", 2},
    V{"shape.on_7", 1},    V{"shape.gain_7", 2}};

inline constexpr std::array<Preset, 8> presets{{
    {"Flat", "Utility", {}},
    {"Low Cut", "Utility", low_cut},
    {"Clean Ends", "Utility", clean_ends},
    {"Presence", "Tone", presence},
    {"Air", "Tone", air},
    {"Mud Cut", "Tone", mud_cut},
    {"Warmth", "Tone", warmth},
    {"Vocal", "Vocal", vocal},
}};

} // namespace oscillate::native_dsp::effects::shape_presets
