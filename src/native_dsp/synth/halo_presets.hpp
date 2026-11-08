#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Halo factory presets: original patches, only values that differ from the
// table defaults (the "E-Piano"). Algorithm values are 0-based (choice 4 is
// algorithm 5); ratio choice n is ratio n, and choice 0 is 0.5. Golden
// fingerprints pin each.
namespace oscillate::native_dsp::synth::halo_presets {

using V = std::pair<std::string_view, double>;

inline constexpr std::array bright_ep{
    V{"halo.op2.level", 0.3}, V{"halo.op4.level", 0.09}, V{"halo.op6.ratio", 3},
    V{"halo.op6.level", 0.2}, V{"halo.feedback", 0.35},  V{"halo.fx.chorus", 2},
    V{"halo.fx.delay", 0.15}, V{"halo.op1.r2", 36},      V{"halo.op1.r3", 36},
    V{"halo.op2.r2", 44},     V{"halo.op2.r3", 44},
};

inline constexpr std::array bells{
    V{"halo.op2.ratio", 3},   V{"halo.op2.fine", 0.1667}, V{"halo.op2.level", 0.25},
    V{"halo.op3.ratio", 2},   V{"halo.op3.level", 0.4},   V{"halo.op4.ratio", 7},
    V{"halo.op4.fine", 0.05}, V{"halo.op4.level", 0.15},  V{"halo.op5.level", 0},
    V{"halo.op6.level", 0},   V{"halo.feedback", 0},      V{"halo.fx.delay", 0.2},
    V{"halo.fx.chorus", 0},   V{"halo.fx.ensemble", 0.3}, V{"halo.op1.r2", 29},
    V{"halo.op1.r3", 29},     V{"halo.op1.r4", 35},       V{"halo.op2.r2", 33},
    V{"halo.op2.r3", 33},     V{"halo.op2.r4", 40},       V{"halo.op3.r2", 32},
    V{"halo.op3.r3", 32},     V{"halo.op3.r4", 38},       V{"halo.op4.r2", 40},
    V{"halo.op4.r3", 40},     V{"halo.op4.r4", 41},
};

inline constexpr std::array tubular{
    V{"halo.op2.ratio", 3},    V{"halo.op2.fine", 0.4}, V{"halo.op2.level", 0.22},
    V{"halo.op3.detune", 4},   V{"halo.op4.ratio", 5},  V{"halo.op4.fine", 0.3},
    V{"halo.op4.level", 0.12}, V{"halo.op5.level", 0},  V{"halo.op6.level", 0},
    V{"halo.feedback", 0},     V{"halo.fx.chorus", 0},  V{"halo.fx.delay", 0.12},
    V{"halo.op1.r2", 27},      V{"halo.op1.r3", 27},    V{"halo.op1.r4", 35},
    V{"halo.op2.r2", 32},      V{"halo.op2.r3", 32},    V{"halo.op2.r4", 40},
    V{"halo.op3.r2", 29},      V{"halo.op3.r3", 29},    V{"halo.op3.r4", 35},
    V{"halo.op4.r2", 35},      V{"halo.op4.r3", 35},    V{"halo.op4.r4", 40},
};

inline constexpr std::array marimba{
    V{"halo.op2.ratio", 4},   V{"halo.op2.level", 0.25}, V{"halo.op3.ratio", 4},
    V{"halo.op3.level", 0.2}, V{"halo.op3.detune", 0},   V{"halo.op4.ratio", 10},
    V{"halo.op4.level", 0.1}, V{"halo.op5.level", 0},    V{"halo.op6.level", 0},
    V{"halo.feedback", 0},    V{"halo.fx.chorus", 0},    V{"halo.op1.r2", 50},
    V{"halo.op1.r3", 50},     V{"halo.op1.r4", 57},      V{"halo.op2.r2", 68},
    V{"halo.op2.r3", 68},     V{"halo.op2.r4", 72},      V{"halo.op3.r2", 57},
    V{"halo.op3.r3", 57},     V{"halo.op3.r4", 60},      V{"halo.op4.r2", 72},
    V{"halo.op4.r3", 72},
};

inline constexpr std::array fm_bass{
    V{"halo.algorithm", 1},    V{"halo.op1.ratio", 0},   V{"halo.op1.level", 0.9},
    V{"halo.op2.level", 0.28}, V{"halo.op3.level", 0.5}, V{"halo.op3.detune", 0},
    V{"halo.op4.ratio", 1},    V{"halo.op4.level", 0.2}, V{"halo.op5.level", 0},
    V{"halo.op6.level", 0},    V{"halo.feedback", 0.4},  V{"halo.fx.chorus", 0},
    V{"halo.op1.r2", 33},      V{"halo.op1.r3", 33},     V{"halo.op1.r4", 66},
    V{"halo.op1.l2", 90},      V{"halo.op1.l3", 90},     V{"halo.op2.r2", 55},
    V{"halo.op2.r3", 55},      V{"halo.op2.r4", 66},     V{"halo.op2.l2", 76},
    V{"halo.op2.l3", 76},      V{"halo.op3.r2", 40},     V{"halo.op3.r3", 40},
    V{"halo.op3.r4", 66},      V{"halo.op3.l2", 88},     V{"halo.op3.l3", 88},
    V{"halo.op4.r2", 50},      V{"halo.op4.r3", 50},     V{"halo.op4.l2", 80},
    V{"halo.op4.l3", 80},
};

inline constexpr std::array slap_bass{
    V{"halo.op1.ratio", 0},    V{"halo.op1.level", 0.9},  V{"halo.op2.ratio", 0},
    V{"halo.op2.level", 0.35}, V{"halo.op2.velocity", 1}, V{"halo.op3.level", 0.4},
    V{"halo.op3.detune", 5},   V{"halo.op4.ratio", 3},    V{"halo.op4.level", 0.25},
    V{"halo.op5.ratio", 7},    V{"halo.op5.level", 0.2},  V{"halo.op6.level", 0},
    V{"halo.feedback", 0},     V{"halo.fx.chorus", 0},    V{"halo.op1.r2", 43},
    V{"halo.op1.r3", 43},      V{"halo.op1.r4", 68},      V{"halo.op1.l2", 80},
    V{"halo.op1.l3", 80},      V{"halo.op2.r2", 64},      V{"halo.op2.r3", 64},
    V{"halo.op2.l2", 64},      V{"halo.op2.l3", 64},      V{"halo.op3.r2", 50},
    V{"halo.op3.r3", 50},      V{"halo.op3.r4", 68},      V{"halo.op3.l2", 72},
    V{"halo.op3.l3", 72},      V{"halo.op4.r2", 71},      V{"halo.op4.r3", 71},
    V{"halo.op5.r2", 74},      V{"halo.op5.r3", 74},      V{"halo.op5.r4", 74},
};

inline constexpr std::array brass{
    V{"halo.algorithm", 21},   V{"halo.op1.level", 0.7}, V{"halo.op2.level", 0.3},
    V{"halo.op3.detune", 4},   V{"halo.op3.level", 0.5}, V{"halo.op4.ratio", 1},
    V{"halo.op4.detune", -4},  V{"halo.op4.level", 0.5}, V{"halo.op5.detune", 0},
    V{"halo.op6.level", 0.22}, V{"halo.feedback", 0.3},  V{"halo.lfo.pitch", 0.05},
    V{"halo.lfo.delay", 0.8},  V{"halo.fx.chorus", 0},   V{"halo.fx.ensemble", 0.25},
    V{"halo.op1.r1", 68},      V{"halo.op1.r2", 27},     V{"halo.op1.r3", 27},
    V{"halo.op1.r4", 58},      V{"halo.op1.l2", 96},     V{"halo.op1.l3", 96},
    V{"halo.op2.r1", 64},      V{"halo.op2.r2", 35},     V{"halo.op2.r3", 35},
    V{"halo.op2.r4", 58},      V{"halo.op2.l2", 94},     V{"halo.op2.l3", 94},
    V{"halo.op3.r1", 66},      V{"halo.op3.r2", 15},     V{"halo.op3.r3", 15},
    V{"halo.op3.r4", 58},      V{"halo.op3.l2", 96},     V{"halo.op3.l3", 96},
    V{"halo.op4.r1", 66},      V{"halo.op4.r2", 27},     V{"halo.op4.r3", 27},
    V{"halo.op4.r4", 58},      V{"halo.op4.l2", 96},     V{"halo.op4.l3", 96},
    V{"halo.op5.r1", 66},      V{"halo.op5.r2", 15},     V{"halo.op5.r3", 15},
    V{"halo.op5.r4", 58},      V{"halo.op5.l2", 96},     V{"halo.op5.l3", 96},
    V{"halo.op6.r1", 63},      V{"halo.op6.r2", 38},     V{"halo.op6.r3", 38},
    V{"halo.op6.r4", 58},      V{"halo.op6.l2", 92},     V{"halo.op6.l3", 92},
};

inline constexpr std::array strings{
    V{"halo.algorithm", 0},     V{"halo.op1.level", 0.6},   V{"halo.op2.level", 0.18},
    V{"halo.op3.detune", 6},    V{"halo.op3.level", 0.55},  V{"halo.op4.ratio", 1},
    V{"halo.op4.level", 0.2},   V{"halo.op5.ratio", 2},     V{"halo.op5.detune", 0},
    V{"halo.op5.level", 0.1},   V{"halo.op6.level", 0.08},  V{"halo.feedback", 0.45},
    V{"halo.voice.unison", 2},  V{"halo.voice.detune", 14}, V{"halo.fx.chorus", 2},
    V{"halo.fx.ensemble", 0.6}, V{"halo.out.level", -3},    V{"halo.op1.r1", 47},
    V{"halo.op1.r2", 11},       V{"halo.op1.r3", 11},       V{"halo.op1.r4", 44},
    V{"halo.op1.l2", 96},       V{"halo.op1.l3", 96},       V{"halo.op2.r1", 46},
    V{"halo.op2.r2", 18},       V{"halo.op2.r3", 18},       V{"halo.op2.r4", 44},
    V{"halo.op2.l2", 96},       V{"halo.op2.l3", 96},       V{"halo.op3.r1", 49},
    V{"halo.op3.r2", 15},       V{"halo.op3.r3", 15},       V{"halo.op3.r4", 44},
    V{"halo.op3.l2", 96},       V{"halo.op3.l3", 96},       V{"halo.op4.r1", 47},
    V{"halo.op4.r2", 38},       V{"halo.op4.r3", 38},       V{"halo.op4.r4", 44},
    V{"halo.op4.l2", 96},       V{"halo.op4.l3", 96},       V{"halo.op5.r2", 15},
    V{"halo.op5.r3", 15},       V{"halo.op5.r4", 44},       V{"halo.op5.l2", 96},
    V{"halo.op5.l3", 96},       V{"halo.op6.r2", 21},       V{"halo.op6.r3", 21},
    V{"halo.op6.r4", 44},       V{"halo.op6.l2", 96},       V{"halo.op6.l3", 96},
};

inline constexpr std::array organ{
    V{"halo.algorithm", 31},   V{"halo.op1.ratio", 0},    V{"halo.op1.level", 0.5},
    V{"halo.op2.level", 0.5},  V{"halo.op3.ratio", 2},    V{"halo.op3.detune", 0},
    V{"halo.op3.level", 0.4},  V{"halo.op4.ratio", 3},    V{"halo.op4.level", 0.3},
    V{"halo.op5.ratio", 4},    V{"halo.op5.detune", 0},   V{"halo.op5.level", 0.25},
    V{"halo.op6.ratio", 8},    V{"halo.op6.level", 0.2},  V{"halo.feedback", 0.1},
    V{"halo.op1.velocity", 0}, V{"halo.op2.velocity", 0}, V{"halo.op3.velocity", 0},
    V{"halo.op4.velocity", 0}, V{"halo.op5.velocity", 0}, V{"halo.op6.velocity", 0},
    V{"halo.op4.key", 0},      V{"halo.op6.key", 0},      V{"halo.fx.chorus", 3},
    V{"halo.lfo.amp", 0.1},    V{"halo.lfo.rate", 6.5},   V{"halo.lfo.delay", 0},
    V{"halo.lfo.shape", 0},    V{"halo.out.level", -6},   V{"halo.op1.r1", 89},
    V{"halo.op1.r2", 2},       V{"halo.op1.r3", 2},       V{"halo.op1.r4", 68},
    V{"halo.op1.l2", 98},      V{"halo.op1.l3", 98},      V{"halo.op2.r1", 89},
    V{"halo.op2.r2", 8},       V{"halo.op2.r3", 8},       V{"halo.op2.r4", 68},
    V{"halo.op2.l2", 98},      V{"halo.op2.l3", 98},      V{"halo.op3.r1", 89},
    V{"halo.op3.r2", 4},       V{"halo.op3.r3", 4},       V{"halo.op3.r4", 68},
    V{"halo.op3.l2", 98},      V{"halo.op3.l3", 98},      V{"halo.op4.r1", 89},
    V{"halo.op4.r2", 29},      V{"halo.op4.r3", 29},      V{"halo.op4.r4", 68},
    V{"halo.op4.l2", 98},      V{"halo.op4.l3", 98},      V{"halo.op5.r1", 89},
    V{"halo.op5.r2", 5},       V{"halo.op5.r3", 5},       V{"halo.op5.r4", 68},
    V{"halo.op5.l2", 98},      V{"halo.op5.l3", 98},      V{"halo.op6.r1", 89},
    V{"halo.op6.r2", 11},      V{"halo.op6.r3", 11},      V{"halo.op6.r4", 68},
    V{"halo.op6.l2", 98},      V{"halo.op6.l3", 98},
};

inline constexpr std::array clav{
    V{"halo.op2.level", 0.35},    V{"halo.op2.wave", 2},
    V{"halo.op3.ratio", 3},       V{"halo.op3.level", 0.3},
    V{"halo.op3.detune", 0},      V{"halo.op4.ratio", 5},
    V{"halo.op4.level", 0.2},     V{"halo.op5.level", 0},
    V{"halo.op6.level", 0},       V{"halo.feedback", 0},
    V{"halo.fx.chorus", 0},       V{"halo.filter.type", 4},
    V{"halo.filter.cutoff", 180}, V{"halo.filter.resonance", 0.2},
    V{"halo.filter.keytrack", 0}, V{"halo.op1.r2", 41},
    V{"halo.op1.r3", 41},         V{"halo.op1.r4", 71},
    V{"halo.op1.l2", 84},         V{"halo.op1.l3", 84},
    V{"halo.op2.r2", 49},         V{"halo.op2.r3", 49},
    V{"halo.op2.r4", 71},         V{"halo.op2.l2", 80},
    V{"halo.op2.l3", 80},         V{"halo.op3.r2", 52},
    V{"halo.op3.r3", 52},         V{"halo.op3.r4", 71},
    V{"halo.op3.l2", 72},         V{"halo.op3.l3", 72},
    V{"halo.op4.r2", 60},         V{"halo.op4.r3", 60},
};

inline constexpr std::array harp{
    V{"halo.op2.ratio", 2},    V{"halo.op2.level", 0.15}, V{"halo.op3.ratio", 2},
    V{"halo.op3.level", 0.25}, V{"halo.op3.detune", 0},   V{"halo.op4.ratio", 1},
    V{"halo.op4.level", 0.1},  V{"halo.op5.level", 0},    V{"halo.op6.level", 0},
    V{"halo.feedback", 0},     V{"halo.fx.chorus", 0},    V{"halo.fx.delay", 0.18},
    V{"halo.op1.r2", 38},      V{"halo.op1.r3", 38},      V{"halo.op1.r4", 40},
    V{"halo.op2.r2", 54},      V{"halo.op2.r3", 54},      V{"halo.op3.r2", 41},
    V{"halo.op3.r3", 41},      V{"halo.op3.r4", 41},      V{"halo.op4.r2", 60},
    V{"halo.op4.r3", 60},
};

inline constexpr std::array glass_pad{
    V{"halo.op1.level", 0.6},       V{"halo.op2.ratio", 7},    V{"halo.op2.level", 0.08},
    V{"halo.op3.ratio", 2},         V{"halo.op3.detune", 5},   V{"halo.op3.level", 0.4},
    V{"halo.op4.ratio", 5},         V{"halo.op4.level", 0.06}, V{"halo.op5.level", 0},
    V{"halo.op6.level", 0},         V{"halo.feedback", 0},     V{"halo.filter.type", 2},
    V{"halo.filter.cutoff", 6'000}, V{"halo.voice.unison", 2}, V{"halo.voice.detune", 9},
    V{"halo.fx.chorus", 3},         V{"halo.fx.delay", 0.25},  V{"halo.out.level", -3},
    V{"halo.op1.r1", 40},           V{"halo.op1.r2", 11},      V{"halo.op1.r3", 11},
    V{"halo.op1.r4", 40},           V{"halo.op1.l2", 96},      V{"halo.op1.l3", 96},
    V{"halo.op2.r1", 38},           V{"halo.op2.r2", 29},      V{"halo.op2.r3", 29},
    V{"halo.op2.r4", 40},           V{"halo.op2.l2", 92},      V{"halo.op2.l3", 92},
    V{"halo.op3.r1", 40},           V{"halo.op3.r2", 15},      V{"halo.op3.r3", 15},
    V{"halo.op3.r4", 40},           V{"halo.op3.l2", 96},      V{"halo.op3.l3", 96},
    V{"halo.op4.r1", 41},           V{"halo.op4.r2", 44},      V{"halo.op4.r3", 44},
    V{"halo.op4.r4", 40},           V{"halo.op4.l2", 94},      V{"halo.op4.l3", 94},
};

inline constexpr std::array metallic_lead{
    V{"halo.algorithm", 0},   V{"halo.op1.level", 0.8},       V{"halo.op2.ratio", 2},
    V{"halo.op2.fine", 0.41}, V{"halo.op2.level", 0.25},      V{"halo.op3.detune", 7},
    V{"halo.op3.level", 0.5}, V{"halo.op4.ratio", 3},         V{"halo.op4.level", 0.2},
    V{"halo.op5.detune", 0},  V{"halo.op5.level", 0.15},      V{"halo.op6.level", 0.1},
    V{"halo.feedback", 0.5},  V{"halo.voice.mode", 2},        V{"halo.voice.glide", 0.06},
    V{"halo.filter.type", 5}, V{"halo.filter.cutoff", 5'000}, V{"halo.filter.resonance", 0.3},
    V{"halo.lfo.pitch", 0.1}, V{"halo.lfo.delay", 0.4},       V{"halo.fx.chorus", 0},
    V{"halo.fx.delay", 0.2},  V{"halo.out.level", -4},        V{"halo.op1.r1", 83},
    V{"halo.op1.r2", 11},     V{"halo.op1.r3", 11},           V{"halo.op1.r4", 60},
    V{"halo.op1.l2", 96},     V{"halo.op1.l3", 96},           V{"halo.op2.r2", 24},
    V{"halo.op2.r3", 24},     V{"halo.op2.r4", 60},           V{"halo.op2.l2", 94},
    V{"halo.op2.l3", 94},     V{"halo.op3.r2", 15},           V{"halo.op3.r3", 15},
    V{"halo.op3.r4", 60},     V{"halo.op3.l2", 96},           V{"halo.op3.l3", 96},
    V{"halo.op4.r2", 44},     V{"halo.op4.r3", 44},           V{"halo.op4.l2", 94},
    V{"halo.op4.l3", 94},     V{"halo.op5.r2", 21},           V{"halo.op5.r3", 21},
    V{"halo.op5.l2", 94},     V{"halo.op5.l3", 94},           V{"halo.op6.r2", 27},
    V{"halo.op6.r3", 27},     V{"halo.op6.l2", 94},           V{"halo.op6.l3", 94},
};

inline constexpr std::array sitar{
    V{"halo.op2.level", 0.3}, V{"halo.op2.wave", 1},   V{"halo.op3.ratio", 3},
    V{"halo.op3.level", 0.2}, V{"halo.op3.detune", 0}, V{"halo.op4.ratio", 1},
    V{"halo.op4.level", 0.4}, V{"halo.op4.wave", 1},   V{"halo.op5.ratio", 2},
    V{"halo.op5.level", 0.2}, V{"halo.op5.detune", 0}, V{"halo.op6.level", 0.3},
    V{"halo.feedback", 0.6},  V{"halo.fx.chorus", 0},  V{"halo.fx.delay", 0.12},
    V{"halo.out.level", -3},  V{"halo.op1.r2", 38},    V{"halo.op1.r3", 38},
    V{"halo.op1.r4", 50},     V{"halo.op1.l2", 72},    V{"halo.op1.l3", 72},
    V{"halo.op2.r2", 40},     V{"halo.op2.r3", 40},    V{"halo.op2.l2", 72},
    V{"halo.op2.l3", 72},     V{"halo.op3.r2", 41},    V{"halo.op3.r3", 41},
    V{"halo.op4.r2", 41},     V{"halo.op4.r3", 41},    V{"halo.op5.r2", 40},
    V{"halo.op5.r3", 40},
};

inline constexpr std::array init{
    V{"halo.algorithm", 0}, V{"halo.op1.level", 0.8}, V{"halo.op1.velocity", 0},
    V{"halo.op2.level", 0}, V{"halo.op3.level", 0},   V{"halo.op4.level", 0},
    V{"halo.op5.level", 0}, V{"halo.op6.level", 0},   V{"halo.feedback", 0},
    V{"halo.fx.chorus", 0}, V{"halo.op3.detune", 0},  V{"halo.op5.detune", 0},
    V{"halo.op4.ratio", 1}, V{"halo.op2.key", 0},     V{"halo.op4.key", 0},
    V{"halo.op6.key", 0},   V{"halo.op1.r2", 24},     V{"halo.op1.r3", 24},
    V{"halo.op1.r4", 66},   V{"halo.op1.l2", 98},     V{"halo.op1.l3", 98},
};

inline constexpr std::array<Preset, 16> presets{{
    {"E-Piano", "Keys", {}},
    {"Bright EP", "Keys", bright_ep},
    {"Bells", "Mallets", bells},
    {"Tubular", "Mallets", tubular},
    {"Marimba", "Mallets", marimba},
    {"FM Bass", "Bass", fm_bass},
    {"Slap Bass", "Bass", slap_bass},
    {"Brass", "Ensemble", brass},
    {"Strings", "Ensemble", strings},
    {"Organ", "Keys", organ},
    {"Clav", "Keys", clav},
    {"Harp", "Plucked", harp},
    {"Glass Pad", "Pad", glass_pad},
    {"Metallic Lead", "Lead", metallic_lead},
    {"Sitar-ish", "Plucked", sitar},
    {"Init", "Init", init},
}};

} // namespace oscillate::native_dsp::synth::halo_presets
