#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Edge presets: only values that differ from the table defaults. On the
// source each is for, peaking at -1 dBFS, they clip at most 2.9 (Kick
// Punch), 3.2 (Drum Bus), 3.1 (808) and 1.5 dB (Master), measured in
// edge_tests.cpp (presets_stay_restrained). Oversampling values are 0-based
// (edge_table::Oversampling).
namespace oscillate::native_dsp::effects::edge_presets {

using V = std::pair<std::string_view, double>;
using namespace std::string_view_literals;

// A hard flat top on the kick's first cycle: the attack gets louder, the
// body is untouched.
inline constexpr std::array kick_punch{V{"edge.input", 3}, V{"edge.shape", 0.1}};
// Rounder on a whole kit, so hats and snares fold less.
inline constexpr std::array drum_bus{V{"edge.input", 2}, V{"edge.shape", 0.35}};
// A soft knee on the sub adds the harmonics that let an 808 read on small
// speakers; it hardly aliases, so 4x is enough.
inline constexpr std::array eight_oh_eight{V{"edge.input", 4}, V{"edge.shape", 0.7},
                                           V{"edge.oversampling", 2}};
// The last stage: a little drive, a soft knee, the cleanest filters.
inline constexpr std::array master{V{"edge.input", 2}, V{"edge.shape", 0.5},
                                   V{"edge.oversampling", 4}};

inline constexpr std::array kick_tags{"kick"sv, "drums"sv, "punch"sv};
inline constexpr std::array bus_tags{"drums"sv, "bus"sv, "glue"sv};
inline constexpr std::array bass_tags{"808"sv, "bass"sv, "sub"sv};
inline constexpr std::array master_tags{"master"sv, "loudness"sv};

inline constexpr std::array<synth::SynthPreset, 5> presets{{
    {"Default", "Init", {}},
    {"Kick Punch", "Drums", kick_punch, kick_tags},
    {"Drum Bus", "Drums", drum_bus, bus_tags},
    {"808", "Bass", eight_oh_eight, bass_tags},
    {"Master", "Master", master, master_tags},
}};

} // namespace oscillate::native_dsp::effects::edge_presets
