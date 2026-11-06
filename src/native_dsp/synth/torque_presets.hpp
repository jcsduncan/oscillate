#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Torque factory presets: only values that differ from the table defaults
// (the "Acid Classic"). Model 0 Acid, 1 Moog, 2 Reese, 3 Sub. Golden
// fingerprints pin each.
namespace oscillate::native_dsp::synth::torque_presets {

using V = std::pair<std::string_view, double>;

inline constexpr std::array acid_squelch{
    V{"torque.cutoff", 260},      V{"torque.resonance", 0.9}, V{"torque.envmod", 0.85},
    V{"torque.decay", 0.22},      V{"torque.accent", 0.9},    V{"torque.fx.dist_drive", 0.6},
    V{"torque.fx.dist_mix", 0.6}, V{"torque.fx.delay", 0.18}, V{"torque.out.level", -2},
};

inline constexpr std::array rubber_acid{
    V{"torque.acid.wave", 1},    V{"torque.acid.tilt", 0.6}, V{"torque.cutoff", 650},
    V{"torque.resonance", 0.55}, V{"torque.envmod", 0.45},   V{"torque.decay", 0.45},
    V{"torque.slide", 0.09},     V{"torque.fx.dist_mix", 0}, V{"torque.fx.chorus", 1},
};

inline constexpr std::array seq_acid_line{
    V{"torque.seq.on", 1},         V{"torque.cutoff", 380},    V{"torque.resonance", 0.8},
    V{"torque.envmod", 0.7},       V{"torque.decay", 0.25},    V{"torque.fx.delay", 0.22},
    V{"torque.fx.dist_mix", 0.45}, V{"torque.seq.swing", 0.2},
};

inline constexpr std::array moog_bass{
    V{"torque.model", 1},         V{"torque.cutoff", 520},       V{"torque.resonance", 0.25},
    V{"torque.envmod", 0.45},     V{"torque.decay", 0.4},        V{"torque.env.sustain", 0.25},
    V{"torque.keytrack", 0.4},    V{"torque.drive", 0.35},       V{"torque.amp.decay", 2},
    V{"torque.amp.sustain", 0.8}, V{"torque.amp.release", 0.08}, V{"torque.fx.dist_mix", 0},
};

inline constexpr std::array funk_pluck{
    V{"torque.model", 1},         V{"torque.moog.wave3", 0},     V{"torque.moog.interval3", 12},
    V{"torque.moog.level3", 0.3}, V{"torque.cutoff", 300},       V{"torque.resonance", 0.4},
    V{"torque.envmod", 0.7},      V{"torque.decay", 0.15},       V{"torque.amp.decay", 0.35},
    V{"torque.amp.sustain", 0.2}, V{"torque.amp.release", 0.05}, V{"torque.fx.dist_mix", 0.15},
};

inline constexpr std::array taurus_pedal{
    V{"torque.model", 1},         V{"torque.moog.level3", 0.6}, V{"torque.moog.interval3", -12},
    V{"torque.moog.wave3", 0},    V{"torque.moog.detune", 12},  V{"torque.cutoff", 380},
    V{"torque.resonance", 0.15},  V{"torque.envmod", 0.3},      V{"torque.decay", 1.2},
    V{"torque.env.sustain", 0.4}, V{"torque.amp.attack", 0.01}, V{"torque.amp.sustain", 1},
    V{"torque.amp.release", 0.6}, V{"torque.slide", 0.15},      V{"torque.fx.dist_mix", 0},
    V{"torque.fx.chorus", 1},
};

inline constexpr std::array synthwave_bass{
    V{"torque.model", 1},         V{"torque.moog.wave2", 2},    V{"torque.moog.interval2", 12},
    V{"torque.moog.level2", 0.4}, V{"torque.moog.level3", 0},   V{"torque.cutoff", 900},
    V{"torque.resonance", 0.2},   V{"torque.envmod", 0.4},      V{"torque.decay", 0.2},
    V{"torque.amp.decay", 0.4},   V{"torque.amp.sustain", 0.5}, V{"torque.amp.release", 0.06},
    V{"torque.fx.chorus", 2},     V{"torque.fx.delay", 0.12},   V{"torque.fx.dist_mix", 0},
};

inline constexpr std::array reese{
    V{"torque.model", 2},          V{"torque.cutoff", 900},       V{"torque.resonance", 0.15},
    V{"torque.envmod", 0.15},      V{"torque.decay", 0.8},        V{"torque.env.sustain", 0.5},
    V{"torque.drive", 0.4},        V{"torque.amp.attack", 0.005}, V{"torque.amp.release", 0.2},
    V{"torque.lfo.movement", 0.3}, V{"torque.lfo.division", 11},  V{"torque.fx.dist_mix", 0.2},
    V{"torque.out.level", -2},
};

inline constexpr std::array neuro_growl{
    V{"torque.model", 2},           V{"torque.reese.filter", 1},  V{"torque.reese.voices", 7},
    V{"torque.reese.detune", 30},   V{"torque.reese.comb", 0.7},  V{"torque.cutoff", 700},
    V{"torque.resonance", 0.55},    V{"torque.envmod", 0.2},      V{"torque.env.sustain", 0.6},
    V{"torque.decay", 0.6},         V{"torque.drive", 0.8},       V{"torque.lfo.movement", 0.8},
    V{"torque.lfo.cutoff", 0.3},    V{"torque.lfo.division", 5},  V{"torque.fx.dist_mode", 1},
    V{"torque.fx.dist_drive", 0.5}, V{"torque.fx.dist_mix", 0.5}, V{"torque.amp.release", 0.1},
    V{"torque.out.level", -4},
};

inline constexpr std::array wobble_reese{
    V{"torque.model", 2},          V{"torque.cutoff", 300},     V{"torque.resonance", 0.45},
    V{"torque.envmod", 0},         V{"torque.lfo.cutoff", 0.9}, V{"torque.lfo.division", 7},
    V{"torque.lfo.shape", 0},      V{"torque.drive", 0.5},      V{"torque.amp.release", 0.15},
    V{"torque.fx.dist_mix", 0.35}, V{"torque.out.level", -2},
};

inline constexpr std::array deep_sub{
    V{"torque.model", 3},          V{"torque.cutoff", 400},       V{"torque.resonance", 0},
    V{"torque.envmod", 0},         V{"torque.drive", 0.15},       V{"torque.sub.drop", 0},
    V{"torque.amp.attack", 0.005}, V{"torque.amp.release", 0.12}, V{"torque.fx.dist_mix", 0},
};

inline constexpr std::array glide_808{
    V{"torque.model", 3},       V{"torque.cutoff", 1'500},  V{"torque.resonance", 0},
    V{"torque.envmod", 0},      V{"torque.sub.drop", 12},   V{"torque.sub.drop_time", 0.04},
    V{"torque.amp.decay", 2.5}, V{"torque.amp.sustain", 0}, V{"torque.amp.release", 0.3},
    V{"torque.slide", 0.18},    V{"torque.drive", 0.35},    V{"torque.fx.dist_mix", 0.2},
};

inline constexpr std::array distorted_808{
    V{"torque.model", 3},         V{"torque.cutoff", 2'500},       V{"torque.resonance", 0},
    V{"torque.envmod", 0},        V{"torque.sub.drop", 10},        V{"torque.amp.decay", 1.8},
    V{"torque.amp.sustain", 0},   V{"torque.amp.release", 0.25},   V{"torque.drive", 0.7},
    V{"torque.fx.dist_mode", 1},  V{"torque.fx.dist_drive", 0.55}, V{"torque.fx.dist_tone", 0.45},
    V{"torque.fx.dist_mix", 0.7}, V{"torque.slide", 0.12},         V{"torque.out.level", -3},
};

inline constexpr std::array dub_sub{
    V{"torque.model", 3},
    V{"torque.sub.layer", 2},
    V{"torque.sub.layer_level", 0.2},
    V{"torque.cutoff", 600},
    V{"torque.resonance", 0.1},
    V{"torque.envmod", 0},
    V{"torque.sub.drop", 2},
    V{"torque.amp.attack", 0.01},
    V{"torque.amp.release", 0.35},
    V{"torque.fx.delay", 0.25},
    V{"torque.fx.delay_feedback", 0.5},
    V{"torque.fx.dist_mix", 0},
};

inline constexpr std::array init{
    V{"torque.resonance", 0},   V{"torque.envmod", 0},     V{"torque.cutoff", 12'000},
    V{"torque.drive", 0},       V{"torque.keytrack", 0},   V{"torque.accent", 0},
    V{"torque.fx.dist_mix", 0}, V{"torque.out.level", -6},
};

inline constexpr std::array<SynthPreset, 16> presets{{
    {"Acid Classic", "Acid", {}},
    {"Acid Squelch", "Acid", acid_squelch},
    {"Rubber Acid", "Acid", rubber_acid},
    {"Seq Acid Line", "Acid", seq_acid_line},
    {"Moog Bass", "Moog", moog_bass},
    {"Funk Pluck", "Moog", funk_pluck},
    {"Taurus Pedal", "Moog", taurus_pedal},
    {"Synthwave Bass", "Moog", synthwave_bass},
    {"Reese", "Reese", reese},
    {"Neuro Growl", "Reese", neuro_growl},
    {"Wobble Reese", "Reese", wobble_reese},
    {"Deep Sub", "Sub", deep_sub},
    {"808 Glide", "Sub", glide_808},
    {"Distorted 808", "Sub", distorted_808},
    {"Dub Sub", "Sub", dub_sub},
    {"Init", "Init", init},
}};

} // namespace oscillate::native_dsp::synth::torque_presets
