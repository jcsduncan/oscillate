#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Prism factory presets. Each lists only values that differ from the table
// defaults, which are themselves the "Prism Default" patch. Golden-render
// tests pin every preset's sound; change one deliberately and re-baseline.
namespace oscillate::native_dsp::synth::prism_presets {

using V = std::pair<std::string_view, double>;

inline constexpr std::array init{
    V{"prism.osc2.fine", 0},          V{"prism.osc2.level", 0},       V{"prism.sub.level", 0},
    V{"prism.filter.cutoff", 20'000}, V{"prism.filter.resonance", 0}, V{"prism.filter.drive", 0},
    V{"prism.filter.keytrack", 0},    V{"prism.filter.env", 0},       V{"prism.filter.velocity", 0},
    V{"prism.amp.sustain", 1},        V{"prism.amp.release", 0.05},   V{"prism.fx.chorus", 0},
};

inline constexpr std::array supersaw{
    V{"prism.osc1.unison", 7},         V{"prism.osc1.detune", 28},
    V{"prism.osc1.width", 1},          V{"prism.osc2.unison", 7},
    V{"prism.osc2.detune", 22},        V{"prism.osc2.width", 1},
    V{"prism.osc2.fine", 0},           V{"prism.osc2.octave", 1},
    V{"prism.osc2.level", 0.35},       V{"prism.sub.level", 0.2},
    V{"prism.filter.mode", 0},         V{"prism.filter.cutoff", 9'000},
    V{"prism.filter.resonance", 0.05}, V{"prism.filter.drive", 0},
    V{"prism.filter.env", 0},          V{"prism.filter.keytrack", 0.3},
    V{"prism.amp.attack", 0.005},      V{"prism.amp.decay", 0.8},
    V{"prism.amp.sustain", 0.85},      V{"prism.amp.release", 0.45},
    V{"prism.fx.chorus", 2},           V{"prism.fx.chorus_mix", 0.35},
    V{"prism.out.level", -4},
};

inline constexpr std::array saw_lead{
    V{"prism.voice.mode", 2},        V{"prism.voice.glide", 0.06},      V{"prism.osc2.fine", 9},
    V{"prism.osc2.level", 0.7},      V{"prism.sub.level", 0.15},        V{"prism.filter.mode", 8},
    V{"prism.filter.cutoff", 1'800}, V{"prism.filter.resonance", 0.35}, V{"prism.filter.drive", 6},
    V{"prism.filter.env", 36},       V{"prism.env2.attack", 0.002},     V{"prism.env2.decay", 0.3},
    V{"prism.env2.sustain", 0.45},   V{"prism.amp.attack", 0.002},      V{"prism.amp.sustain", 0.9},
    V{"prism.amp.release", 0.18},    V{"prism.lfo1.wheel", 30},         V{"prism.fx.chorus", 0},
    V{"prism.fx.drive", 4},
};

inline constexpr std::array square_lead{
    V{"prism.voice.mode", 2},        V{"prism.voice.glide", 0.04},     V{"prism.osc1.wave", 1},
    V{"prism.osc2.wave", 1},         V{"prism.osc2.pw", 0.3},          V{"prism.osc2.fine", 5},
    V{"prism.osc2.level", 0.4},      V{"prism.sub.level", 0.1},        V{"prism.filter.mode", 0},
    V{"prism.filter.cutoff", 3'000}, V{"prism.filter.resonance", 0.2}, V{"prism.filter.env", 18},
    V{"prism.amp.sustain", 0.85},    V{"prism.amp.release", 0.15},     V{"prism.lfo1.vibrato", 8},
    V{"prism.lfo1.fade", 0.6},       V{"prism.fx.chorus", 0},
};

inline constexpr std::array reese_bass{
    V{"prism.voice.mode", 1},   V{"prism.osc1.unison", 2},        V{"prism.osc1.detune", 18},
    V{"prism.osc1.width", 0.3}, V{"prism.osc2.unison", 2},        V{"prism.osc2.detune", 22},
    V{"prism.osc2.width", 0.3}, V{"prism.osc2.fine", -11},        V{"prism.osc2.level", 0.75},
    V{"prism.sub.level", 0.55}, V{"prism.filter.cutoff", 420},    V{"prism.filter.resonance", 0.2},
    V{"prism.filter.drive", 8}, V{"prism.filter.keytrack", 0.25}, V{"prism.filter.env", 18},
    V{"prism.env2.decay", 0.3}, V{"prism.env2.sustain", 0.3},     V{"prism.amp.attack", 0.002},
    V{"prism.amp.sustain", 1},  V{"prism.amp.release", 0.12},     V{"prism.fx.chorus", 0},
};

inline constexpr std::array sub_bass{
    V{"prism.voice.mode", 2},      V{"prism.voice.glide", 0.03},  V{"prism.osc1.wave", 3},
    V{"prism.osc1.level", 0.8},    V{"prism.osc2.level", 0},      V{"prism.sub.level", 0.4},
    V{"prism.filter.mode", 0},     V{"prism.filter.cutoff", 600}, V{"prism.filter.resonance", 0},
    V{"prism.filter.env", 0},      V{"prism.filter.keytrack", 0}, V{"prism.filter.drive", 2},
    V{"prism.filter.velocity", 0}, V{"prism.amp.decay", 0.3},     V{"prism.amp.sustain", 1},
    V{"prism.amp.release", 0.1},   V{"prism.fx.chorus", 0},       V{"prism.fx.drive", 3},
};

inline constexpr std::array pluck{
    V{"prism.osc2.wave", 1},        V{"prism.osc2.pw", 0.35},
    V{"prism.osc2.fine", 6},        V{"prism.osc2.level", 0.5},
    V{"prism.filter.cutoff", 380},  V{"prism.filter.resonance", 0.25},
    V{"prism.filter.env", 54},      V{"prism.filter.velocity", 0.4},
    V{"prism.env2.attack", 0.0005}, V{"prism.env2.decay", 0.22},
    V{"prism.env2.sustain", 0},     V{"prism.env2.release", 0.2},
    V{"prism.env2.curve", 0.8},     V{"prism.amp.attack", 0.001},
    V{"prism.amp.decay", 0.5},      V{"prism.amp.sustain", 0},
    V{"prism.amp.release", 0.3},    V{"prism.amp.curve", 0.7},
    V{"prism.fx.chorus_mix", 0.5},
};

inline constexpr std::array soft_pad{
    V{"prism.osc1.unison", 4},   V{"prism.osc1.detune", 14},      V{"prism.osc1.width", 0.8},
    V{"prism.osc2.wave", 2},     V{"prism.osc2.unison", 3},       V{"prism.osc2.detune", 10},
    V{"prism.osc2.octave", 1},   V{"prism.osc2.level", 0.4},      V{"prism.sub.level", 0},
    V{"prism.filter.mode", 0},   V{"prism.filter.cutoff", 1'400}, V{"prism.filter.resonance", 0.1},
    V{"prism.filter.env", 12},   V{"prism.filter.keytrack", 0.4}, V{"prism.env2.attack", 1.2},
    V{"prism.env2.decay", 2},    V{"prism.env2.sustain", 0.6},    V{"prism.env2.release", 2.5},
    V{"prism.amp.attack", 0.9},  V{"prism.amp.decay", 1.5},       V{"prism.amp.sustain", 0.85},
    V{"prism.amp.release", 2.2}, V{"prism.lfo2.rate", 0.2},       V{"prism.lfo2.cutoff", 6},
    V{"prism.fx.chorus", 3},     V{"prism.fx.chorus_mix", 0.5},   V{"prism.out.level", -3},
};

inline constexpr std::array brass{
    V{"prism.osc2.fine", 8},          V{"prism.osc2.level", 0.7},    V{"prism.filter.cutoff", 700},
    V{"prism.filter.resonance", 0.1}, V{"prism.filter.env", 40},     V{"prism.filter.drive", 4},
    V{"prism.filter.velocity", 0.5},  V{"prism.env2.attack", 0.06},  V{"prism.env2.decay", 0.5},
    V{"prism.env2.sustain", 0.55},    V{"prism.env2.release", 0.25}, V{"prism.env2.curve", 0.4},
    V{"prism.amp.attack", 0.03},      V{"prism.amp.decay", 0.4},     V{"prism.amp.sustain", 0.8},
    V{"prism.amp.release", 0.2},      V{"prism.lfo1.vibrato", 6},    V{"prism.lfo1.fade", 0.8},
    V{"prism.fx.chorus_mix", 0.3},
};

inline constexpr std::array strings{
    V{"prism.osc1.unison", 5},       V{"prism.osc1.detune", 18},
    V{"prism.osc1.width", 0.9},      V{"prism.osc2.unison", 3},
    V{"prism.osc2.detune", 10},      V{"prism.osc2.level", 0.4},
    V{"prism.sub.level", 0.1},       V{"prism.filter.mode", 0},
    V{"prism.filter.cutoff", 3'200}, V{"prism.filter.resonance", 0.05},
    V{"prism.filter.keytrack", 0.6}, V{"prism.filter.env", 6},
    V{"prism.amp.attack", 0.35},     V{"prism.amp.decay", 1},
    V{"prism.amp.sustain", 0.9},     V{"prism.amp.release", 0.9},
    V{"prism.lfo1.vibrato", 6},      V{"prism.lfo1.fade", 1},
    V{"prism.lfo1.rate", 5},         V{"prism.fx.chorus", 2},
};

// FM bell: osc 1 is a silent modulator 3.5x above the osc 2 carrier, and
// env 2 shapes the modulation index so the attack is bright and inharmonic.
inline constexpr std::array bell{
    V{"prism.osc1.wave", 3},        V{"prism.osc1.octave", 1},
    V{"prism.osc1.semi", 10},       V{"prism.osc1.fine", -31},
    V{"prism.osc1.level", 0},       V{"prism.osc1.phase", 1},
    V{"prism.osc2.wave", 3},        V{"prism.osc2.fine", 0},
    V{"prism.osc2.level", 0.8},     V{"prism.osc2.phase", 1},
    V{"prism.osc2.fm", 0.2},        V{"prism.sub.level", 0},
    V{"prism.filter.mode", 0},      V{"prism.filter.cutoff", 12'000},
    V{"prism.filter.resonance", 0}, V{"prism.filter.env", 0},
    V{"prism.filter.keytrack", 0},  V{"prism.filter.drive", 0},
    V{"prism.env2.attack", 0.0005}, V{"prism.env2.decay", 1.2},
    V{"prism.env2.sustain", 0},     V{"prism.env2.release", 1},
    V{"prism.amp.attack", 0.001},   V{"prism.amp.decay", 2.5},
    V{"prism.amp.sustain", 0},      V{"prism.amp.release", 2.2},
    V{"prism.amp.curve", 0.8},      V{"prism.mod1.source", 1},
    V{"prism.mod1.target", 6},      V{"prism.mod1.amount", 0.35},
    V{"prism.fx.chorus_mix", 0.4},
};

inline constexpr std::array sync_lead{
    V{"prism.voice.mode", 2},         V{"prism.voice.glide", 0.03}, V{"prism.osc1.level", 0.15},
    V{"prism.osc2.sync", 1},          V{"prism.osc2.octave", 1},    V{"prism.osc2.fine", 0},
    V{"prism.osc2.level", 0.8},       V{"prism.sub.level", 0.15},   V{"prism.mod1.source", 1},
    V{"prism.mod1.target", 3},        V{"prism.mod1.amount", 0.5},  V{"prism.env2.attack", 0.0005},
    V{"prism.env2.decay", 0.6},       V{"prism.env2.sustain", 0.1}, V{"prism.filter.cutoff", 5'000},
    V{"prism.filter.resonance", 0.1}, V{"prism.filter.env", 0},     V{"prism.amp.sustain", 0.9},
    V{"prism.amp.release", 0.2},      V{"prism.fx.chorus", 0},      V{"prism.fx.drive", 5},
};

inline constexpr std::array acid{
    V{"prism.voice.mode", 2},
    V{"prism.voice.glide", 0.07},
    V{"prism.osc2.level", 0},
    V{"prism.sub.level", 0},
    V{"prism.filter.mode", 8},
    V{"prism.filter.cutoff", 350},
    V{"prism.filter.resonance", 0.82},
    V{"prism.filter.env", 60},
    V{"prism.filter.drive", 8},
    V{"prism.filter.velocity", 0.6},
    V{"prism.filter.keytrack", 0.3},
    V{"prism.env2.attack", 0.0005},
    V{"prism.env2.decay", 0.25},
    V{"prism.env2.sustain", 0},
    V{"prism.env2.release", 0.1},
    V{"prism.env2.curve", 0.7},
    V{"prism.amp.attack", 0.001},
    V{"prism.amp.decay", 0.3},
    V{"prism.amp.release", 0.08},
    V{"prism.fx.chorus", 0},
    V{"prism.fx.drive", 6},
};

inline constexpr std::array wobble{
    V{"prism.voice.mode", 1},      V{"prism.osc1.unison", 3},
    V{"prism.osc1.detune", 15},    V{"prism.osc2.wave", 1},
    V{"prism.osc2.octave", -1},    V{"prism.osc2.fine", 0},
    V{"prism.osc2.level", 0.6},    V{"prism.sub.level", 0.5},
    V{"prism.filter.cutoff", 220}, V{"prism.filter.resonance", 0.45},
    V{"prism.filter.drive", 10},   V{"prism.filter.env", 0},
    V{"prism.lfo2.shape", 0},      V{"prism.lfo2.sync", 1},
    V{"prism.lfo2.division", 5},   V{"prism.lfo2.retrigger", 1},
    V{"prism.lfo2.mode", 0},       V{"prism.lfo2.cutoff", 42},
    V{"prism.amp.sustain", 1},     V{"prism.amp.release", 0.1},
    V{"prism.fx.chorus", 0},       V{"prism.fx.drive", 6},
};

inline constexpr std::array juno_pad{
    V{"prism.osc1.wave", 1},          V{"prism.osc2.fine", 0},     V{"prism.osc2.level", 0.5},
    V{"prism.sub.wave", 1},           V{"prism.sub.level", 0.35},  V{"prism.filter.cutoff", 1'600},
    V{"prism.filter.resonance", 0.2}, V{"prism.filter.env", 10},   V{"prism.env2.attack", 0.6},
    V{"prism.amp.attack", 0.4},       V{"prism.amp.decay", 1},     V{"prism.amp.sustain", 0.8},
    V{"prism.amp.release", 1.5},      V{"prism.lfo2.rate", 0.4},   V{"prism.lfo2.pw", 0.3},
    V{"prism.fx.chorus", 3},          V{"prism.fx.chorus_mix", 1},
};

inline constexpr std::array<Preset, 16> presets{{
    {"Init", "Init", init},
    {"Prism Default", "Keys", {}},
    {"Supersaw", "Lead", supersaw},
    {"Saw Lead", "Lead", saw_lead},
    {"Square Lead", "Lead", square_lead},
    {"Reese Bass", "Bass", reese_bass},
    {"Sub Bass", "Bass", sub_bass},
    {"Pluck", "Pluck", pluck},
    {"Soft Pad", "Pad", soft_pad},
    {"Brass", "Brass", brass},
    {"Strings", "Strings", strings},
    {"Bell", "Keys", bell},
    {"Sync Lead", "Lead", sync_lead},
    {"Acid", "Bass", acid},
    {"Wobble", "Bass", wobble},
    {"Juno Chorus Pad", "Pad", juno_pad},
}};

} // namespace oscillate::native_dsp::synth::prism_presets
