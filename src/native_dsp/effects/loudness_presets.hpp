#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Loudness targets: only values that differ from the table defaults
// (streaming: -14 LUFS, -1 dBTP).
namespace oscillate::native_dsp::effects::loudness_presets {

using V = std::pair<std::string_view, double>;

inline constexpr std::array apple{V{"loudness.target", -16}};
inline constexpr std::array broadcast{V{"loudness.target", -23}, V{"loudness.scale", 0}};
inline constexpr std::array atsc{V{"loudness.target", -24}, V{"loudness.ceiling", -2}};
inline constexpr std::array podcast{V{"loudness.target", -16}};
inline constexpr std::array club{V{"loudness.target", -8}, V{"loudness.ceiling", -0.3},
                                 V{"loudness.scale", 1}};
inline constexpr std::array cinema{V{"loudness.target", -27}, V{"loudness.ceiling", -2},
                                   V{"loudness.scale", 1}};

inline constexpr std::array<Preset, 7> presets{{
    {"Streaming (Spotify, YouTube)", "Music", {}},
    {"Apple Music", "Music", apple},
    {"Club Master", "Music", club},
    {"Podcast", "Speech", podcast},
    {"EBU R128 Broadcast", "Broadcast", broadcast},
    {"ATSC A/85", "Broadcast", atsc},
    {"Cinema Dialogue", "Broadcast", cinema},
}};

} // namespace oscillate::native_dsp::effects::loudness_presets
