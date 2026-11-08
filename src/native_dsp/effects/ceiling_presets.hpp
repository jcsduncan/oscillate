#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Ceiling presets: only values that differ from the (classic) defaults.
namespace oscillate::native_dsp::effects::ceiling_presets {

using V = std::pair<std::string_view, double>;

inline constexpr std::array transparent{V{"ceiling.release", 250}};
inline constexpr std::array loud{V{"ceiling.gain", 6}, V{"ceiling.ceiling", -0.3},
                                 V{"ceiling.lookahead", 2}, V{"ceiling.release", 40}};
inline constexpr std::array safety{V{"ceiling.ceiling", -0.1}, V{"ceiling.lookahead", 1},
                                   V{"ceiling.release", 50}};
inline constexpr std::array broadcast{V{"ceiling.ceiling", -2}, V{"ceiling.lookahead", 8},
                                      V{"ceiling.release", 400}};

inline constexpr std::array<Preset, 5> presets{{
    {"Default", "Master", {}},
    {"Transparent", "Master", transparent},
    {"Loud", "Master", loud},
    {"Safety", "Utility", safety},
    {"Broadcast", "Master", broadcast},
}};

} // namespace oscillate::native_dsp::effects::ceiling_presets
