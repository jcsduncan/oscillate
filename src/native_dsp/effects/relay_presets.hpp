#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Relay factory presets: only values that differ from the table defaults
// (the "Quarter Echo"). Choice values are 0-based (see relay_table).
namespace oscillate::native_dsp::effects::relay_presets {

using V = std::pair<std::string_view, double>;

inline constexpr std::array dotted_eighth{V{"relay.division", 6}, V{"relay.feedback", 0.45}};
inline constexpr std::array slapback{V{"relay.character", 1}, V{"relay.sync", 0},
                                     V{"relay.time", 110},    V{"relay.feedback", 0.08},
                                     V{"relay.mix", 0.35},    V{"relay.drive", 4}};
inline constexpr std::array tape_echo{V{"relay.character", 1},   V{"relay.sync", 0},
                                      V{"relay.time", 320},      V{"relay.feedback", 0.55},
                                      V{"relay.mod_depth", 0.4}, V{"relay.drive", 6},
                                      V{"relay.high_cut", 6'500}};
inline constexpr std::array runaway_tape{V{"relay.character", 1},    V{"relay.sync", 0},
                                         V{"relay.time", 420},       V{"relay.feedback", 0.92},
                                         V{"relay.drive", 12},       V{"relay.mod_depth", 0.5},
                                         V{"relay.high_cut", 5'000}, V{"relay.low_cut", 150}};
inline constexpr std::array analog_bucket{V{"relay.character", 2},   V{"relay.sync", 0},
                                          V{"relay.time", 300},      V{"relay.feedback", 0.6},
                                          V{"relay.mod_depth", 0.3}, V{"relay.drive", 3}};
inline constexpr std::array ping_pong_eighths{V{"relay.routing", 1}, V{"relay.division", 5},
                                              V{"relay.feedback", 0.5}};
inline constexpr std::array wide_dotted_ping_pong{V{"relay.routing", 1}, V{"relay.division", 6},
                                                  V{"relay.feedback", 0.55},
                                                  V{"relay.diffusion", 0.3}, V{"relay.mix", 0.35}};
inline constexpr std::array ducked_vocal{V{"relay.ducking", 0.7}, V{"relay.feedback", 0.4},
                                         V{"relay.mix", 0.4}, V{"relay.high_cut", 7'000},
                                         V{"relay.low_cut", 200}};
inline constexpr std::array diffuse_ambience{V{"relay.character", 2},    V{"relay.sync", 0},
                                             V{"relay.time", 450},       V{"relay.feedback", 0.7},
                                             V{"relay.diffusion", 0.85}, V{"relay.mod_depth", 0.35},
                                             V{"relay.mix", 0.4}};
inline constexpr std::array dub_siren{V{"relay.character", 2},    V{"relay.division", 6},
                                      V{"relay.feedback", 0.78},  V{"relay.drive", 9},
                                      V{"relay.high_cut", 2'500}, V{"relay.low_cut", 300},
                                      V{"relay.mix", 0.45}};
inline constexpr std::array haas_widener{
    V{"relay.sync", 0},  V{"relay.time", 12},    V{"relay.offset", 0.5},     V{"relay.feedback", 0},
    V{"relay.mix", 0.3}, V{"relay.low_cut", 20}, V{"relay.high_cut", 20'000}};
inline constexpr std::array init{V{"relay.sync", 0},          V{"relay.time", 500},
                                 V{"relay.feedback", 0.3},    V{"relay.low_cut", 20},
                                 V{"relay.high_cut", 20'000}, V{"relay.mix", 0.5}};

inline constexpr std::array<Preset, 13> presets{{
    {"Quarter Echo", "Echo", {}},
    {"Dotted Eighth", "Echo", dotted_eighth},
    {"Slapback", "Tape", slapback},
    {"Tape Echo", "Tape", tape_echo},
    {"Runaway Tape", "Tape", runaway_tape},
    {"Analog Bucket", "Analog", analog_bucket},
    {"Ping-Pong Eighths", "Stereo", ping_pong_eighths},
    {"Wide Dotted Ping-Pong", "Stereo", wide_dotted_ping_pong},
    {"Ducked Vocal", "Echo", ducked_vocal},
    {"Diffuse Ambience", "Ambient", diffuse_ambience},
    {"Dub Siren", "Analog", dub_siren},
    {"Haas Widener", "Stereo", haas_widener},
    {"Init", "Init", init},
}};

} // namespace oscillate::native_dsp::effects::relay_presets
