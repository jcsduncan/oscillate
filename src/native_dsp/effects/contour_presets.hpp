#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Contour factory presets: only values that differ from the table defaults
// (the "Warm Low-Pass"). Model values are 0-based (see contour_table::Model).
namespace oscillate::native_dsp::effects::contour_presets {

using V = std::pair<std::string_view, double>;

inline constexpr std::array acid_sweep{
    V{"contour.model", 6},         V{"contour.cutoff", 500},   V{"contour.resonance", 0.75},
    V{"contour.drive", 9},         V{"contour.lfo.depth", 30}, V{"contour.lfo.sync", 1},
    V{"contour.lfo.division", 10}, V{"contour.lfo.shape", 1},
};
inline constexpr std::array auto_wah{
    V{"contour.model", 2},       V{"contour.cutoff", 400},   V{"contour.resonance", 0.6},
    V{"contour.env.amount", 36}, V{"contour.env.attack", 3}, V{"contour.env.release", 90},
};
inline constexpr std::array juno_pad{
    V{"contour.model", 7},     V{"contour.cutoff", 2'200}, V{"contour.resonance", 0.25},
    V{"contour.lfo.depth", 7}, V{"contour.lfo.rate", 0.2}, V{"contour.lfo.stereo", 90},
};
inline constexpr std::array sem_morph{
    V{"contour.model", 8},   V{"contour.cutoff", 1'500}, V{"contour.resonance", 0.45},
    V{"contour.morph", 0.5}, V{"contour.lfo.depth", 12}, V{"contour.lfo.rate", 0.15},
};
inline constexpr std::array telephone{
    V{"contour.model", 2},
    V{"contour.cutoff", 1'600},
    V{"contour.resonance", 0.15},
    V{"contour.drive", 6},
};
inline constexpr std::array dj_high_pass{
    V{"contour.model", 4},
    V{"contour.cutoff", 180},
    V{"contour.resonance", 0.35},
};
inline constexpr std::array stereo_phaser_sweep{
    V{"contour.model", 5},      V{"contour.cutoff", 900},   V{"contour.resonance", 0.5},
    V{"contour.lfo.depth", 36}, V{"contour.lfo.rate", 0.3}, V{"contour.lfo.stereo", 180},
    V{"contour.mix", 0.6},
};
inline constexpr std::array metallic_comb{
    V{"contour.model", 9},     V{"contour.cutoff", 220},   V{"contour.resonance", 0.85},
    V{"contour.lfo.depth", 5}, V{"contour.lfo.rate", 0.1}, V{"contour.mix", 0.7},
};
inline constexpr std::array ducked_sidechain{
    V{"contour.model", 1},      V{"contour.cutoff", 6'000},    V{"contour.env.amount", -36},
    V{"contour.env.source", 1}, V{"contour.env.release", 220},
};
// Melodic house's pad movement: the cutoff breathing on a dotted 8th against
// the kick (In A Blurry World's Diva LFO).
inline constexpr std::array dotted_breath{
    V{"contour.model", 7},     V{"contour.cutoff", 1'600}, V{"contour.resonance", 0.2},
    V{"contour.lfo.depth", 14}, V{"contour.lfo.sync", 1},   V{"contour.lfo.division", 6},
};
inline constexpr std::array init{
    V{"contour.cutoff", 20'000},
    V{"contour.resonance", 0},
};

inline constexpr std::array<Preset, 12> presets{{
    {"Warm Low-Pass", "Low-Pass", {}},
    {"Acid Sweep", "Movement", acid_sweep},
    {"Auto-Wah", "Envelope", auto_wah},
    {"Juno Pad", "Movement", juno_pad},
    {"SEM Morph", "Movement", sem_morph},
    {"Telephone", "Band-Pass", telephone},
    {"DJ High-Pass", "High-Pass", dj_high_pass},
    {"Stereo Notch Sweep", "Movement", stereo_phaser_sweep},
    {"Metallic Comb", "Comb", metallic_comb},
    {"Ducked by Sidechain", "Envelope", ducked_sidechain},
    {"Init", "Init", init},
    {"Dotted Breath", "Movement", dotted_breath},
}};

} // namespace oscillate::native_dsp::effects::contour_presets
// clang-format on
