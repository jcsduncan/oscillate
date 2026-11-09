#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Scope presets: only values that differ from the table defaults (a beat
// across the screen, the sidechain overlaid, low-band alignment).
namespace oscillate::native_dsp::effects::scope_presets {

using V = std::pair<std::string_view, double>;

inline constexpr std::array bar_view{V{"scope.length", 4}};
inline constexpr std::array sixteenth{V{"scope.length", 0}};
inline constexpr std::array waveform{V{"scope.sync", 0}, V{"scope.time", 20}, V{"scope.overlay", 0},
                                     V{"scope.align", 0}};
inline constexpr std::array full_band_align{V{"scope.align_band", 0}};
inline constexpr std::array stereo_side{V{"scope.channel", 1}, V{"scope.overlay", 0}};

inline constexpr std::array<Preset, 6> presets{{
    {"Kick and Bass", "Alignment", {}},
    {"Bar View", "Groove", bar_view},
    {"Sixteenth Detail", "Groove", sixteenth},
    {"Waveform", "Scope", waveform},
    {"Full-Band Alignment", "Alignment", full_band_align},
    {"Side Channel", "Scope", stereo_side},
}};

} // namespace oscillate::native_dsp::effects::scope_presets
