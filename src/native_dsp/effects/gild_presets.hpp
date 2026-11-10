#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Gild presets: only values that differ from the table defaults. Each
// excites one region of the spectrum and leaves the rest dry. What each
// adds sits 18 to 60 dB under the programme and the loudness moves by under
// half a decibel: measured in gild_tests.cpp (presets_stay_restrained) and,
// on mastered tracks, by oscillate_limiter_audit --exciter. Band counts, phases, curves and
// oversampling factors are 0-based choice indices (gild_table::band_counts,
// phases, curves, oversampling).
namespace oscillate::native_dsp::effects::gild_presets {

using V = std::pair<std::string_view, double>;
using namespace std::string_view_literals;

// Over 8 kHz, tape with a touch of even, antialiased at 8x: the top
// octave lifted without a shelf's hiss. Dynamic keeps it going through
// quiet passages.
inline constexpr std::array air_lift{V{"gild.bands", 1},         V{"gild.xover_1", 8'000},
                                     V{"gild.oversampling", 3},  V{"gild.band_2.drive", 0.65},
                                     V{"gild.band_2.even", 0.3}, V{"gild.band_2.mix", 0.6},
                                     V{"gild.dynamic", 0.5}};
// 250 Hz to 2.5 kHz through the tube, mostly even: body without boxiness.
inline constexpr std::array warm_mids{V{"gild.bands", 2},        V{"gild.xover_1", 250},
                                      V{"gild.xover_2", 2'500},  V{"gild.band_2.drive", 0.4},
                                      V{"gild.band_2.curve", 1}, V{"gild.band_2.even", 0.6},
                                      V{"gild.band_2.mix", 0.5}, V{"gild.dynamic", 0.5}};
// Under 120 Hz through the tube: the second and third harmonics carry the
// bass on speakers that cannot play its fundamental.
inline constexpr std::array bass_weight{V{"gild.bands", 1},          V{"gild.xover_1", 120},
                                        V{"gild.band_1.drive", 0.4}, V{"gild.band_1.curve", 1},
                                        V{"gild.band_1.even", 0.7},  V{"gild.band_1.mix", 0.5},
                                        V{"gild.dynamic", 0.5}};
// All four bands lightly through tape at a linear phase: glue, not grit.
inline constexpr std::array tape_glue{
    V{"gild.phase", 1},           V{"gild.band_1.drive", 0.25}, V{"gild.band_2.drive", 0.25},
    V{"gild.band_3.drive", 0.25}, V{"gild.band_4.drive", 0.2},  V{"gild.band_1.even", 0.3},
    V{"gild.band_2.even", 0.3},   V{"gild.band_3.even", 0.3},   V{"gild.band_4.even", 0.3}};
// 1.5 to 6 kHz through the transistor, antialiased at 8x: presence for a
// voice, its sibilance band left dry.
inline constexpr std::array vocal_presence{V{"gild.bands", 2},        V{"gild.xover_1", 1'500},
                                           V{"gild.xover_2", 6'000},  V{"gild.band_2.drive", 0.55},
                                           V{"gild.band_2.curve", 2}, V{"gild.band_2.even", 0.2},
                                           V{"gild.band_2.mix", 0.5}, V{"gild.oversampling", 3}};

inline constexpr std::array air_tags{"air"sv, "top"sv, "bright"sv, "master"sv};
inline constexpr std::array mids_tags{"warm"sv, "mids"sv, "body"sv, "tube"sv};
inline constexpr std::array bass_tags{"bass"sv, "harmonics"sv, "small speakers"sv, "weight"sv};
inline constexpr std::array glue_tags{"tape"sv, "glue"sv, "master"sv, "multiband"sv};
inline constexpr std::array vocal_tags{"vocal"sv, "presence"sv, "mids"sv, "forward"sv};

inline constexpr std::array<Preset, 6> presets{{
    {"Default", "Init", {}},
    {"Air Lift", "Master", air_lift, air_tags},
    {"Warm Mids", "Mix", warm_mids, mids_tags},
    {"Bass Weight", "Mix", bass_weight, bass_tags},
    {"Tape Glue", "Master", tape_glue, glue_tags},
    {"Vocal Presence", "Vocal", vocal_presence, vocal_tags},
}};

} // namespace oscillate::native_dsp::effects::gild_presets
