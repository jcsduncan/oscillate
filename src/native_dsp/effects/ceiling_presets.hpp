#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Ceiling presets: only values that differ from the table defaults (half
// the onset room, a nearly hard knee, a 100 ms release to a 60% loudness
// stage). Measured at equal loudness by tools/limiter_audit
// (docs/performance/native-effects.md).
namespace oscillate::native_dsp::effects::ceiling_presets {

using V = std::pair<std::string_view, double>;
using namespace std::string_view_literals;

// The least audible: a slower release that rests on a fuller loudness
// stage, a little room for onsets.
inline constexpr std::array transparent{V{"ceiling.release", 150}, V{"ceiling.character", 0.8},
                                        V{"ceiling.transients", 0.3}};
// Loud and dense: a fast release, full onset room and every peak rounded
// by a softer clipper, under -0.3 dBTP.
inline constexpr std::array loud_club{V{"ceiling.gain", 6},     V{"ceiling.ceiling", -0.3},
                                      V{"ceiling.release", 50}, V{"ceiling.transients", 1},
                                      V{"ceiling.clip", 1},     V{"ceiling.shape", 0.2},
                                      V{"ceiling.target", -7}};
// Streaming loudness, a dB of headroom under 0 dBTP for the encoders.
inline constexpr std::array streaming{V{"ceiling.target", -14}};
// Drums forward: full onset room, a hard knee, and the onsets' gain per
// channel so a hat on one side does not duck the other.
inline constexpr std::array punchy{V{"ceiling.release", 60}, V{"ceiling.transients", 1},
                                   V{"ceiling.shape", 0}, V{"ceiling.transient_link", 0.5},
                                   V{"ceiling.target", -9}};
// A bus or a quiet master: slow, steady and gentle on onsets.
inline constexpr std::array gentle_bus{V{"ceiling.lookahead", 6}, V{"ceiling.release", 250},
                                       V{"ceiling.character", 1}, V{"ceiling.transients", 0.25},
                                       V{"ceiling.shape", 0.4},   V{"ceiling.target", -18}};

inline constexpr std::array master_tags{"master"sv, "transparent"sv};
inline constexpr std::array club_tags{"master"sv, "loud"sv, "club"sv};
inline constexpr std::array streaming_tags{"master"sv, "streaming"sv, "loudness"sv};
inline constexpr std::array punch_tags{"master"sv, "drums"sv, "punch"sv};
inline constexpr std::array bus_tags{"bus"sv, "gentle"sv};

inline constexpr std::array<Preset, 6> presets{{
    {"Default", "Master", {}},
    {"Transparent", "Master", transparent, master_tags},
    {"Loud Club", "Master", loud_club, club_tags},
    {"Streaming -14", "Master", streaming, streaming_tags},
    {"Punchy", "Master", punchy, punch_tags},
    {"Gentle Bus", "Bus", gentle_bus, bus_tags},
}};

} // namespace oscillate::native_dsp::effects::ceiling_presets
