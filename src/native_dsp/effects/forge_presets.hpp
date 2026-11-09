#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Forge factory presets: only values that differ from the table defaults
// (the "Warm Tape"). Model values are 0-based (see forge_table::Model).
namespace oscillate::native_dsp::effects::forge_presets {

using V = std::pair<std::string_view, double>;

inline constexpr std::array tube_warmth{V{"forge.model", 1}, V{"forge.drive", 12},
                                        V{"forge.bias", 0.15}, V{"forge.colour", -3}};
inline constexpr std::array hot_tube{V{"forge.model", 1}, V{"forge.drive", 24},
                                     V{"forge.bias", 0.4}, V{"forge.tone", 9'000}};
inline constexpr std::array transistor_crunch{V{"forge.model", 2}, V{"forge.drive", 22},
                                              V{"forge.colour", 4}, V{"forge.tone", 7'500}};
inline constexpr std::array diode_grit{V{"forge.model", 3}, V{"forge.drive", 28},
                                       V{"forge.bias", -0.2}, V{"forge.tone", 6'000}};
inline constexpr std::array hard_clipper{V{"forge.model", 4}, V{"forge.drive", 6}};
inline constexpr std::array west_coast_fold{V{"forge.model", 5}, V{"forge.drive", 16},
                                            V{"forge.bias", 0.1}, V{"forge.tone", 8'000}};
inline constexpr std::array sine_shimmer{V{"forge.model", 6}, V{"forge.drive", 10},
                                         V{"forge.mix", 0.5}};
inline constexpr std::array octave_rectifier{V{"forge.model", 7}, V{"forge.drive", 12},
                                             V{"forge.mix", 0.6}};
inline constexpr std::array parallel_smash{V{"forge.model", 4}, V{"forge.drive", 36},
                                           V{"forge.mix", 0.35}, V{"forge.tone", 10'000}};
inline constexpr std::array broken_speaker{V{"forge.model", 3}, V{"forge.drive", 30},
                                           V{"forge.colour", 9}, V{"forge.tone", 3'200}};
inline constexpr std::array init{V{"forge.drive", 0}};

inline constexpr std::array<Preset, 12> presets{{
    {"Warm Tape", "Warmth", {}},
    {"Tube Warmth", "Warmth", tube_warmth},
    {"Hot Tube", "Drive", hot_tube},
    {"Transistor Crunch", "Drive", transistor_crunch},
    {"Diode Grit", "Drive", diode_grit},
    {"Hard Clipper", "Loudness", hard_clipper},
    {"West Coast Fold", "Fold", west_coast_fold},
    {"Sine Shimmer", "Fold", sine_shimmer},
    {"Octave Rectifier", "Fold", octave_rectifier},
    {"Parallel Smash", "Loudness", parallel_smash},
    {"Broken Speaker", "Lo-Fi", broken_speaker},
    {"Init", "Init", init},
}};

} // namespace oscillate::native_dsp::effects::forge_presets
