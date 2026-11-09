#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Orbit factory presets: only values that differ from the table defaults
// (the "Classic Chorus"). Mode values are 0-based (see orbit_table::Mode).
namespace oscillate::native_dsp::effects::orbit_presets {

using V = std::pair<std::string_view, double>;

inline constexpr std::array lush_chorus{V{"orbit.voices", 3}, V{"orbit.depth", 0.7},
                                        V{"orbit.rate", 0.35}, V{"orbit.time", 16},
                                        V{"orbit.mix", 0.6}};
inline constexpr std::array string_ensemble{V{"orbit.mode", 1}, V{"orbit.depth", 0.7},
                                            V{"orbit.mix", 0.7}};
inline constexpr std::array dimension_two{V{"orbit.mode", 2}, V{"orbit.time", 8},
                                          V{"orbit.depth", 0.4}, V{"orbit.rate", 0.5},
                                          V{"orbit.shape", 1}};
inline constexpr std::array dimension_four{V{"orbit.mode", 2}, V{"orbit.time", 9},
                                           V{"orbit.depth", 0.8}, V{"orbit.rate", 0.8},
                                           V{"orbit.shape", 1}};
inline constexpr std::array jet_flanger{V{"orbit.mode", 3},       V{"orbit.time", 4},
                                        V{"orbit.depth", 0.9},    V{"orbit.rate", 0.15},
                                        V{"orbit.feedback", 0.8}, V{"orbit.shape", 1}};
inline constexpr std::array negative_flanger{V{"orbit.mode", 3},        V{"orbit.time", 3},
                                             V{"orbit.depth", 0.8},     V{"orbit.rate", 0.25},
                                             V{"orbit.feedback", -0.7}, V{"orbit.shape", 1}};
inline constexpr std::array synced_flange{V{"orbit.mode", 3},      V{"orbit.time", 5},
                                          V{"orbit.depth", 1},     V{"orbit.sync", 1},
                                          V{"orbit.division", 13}, V{"orbit.feedback", 0.6}};
inline constexpr std::array phase_ninety{V{"orbit.mode", 4}, V{"orbit.rate", 0.4},
                                         V{"orbit.depth", 0.6}, V{"orbit.feedback", 0.2},
                                         V{"orbit.stereo", 0}};
inline constexpr std::array deep_phaser{V{"orbit.mode", 4},        V{"orbit.stages", 4},
                                        V{"orbit.rate", 0.12},     V{"orbit.depth", 0.85},
                                        V{"orbit.feedback", 0.65}, V{"orbit.centre", 600}};
inline constexpr std::array tape_wow{V{"orbit.mode", 5},    V{"orbit.rate", 0.7},
                                     V{"orbit.depth", 0.2}, V{"orbit.time", 6},
                                     V{"orbit.mix", 1},     V{"orbit.stereo", 0}};
inline constexpr std::array rotary_vibrato{V{"orbit.mode", 5}, V{"orbit.rate", 6.5},
                                           V{"orbit.depth", 0.25}, V{"orbit.time", 3},
                                           V{"orbit.mix", 1}};
inline constexpr std::array tremolo{V{"orbit.mode", 6}, V{"orbit.rate", 5}, V{"orbit.depth", 0.6},
                                    V{"orbit.stereo", 0}, V{"orbit.mix", 1}};
inline constexpr std::array auto_pan{V{"orbit.mode", 6},      V{"orbit.sync", 1},
                                     V{"orbit.division", 11}, V{"orbit.depth", 1},
                                     V{"orbit.stereo", 180},  V{"orbit.mix", 1}};
inline constexpr std::array init{V{"orbit.depth", 0}, V{"orbit.mix", 0}};

inline constexpr std::array<Preset, 15> presets{{
    {"Classic Chorus", "Chorus", {}},
    {"Lush Chorus", "Chorus", lush_chorus},
    {"String Ensemble", "Chorus", string_ensemble},
    {"Dimension 2", "Chorus", dimension_two},
    {"Dimension 4", "Chorus", dimension_four},
    {"Jet Flanger", "Flanger", jet_flanger},
    {"Negative Flanger", "Flanger", negative_flanger},
    {"Synced Flange", "Flanger", synced_flange},
    {"Phase Ninety", "Phaser", phase_ninety},
    {"Deep Phaser", "Phaser", deep_phaser},
    {"Tape Wow", "Vibrato", tape_wow},
    {"Rotary Vibrato", "Vibrato", rotary_vibrato},
    {"Tremolo", "Amplitude", tremolo},
    {"Auto-Pan", "Amplitude", auto_pan},
    {"Init", "Init", init},
}};

} // namespace oscillate::native_dsp::effects::orbit_presets
