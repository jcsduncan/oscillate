#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Press presets: only values that differ from the (classic) defaults.
namespace oscillate::native_dsp::effects::press_presets {

using V = std::pair<std::string_view, double>;

inline constexpr std::array gentle_glue{V{"press.threshold", -20}, V{"press.ratio", 2},
                                        V{"press.attack", 30}, V{"press.release", 200},
                                        V{"press.makeup", 2}};
inline constexpr std::array vocal{V{"press.threshold", -24},    V{"press.ratio", 3},
                                  V{"press.attack", 5},         V{"press.release", 80},
                                  V{"press.makeup", 4},         V{"press.detector_hp", 1},
                                  V{"press.detector_freq", 120}};
inline constexpr std::array drum_bus{V{"press.threshold", -16},   V{"press.knee", 3},
                                     V{"press.attack", 30},       V{"press.release", 120},
                                     V{"press.makeup", 3},        V{"press.detector_hp", 1},
                                     V{"press.detector_freq", 60}};
inline constexpr std::array squash{V{"press.threshold", -36}, V{"press.ratio", 12},
                                   V{"press.knee", 0},        V{"press.attack", 1},
                                   V{"press.release", 50},    V{"press.makeup", 10}};
inline constexpr std::array bass{V{"press.threshold", -22}, V{"press.ratio", 5},
                                 V{"press.attack", 20}, V{"press.release", 150},
                                 V{"press.makeup", 3}};

inline constexpr std::array<Preset, 6> presets{{
    {"Default", "Utility", {}},
    {"Gentle Glue", "Bus", gentle_glue},
    {"Vocal", "Vocal", vocal},
    {"Drum Bus", "Bus", drum_bus},
    {"Squash", "Character", squash},
    {"Bass", "Instrument", bass},
}};

} // namespace oscillate::native_dsp::effects::press_presets
