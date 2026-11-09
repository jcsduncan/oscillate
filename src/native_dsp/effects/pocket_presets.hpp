#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Pocket factory presets: only values that differ from the table defaults
// (Unmask, 9 dB, no lookahead). Choice values are 0-based (see
// pocket_table). Every preset but Init looks 20 ms ahead, which puts the
// cut on time with the kick.
namespace oscillate::native_dsp::effects::pocket_presets {

using V = std::pair<std::string_view, double>;

inline constexpr std::array clean_low_end{V{"pocket.lookahead", 3}};
inline constexpr std::array sub_only{V{"pocket.focus_high", 110}, V{"pocket.depth", 12},
                                     V{"pocket.margin", 4}, V{"pocket.lookahead", 3}};
inline constexpr std::array punchy_house{V{"pocket.mode", 1}, V{"pocket.depth", 8},
                                         V{"pocket.focus_high", 180}, V{"pocket.follow", 1},
                                         V{"pocket.lookahead", 3}};
inline constexpr std::array gentle_rock{V{"pocket.depth", 5}, V{"pocket.margin", 1},
                                        V{"pocket.focus_high", 400}, V{"pocket.mono_sub", 0},
                                        V{"pocket.lookahead", 3}};
inline constexpr std::array eight_oh_eight{V{"pocket.depth", 14},     V{"pocket.margin", 6},
                                           V{"pocket.focus_low", 25}, V{"pocket.focus_high", 160},
                                           V{"pocket.release", 160},  V{"pocket.follow", 0},
                                           V{"pocket.lookahead", 3}};
inline constexpr std::array classic_pump{V{"pocket.mode", 2},       V{"pocket.depth", 18},
                                         V{"pocket.focus_low", 20}, V{"pocket.focus_high", 2'000},
                                         V{"pocket.release", 220},  V{"pocket.follow", 0},
                                         V{"pocket.lookahead", 3}};
inline constexpr std::array init{V{"pocket.depth", 0}, V{"pocket.mono_sub", 0}};

inline constexpr std::array<Preset, 7> presets{{
    {"Clean Low End", "Unmask", clean_low_end},
    {"Sub Only", "Unmask", sub_only},
    {"Punchy House", "Duck", punchy_house},
    {"Gentle Rock", "Unmask", gentle_rock},
    {"808 And Kick", "Unmask", eight_oh_eight},
    {"Classic Pump", "Pump", classic_pump},
    {"Init", "Init", init},
}};

} // namespace oscillate::native_dsp::effects::pocket_presets
