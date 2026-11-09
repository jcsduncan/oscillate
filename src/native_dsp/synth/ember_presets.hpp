#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Ember factory presets: only values that differ from the table defaults,
// which are the "Ember Default" warm poly. Golden fingerprints pin each.
namespace oscillate::native_dsp::synth::ember_presets {

using V = std::pair<std::string_view, double>;

inline constexpr std::array init{
    V{"ember.dco.sub", 0},          V{"ember.vco.level", 0},    V{"ember.filter.cutoff", 20'000},
    V{"ember.filter.resonance", 0}, V{"ember.filter.env", 0},   V{"ember.filter.keytrack", 0},
    V{"ember.filter.velocity", 0},  V{"ember.filter.drive", 0}, V{"ember.amp.sustain", 1},
    V{"ember.amp.release", 0.05},   V{"ember.fx.chorus", 0},    V{"ember.fx.hiss", 0},
    V{"ember.fx.tape", 0},          V{"ember.fx.wobble", 0},    V{"ember.age", 0},
    V{"ember.out.level", -6},
};

inline constexpr std::array juno_strings{
    V{"ember.dco.saw", 0.9},         V{"ember.dco.pulse", 0.4},        V{"ember.dco.pwm", 0.55},
    V{"ember.dco.sub", 0},           V{"ember.vco.level", 0},          V{"ember.lfo.rate", 0.6},
    V{"ember.filter.cutoff", 3'200}, V{"ember.filter.resonance", 0.1}, V{"ember.filter.env", 0.1},
    V{"ember.amp.attack", 0.45},     V{"ember.amp.decay", 1},          V{"ember.amp.sustain", 0.9},
    V{"ember.amp.release", 1.1},     V{"ember.fx.chorus", 2},
};

inline constexpr std::array solina{
    V{"ember.dco.saw", 0.85},    V{"ember.dco.sub", 0},           V{"ember.vco.level", 0},
    V{"ember.hpf", 2},           V{"ember.filter.cutoff", 6'000}, V{"ember.filter.resonance", 0},
    V{"ember.filter.env", 0},    V{"ember.amp.attack", 0.25},     V{"ember.amp.sustain", 1},
    V{"ember.amp.release", 1.2}, V{"ember.fx.chorus", 0},         V{"ember.fx.ensemble", 1},
    V{"ember.fx.tape", 0.2},
};

inline constexpr std::array brass{
    V{"ember.dco.saw", 0.9},       V{"ember.vco.level", 0.7},         V{"ember.vco.fine", 9},
    V{"ember.filter.cutoff", 600}, V{"ember.filter.resonance", 0.15}, V{"ember.filter.env", 0.7},
    V{"ember.env.attack", 0.08},   V{"ember.env.decay", 0.5},         V{"ember.env.sustain", 0.55},
    V{"ember.env.release", 0.3},   V{"ember.amp.attack", 0.05},       V{"ember.amp.sustain", 0.85},
    V{"ember.amp.release", 0.25},  V{"ember.lfo.pitch", 0.08},        V{"ember.lfo.delay", 1},
};

inline constexpr std::array poly_keys{
    V{"ember.dco.saw", 0.4},    V{"ember.dco.pulse", 0.6},    V{"ember.dco.pw", 0.3},
    V{"ember.dco.pwm", 0},      V{"ember.dco.pwm_source", 0}, V{"ember.vco.wave", 2},
    V{"ember.vco.level", 0.35}, V{"ember.vco.semi", 12},      V{"ember.filter.cutoff", 2'600},
    V{"ember.filter.env", 0.3}, V{"ember.env.decay", 0.5},    V{"ember.env.sustain", 0.2},
    V{"ember.amp.decay", 1.6},  V{"ember.amp.sustain", 0.3},  V{"ember.amp.release", 0.5},
};

inline constexpr std::array sync_lead{
    V{"ember.dco.sub", 0},           V{"ember.vco.sync", 1},
    V{"ember.vco.level", 0.3},       V{"ember.vco.fine", 0},
    V{"ember.pm.env", 0.7},          V{"ember.pm.freq", 1},
    V{"ember.env.decay", 0.6},       V{"ember.env.sustain", 0.2},
    V{"ember.filter.cutoff", 4'000}, V{"ember.filter.resonance", 0.1},
    V{"ember.filter.env", 0.2},      V{"ember.voice.glide", 0.03},
    V{"ember.fx.chorus", 0},         V{"ember.fx.delay", 0.2},
};

inline constexpr std::array unison_lead{
    V{"ember.voice.mode", 1},         V{"ember.voice.detune", 18},
    V{"ember.dco.saw", 0.9},          V{"ember.vco.level", 0.5},
    V{"ember.vco.fine", 5},           V{"ember.filter.cutoff", 2'400},
    V{"ember.filter.resonance", 0.3}, V{"ember.filter.env", 0.4},
    V{"ember.amp.sustain", 0.9},      V{"ember.amp.release", 0.2},
    V{"ember.voice.glide", 0.05},     V{"ember.fx.chorus", 0},
    V{"ember.fx.delay", 0.25},        V{"ember.fx.delay_feedback", 0.3},
};

inline constexpr std::array bass_pluck{
    V{"ember.dco.saw", 0.9},       V{"ember.dco.sub", 0.6},           V{"ember.vco.level", 0},
    V{"ember.filter.cutoff", 300}, V{"ember.filter.resonance", 0.35}, V{"ember.filter.env", 0.65},
    V{"ember.env.decay", 0.25},    V{"ember.env.sustain", 0},         V{"ember.env.release", 0.15},
    V{"ember.amp.decay", 0.4},     V{"ember.amp.sustain", 0.5},       V{"ember.amp.release", 0.1},
    V{"ember.fx.chorus", 0},       V{"ember.fx.tape", 0.3},           V{"ember.age", 0.2},
};

inline constexpr std::array fat_bass{
    V{"ember.voice.mode", 1},   V{"ember.voice.detune", 10},  V{"ember.dco.range", 0},
    V{"ember.dco.saw", 0.8},    V{"ember.dco.sub", 0.7},      V{"ember.vco.level", 0.6},
    V{"ember.vco.semi", -12},   V{"ember.vco.fine", 4},       V{"ember.filter.cutoff", 500},
    V{"ember.filter.env", 0.4}, V{"ember.env.decay", 0.35},   V{"ember.env.sustain", 0.2},
    V{"ember.amp.sustain", 1},  V{"ember.amp.release", 0.1},  V{"ember.fx.chorus", 0},
    V{"ember.fx.tape", 0.35},   V{"ember.filter.drive", 0.7},
};

inline constexpr std::array sweep_pad{
    V{"ember.dco.pulse", 0.3},
    V{"ember.dco.pwm", 0.4},
    V{"ember.vco.fine", 8},
    V{"ember.vco.level", 0.5},
    V{"ember.filter.type", 1},
    V{"ember.filter.cutoff", 300},
    V{"ember.filter.resonance", 0.45},
    V{"ember.filter.env", 0.55},
    V{"ember.env.attack", 2.5},
    V{"ember.env.decay", 3},
    V{"ember.env.sustain", 0.4},
    V{"ember.env.release", 2.5},
    V{"ember.amp.attack", 1},
    V{"ember.amp.sustain", 0.9},
    V{"ember.amp.release", 2.5},
    V{"ember.fx.chorus", 3},
    V{"ember.lfo.pitch", 0.03},
    V{"ember.age", 0.5},
    V{"ember.fx.delay", 0.2},
    V{"ember.fx.delay_time", 0.5},
    V{"ember.fx.delay_feedback", 0.45},
};

inline constexpr std::array dark_pad{
    V{"ember.dco.saw", 0.5},     V{"ember.dco.pulse", 0.5},     V{"ember.dco.pw", 0.4},
    V{"ember.dco.sub", 0.3},     V{"ember.vco.wave", 2},        V{"ember.vco.level", 0.5},
    V{"ember.vco.semi", -12},    V{"ember.filter.cutoff", 700}, V{"ember.filter.resonance", 0.1},
    V{"ember.filter.env", 0.15}, V{"ember.amp.attack", 1.4},    V{"ember.amp.sustain", 0.85},
    V{"ember.amp.release", 3},   V{"ember.fx.tape", 0.3},       V{"ember.fx.wobble", 0.3},
    V{"ember.age", 0.6},
};

inline constexpr std::array chord_stab{
    V{"ember.voice.mode", 2},          V{"ember.voice.chord", 3},
    V{"ember.dco.saw", 0.8},           V{"ember.dco.pulse", 0.3},
    V{"ember.filter.cutoff", 900},     V{"ember.filter.resonance", 0.3},
    V{"ember.filter.env", 0.6},        V{"ember.env.decay", 0.3},
    V{"ember.env.sustain", 0.1},       V{"ember.amp.decay", 0.5},
    V{"ember.amp.sustain", 0.2},       V{"ember.amp.release", 0.3},
    V{"ember.fx.chorus", 2},           V{"ember.fx.delay", 0.25},
    V{"ember.fx.delay_sync", 1},       V{"ember.fx.delay_division", 6},
    V{"ember.fx.delay_feedback", 0.4},
};

inline constexpr std::array dream_delay{
    V{"ember.dco.saw", 0.3},         V{"ember.dco.pulse", 0.6},
    V{"ember.dco.pwm", 0.5},         V{"ember.vco.wave", 2},
    V{"ember.vco.level", 0.4},       V{"ember.vco.semi", 12},
    V{"ember.filter.cutoff", 2'200}, V{"ember.filter.env", 0.25},
    V{"ember.amp.attack", 0.02},     V{"ember.amp.decay", 1.5},
    V{"ember.amp.sustain", 0.3},     V{"ember.amp.release", 1.2},
    V{"ember.fx.delay", 0.45},       V{"ember.fx.delay_sync", 1},
    V{"ember.fx.delay_division", 8}, V{"ember.fx.delay_feedback", 0.55},
    V{"ember.fx.ensemble", 0.3},     V{"ember.fx.tape", 0.2},
    V{"ember.fx.wobble", 0.25},
};

// Everything the circuit can do wrong: worn cards, an uncalibrated VCO,
// leakage and a little hum from a different unit.
inline constexpr std::array age_max{
    V{"ember.age", 1},
    V{"ember.circuit.tuning", 1},
    V{"ember.circuit.bleed", 1},
    V{"ember.circuit.hum", 0.4},
    V{"ember.circuit.unit", 7},
    V{"ember.voice.mode", 1},
    V{"ember.voice.detune", 35},
    V{"ember.dco.saw", 0.9},
    V{"ember.dco.pulse", 0.5},
    V{"ember.dco.pwm", 0.7},
    V{"ember.lfo.rate", 6},
    V{"ember.vco.level", 0.6},
    V{"ember.vco.semi", -12},
    V{"ember.voice.glide", 0.12},
    V{"ember.filter.cutoff", 1'500},
    V{"ember.filter.resonance", 0.25},
    V{"ember.filter.env", 0.3},
    V{"ember.fx.chorus", 3},
    V{"ember.fx.tape", 0.5},
    V{"ember.fx.wobble", 0.5},
};

inline constexpr std::array tape_keys{
    V{"ember.dco.saw", 0.3},     V{"ember.dco.pulse", 0.5},  V{"ember.dco.pw", 0.2},
    V{"ember.dco.pwm", 0},       V{"ember.vco.wave", 2},     V{"ember.vco.level", 0.4},
    V{"ember.vco.semi", 12},     V{"ember.filter.env", 0.4}, V{"ember.env.decay", 0.6},
    V{"ember.env.sustain", 0.2}, V{"ember.amp.decay", 1.5},  V{"ember.amp.sustain", 0.25},
    V{"ember.amp.release", 0.6}, V{"ember.fx.tape", 0.7},    V{"ember.fx.wobble", 0.6},
    V{"ember.age", 0.5},         V{"ember.fx.hiss", 0.5},
};

// ---- The circuit collection (ADR-0066) ---------------------------------------
// Sixteen patches that lean on the circuit: the steady DCO against the
// wandering VCO, the cards' differences, a little leakage and hum.

// Slow, wide strings: PWM and VCO B beating against the DCO through both
// choruses, a breath of tape.
inline constexpr std::array velvet_strings{
    V{"ember.dco.saw", 0.85},        V{"ember.dco.pulse", 0.35},       V{"ember.dco.pwm", 0.6},
    V{"ember.dco.sub", 0},           V{"ember.vco.level", 0.35},       V{"ember.vco.fine", 9},
    V{"ember.lfo.rate", 0.5},        V{"ember.lfo.delay", 0},          V{"ember.filter.cutoff", 4'200},
    V{"ember.filter.resonance", 0.05}, V{"ember.filter.env", 0.15},    V{"ember.filter.keytrack", 0.4},
    V{"ember.env.attack", 0.9},      V{"ember.env.decay", 2},          V{"ember.env.sustain", 0.8},
    V{"ember.env.release", 1.6},     V{"ember.amp.attack", 0.7},       V{"ember.amp.decay", 2},
    V{"ember.amp.sustain", 0.95},    V{"ember.amp.release", 1.8},      V{"ember.fx.chorus", 3},
    V{"ember.fx.ensemble", 0.35},    V{"ember.fx.delay", 0.12},        V{"ember.fx.delay_time", 0.42},
    V{"ember.fx.delay_feedback", 0.3}, V{"ember.fx.tape", 0.25},       V{"ember.fx.wobble", 0.15},
    V{"ember.age", 0.45},            V{"ember.circuit.tuning", 0.45},
    V{"ember.out.level", 1.5},
};
// The classic two-oscillator poly: a saw on each, a few cents apart, the
// VCO drifting against the crystal so every chord breathes.
inline constexpr std::array prophet_poly{
    V{"ember.dco.saw", 0.7},         V{"ember.dco.sub", 0.15},         V{"ember.vco.level", 0.7},
    V{"ember.vco.fine", 6},          V{"ember.filter.cutoff", 1'400},  V{"ember.filter.resonance", 0.25},
    V{"ember.filter.env", 0.45},     V{"ember.filter.velocity", 0.35}, V{"ember.env.attack", 0.003},
    V{"ember.env.decay", 0.9},       V{"ember.env.sustain", 0.3},      V{"ember.env.release", 0.5},
    V{"ember.amp.decay", 1.4},       V{"ember.amp.sustain", 0.7},      V{"ember.amp.release", 0.6},
    V{"ember.amp.velocity", 0.5},    V{"ember.fx.chorus", 0},          V{"ember.fx.tape", 0.2},
    V{"ember.age", 0.4},             V{"ember.circuit.tuning", 0.6},
};
// A pad that opens like light through a window: the SEM filter rising over
// seconds, a long echo behind it.
inline constexpr std::array sunrise_swell{
    V{"ember.dco.saw", 0.8},         V{"ember.dco.pulse", 0.3},        V{"ember.dco.pwm", 0.5},
    V{"ember.dco.sub", 0.1},         V{"ember.vco.level", 0.5},        V{"ember.vco.fine", -8},
    V{"ember.lfo.rate", 0.3},        V{"ember.lfo.delay", 0},          V{"ember.filter.type", 1},
    V{"ember.filter.morph", 0.15},   V{"ember.filter.cutoff", 500},    V{"ember.filter.resonance", 0.3},
    V{"ember.filter.env", 0.55},     V{"ember.env.attack", 3.5},       V{"ember.env.decay", 4},
    V{"ember.env.sustain", 0.6},     V{"ember.env.release", 2.5},      V{"ember.amp.attack", 1.5},
    V{"ember.amp.decay", 3},         V{"ember.amp.sustain", 0.9},      V{"ember.amp.release", 2.8},
    V{"ember.fx.chorus", 2},         V{"ember.fx.delay", 0.25},        V{"ember.fx.delay_time", 0.5},
    V{"ember.fx.delay_feedback", 0.45}, V{"ember.fx.tape", 0.2},       V{"ember.age", 0.4},
    V{"ember.out.level", -4},
};
// Bells from a narrow pulse and a triangle an octave and a fifth above,
// ringing out through a ping-pong echo.
inline constexpr std::array glass_bells{
    V{"ember.dco.saw", 0},           V{"ember.dco.pulse", 0.6},        V{"ember.dco.pw", 0.75},
    V{"ember.dco.pwm", 0.15},        V{"ember.dco.sub", 0},            V{"ember.vco.wave", 2},
    V{"ember.vco.level", 0.6},       V{"ember.vco.semi", 19},          V{"ember.vco.fine", 4},
    V{"ember.filter.cutoff", 6'000}, V{"ember.filter.resonance", 0.15}, V{"ember.filter.env", 0.2},
    V{"ember.env.attack", 0.001},    V{"ember.env.decay", 1.5},        V{"ember.env.sustain", 0},
    V{"ember.env.release", 1.2},     V{"ember.amp.attack", 0.001},     V{"ember.amp.decay", 2.5},
    V{"ember.amp.sustain", 0},       V{"ember.amp.release", 2},        V{"ember.fx.delay", 0.3},
    V{"ember.fx.delay_time", 0.36},  V{"ember.fx.delay_feedback", 0.45}, V{"ember.fx.tape", 0.1},
    V{"ember.age", 0.25},
    V{"ember.out.level", 2},
};
// Hard sync swept by the filter envelope through poly-mod: the scream.
inline constexpr std::array sync_scream{
    V{"ember.voice.mode", 1},        V{"ember.voice.detune", 10},      V{"ember.voice.glide", 0.06},
    V{"ember.dco.saw", 0.4},         V{"ember.dco.sub", 0},            V{"ember.vco.level", 0.9},
    V{"ember.vco.semi", 7},          V{"ember.vco.fine", 0},           V{"ember.vco.sync", 1},
    V{"ember.pm.env", 0.6},          V{"ember.pm.freq", 1},            V{"ember.filter.cutoff", 3'500},
    V{"ember.filter.resonance", 0.2}, V{"ember.filter.env", 0.3},      V{"ember.env.attack", 0.002},
    V{"ember.env.decay", 0.8},       V{"ember.env.sustain", 0.4},      V{"ember.env.release", 0.3},
    V{"ember.amp.sustain", 0.9},     V{"ember.amp.release", 0.3},      V{"ember.lfo.rate", 5.5},
    V{"ember.lfo.pitch", 0.05},      V{"ember.fx.chorus", 0},          V{"ember.fx.delay", 0.2},
    V{"ember.fx.delay_time", 0.3},   V{"ember.circuit.tuning", 0.4},   V{"ember.out.level", -6},
};
// A singing lead: driven OTA, gentle unison, glide and delayed vibrato.
inline constexpr std::array liquid_lead{
    V{"ember.voice.mode", 1},        V{"ember.voice.detune", 8},       V{"ember.voice.glide", 0.12},
    V{"ember.dco.saw", 0.8},         V{"ember.dco.sub", 0.2},          V{"ember.vco.level", 0.8},
    V{"ember.vco.fine", -5},         V{"ember.filter.cutoff", 1'800},  V{"ember.filter.resonance", 0.45},
    V{"ember.filter.env", 0.35},     V{"ember.filter.drive", 0.5},     V{"ember.env.decay", 1.2},
    V{"ember.env.sustain", 0.5},     V{"ember.amp.sustain", 0.9},      V{"ember.amp.release", 0.35},
    V{"ember.lfo.rate", 5},          V{"ember.lfo.pitch", 0.06},       V{"ember.fx.chorus", 0},
    V{"ember.fx.delay", 0.18},       V{"ember.fx.delay_time", 0.42},   V{"ember.fx.tape", 0.3},
    V{"ember.out.level", -4},
};
// A round, steady sub: the crystal DCO and its square sub, barely filtered.
inline constexpr std::array round_sub{
    V{"ember.dco.range", 0},         V{"ember.dco.saw", 0.4},          V{"ember.dco.sub", 0.9},
    V{"ember.vco.level", 0},         V{"ember.filter.cutoff", 300},    V{"ember.filter.resonance", 0.1},
    V{"ember.filter.env", 0.3},      V{"ember.filter.keytrack", 0.3},  V{"ember.filter.drive", 0.5},
    V{"ember.env.attack", 0.001},    V{"ember.env.decay", 0.35},       V{"ember.env.sustain", 0.2},
    V{"ember.amp.attack", 0.002},    V{"ember.amp.decay", 0.6},        V{"ember.amp.sustain", 0.85},
    V{"ember.amp.release", 0.12},    V{"ember.fx.chorus", 0},          V{"ember.fx.hiss", 0},
    V{"ember.fx.tape", 0.2},         V{"ember.circuit.bleed", 0.1},
    V{"ember.out.level", 2},
};
// A bouncy, resonant bass that snaps shut.
inline constexpr std::array rubber_bass{
    V{"ember.dco.range", 0},         V{"ember.dco.saw", 0.8},          V{"ember.dco.pulse", 0.5},
    V{"ember.dco.pw", 0.3},          V{"ember.dco.pwm", 0},            V{"ember.dco.sub", 0.5},
    V{"ember.vco.level", 0},         V{"ember.filter.cutoff", 220},    V{"ember.filter.resonance", 0.55},
    V{"ember.filter.env", 0.6},      V{"ember.filter.drive", 0.6},     V{"ember.filter.velocity", 0.4},
    V{"ember.env.attack", 0.001},    V{"ember.env.decay", 0.22},       V{"ember.env.sustain", 0.05},
    V{"ember.env.release", 0.1},     V{"ember.amp.decay", 0.5},        V{"ember.amp.sustain", 0.7},
    V{"ember.amp.release", 0.08},    V{"ember.amp.velocity", 0.6},     V{"ember.fx.chorus", 0},
    V{"ember.fx.hiss", 0},
    V{"ember.out.level", 6},
};
// A squelchy, high-resonance line that loves an accent.
inline constexpr std::array acid_squelch{
    V{"ember.dco.saw", 1},           V{"ember.dco.sub", 0},            V{"ember.vco.level", 0},
    V{"ember.filter.cutoff", 400},   V{"ember.filter.resonance", 0.8}, V{"ember.filter.env", 0.7},
    V{"ember.filter.drive", 0.8},    V{"ember.filter.velocity", 0.5},  V{"ember.filter.keytrack", 0.2},
    V{"ember.env.attack", 0.001},    V{"ember.env.decay", 0.18},       V{"ember.env.sustain", 0},
    V{"ember.env.release", 0.08},    V{"ember.amp.mode", 1},           V{"ember.fx.chorus", 0},
    V{"ember.fx.hiss", 0},           V{"ember.fx.delay", 0.15},        V{"ember.fx.delay_time", 0.375},
    V{"ember.out.level", 5},
};
// A glassy pluck for arpeggios, its echoes bouncing left and right.
inline constexpr std::array crystal_arp{
    V{"ember.dco.saw", 0.5},         V{"ember.dco.pulse", 0.5},        V{"ember.dco.pw", 0.4},
    V{"ember.dco.sub", 0},           V{"ember.vco.wave", 2},           V{"ember.vco.level", 0.4},
    V{"ember.vco.semi", 12},         V{"ember.vco.fine", 0},           V{"ember.filter.cutoff", 2'400},
    V{"ember.filter.resonance", 0.3}, V{"ember.filter.env", 0.5},      V{"ember.env.attack", 0.001},
    V{"ember.env.decay", 0.25},      V{"ember.env.sustain", 0},        V{"ember.env.release", 0.2},
    V{"ember.amp.decay", 0.4},       V{"ember.amp.sustain", 0},        V{"ember.amp.release", 0.3},
    V{"ember.fx.delay", 0.35},       V{"ember.fx.delay_time", 0.28},   V{"ember.fx.delay_feedback", 0.5},
    V{"ember.out.level", 6},
};
// A square-and-sub organ through chorus II: on with the key, off with it.
inline constexpr std::array analog_organ{
    V{"ember.dco.saw", 0},           V{"ember.dco.pulse", 0.7},        V{"ember.dco.pw", 0},
    V{"ember.dco.pwm", 0},           V{"ember.dco.sub", 0.6},          V{"ember.vco.wave", 2},
    V{"ember.vco.level", 0.4},       V{"ember.vco.semi", 12},          V{"ember.vco.fine", 0},
    V{"ember.filter.cutoff", 5'000}, V{"ember.filter.resonance", 0},   V{"ember.filter.env", 0},
    V{"ember.filter.keytrack", 0.6}, V{"ember.filter.velocity", 0},    V{"ember.amp.mode", 1},
    V{"ember.fx.chorus", 2},         V{"ember.fx.ensemble", 0.3},      V{"ember.fx.tape", 0.25},
    V{"ember.age", 0.3},
};
// Soft electric-piano keys on an old tape, a little hum in the room.
inline constexpr std::array night_keys{
    V{"ember.dco.saw", 0.2},         V{"ember.dco.pulse", 0.3},        V{"ember.dco.pwm", 0},
    V{"ember.dco.sub", 0.2},         V{"ember.vco.wave", 2},           V{"ember.vco.level", 0.6},
    V{"ember.vco.fine", 3},          V{"ember.filter.cutoff", 1'600},  V{"ember.filter.resonance", 0},
    V{"ember.filter.env", 0.3},      V{"ember.filter.velocity", 0.6},  V{"ember.env.attack", 0.001},
    V{"ember.env.decay", 1.8},       V{"ember.env.sustain", 0.15},     V{"ember.env.release", 0.5},
    V{"ember.amp.attack", 0.001},    V{"ember.amp.decay", 2.5},        V{"ember.amp.sustain", 0.3},
    V{"ember.amp.release", 0.5},     V{"ember.amp.velocity", 0.7},     V{"ember.fx.tape", 0.35},
    V{"ember.fx.wobble", 0.3},       V{"ember.age", 0.4},              V{"ember.circuit.hum", 0.1},
    V{"ember.out.level", 3},
};
// A pad remembered from a worn cassette: everything a little wrong.
inline constexpr std::array cassette_memory{
    V{"ember.dco.saw", 0.6},         V{"ember.dco.pulse", 0.4},        V{"ember.dco.pwm", 0.6},
    V{"ember.lfo.rate", 0.4},        V{"ember.lfo.delay", 0},          V{"ember.filter.cutoff", 1'300},
    V{"ember.filter.env", 0.15},     V{"ember.env.attack", 1},         V{"ember.env.sustain", 0.8},
    V{"ember.env.release", 2},       V{"ember.amp.attack", 0.8},       V{"ember.amp.sustain", 0.9},
    V{"ember.amp.release", 2.5},     V{"ember.fx.chorus", 3},          V{"ember.fx.tape", 0.8},
    V{"ember.fx.wobble", 0.8},       V{"ember.fx.hiss", 0.6},          V{"ember.age", 0.85},
    V{"ember.circuit.tuning", 0.8},  V{"ember.circuit.bleed", 0.6},    V{"ember.circuit.hum", 0.25},
    V{"ember.circuit.unit", 4},
    V{"ember.out.level", 2},
};
// Each key a fifth, slowly breathing, on an old unit left humming.
inline constexpr std::array fifths_drone{
    V{"ember.voice.mode", 2},        V{"ember.voice.chord", 5},        V{"ember.dco.saw", 0.7},
    V{"ember.dco.sub", 0.4},         V{"ember.vco.level", 0.5},        V{"ember.vco.fine", 11},
    V{"ember.filter.cutoff", 900},   V{"ember.filter.resonance", 0.35}, V{"ember.filter.lfo", 0.25},
    V{"ember.filter.env", 0.1},      V{"ember.lfo.rate", 0.12},        V{"ember.lfo.delay", 0},
    V{"ember.env.attack", 2},        V{"ember.env.sustain", 1},        V{"ember.env.release", 3},
    V{"ember.amp.attack", 2.5},      V{"ember.amp.sustain", 1},        V{"ember.amp.release", 4},
    V{"ember.fx.chorus", 2},         V{"ember.fx.delay", 0.3},         V{"ember.fx.delay_time", 0.75},
    V{"ember.fx.delay_feedback", 0.55}, V{"ember.age", 0.6},           V{"ember.circuit.tuning", 0.7},
    V{"ember.circuit.bleed", 0.8},   V{"ember.circuit.hum", 0.35},     V{"ember.circuit.unit", 9},
    V{"ember.out.level", 0},
};
// Early-eighties brass: two saws, a quick filter swell, a short fall.
inline constexpr std::array brass_82{
    V{"ember.dco.saw", 0.9},         V{"ember.dco.sub", 0.1},          V{"ember.vco.level", 0.7},
    V{"ember.vco.fine", 7},          V{"ember.filter.cutoff", 900},    V{"ember.filter.resonance", 0.15},
    V{"ember.filter.env", 0.6},      V{"ember.filter.velocity", 0.4},  V{"ember.env.attack", 0.06},
    V{"ember.env.decay", 0.5},       V{"ember.env.sustain", 0.5},      V{"ember.env.release", 0.25},
    V{"ember.amp.attack", 0.02},     V{"ember.amp.decay", 0.8},        V{"ember.amp.sustain", 0.8},
    V{"ember.amp.release", 0.25},    V{"ember.age", 0.4},              V{"ember.circuit.tuning", 0.5},
};
// Silk: PWM and a soft octave triangle in the ensemble, an echo behind.
inline constexpr std::array silk_pad{
    V{"ember.dco.saw", 0.6},         V{"ember.dco.pulse", 0.5},        V{"ember.dco.pwm", 0.7},
    V{"ember.dco.sub", 0},           V{"ember.lfo.rate", 0.35},        V{"ember.lfo.delay", 0},
    V{"ember.vco.wave", 2},          V{"ember.vco.level", 0.3},        V{"ember.vco.semi", 12},
    V{"ember.vco.fine", 0},          V{"ember.filter.cutoff", 2'200},  V{"ember.filter.resonance", 0.1},
    V{"ember.filter.env", 0.1},      V{"ember.env.attack", 1.2},       V{"ember.env.sustain", 0.85},
    V{"ember.env.release", 2},       V{"ember.amp.attack", 1.2},       V{"ember.amp.decay", 2},
    V{"ember.amp.sustain", 0.85},    V{"ember.amp.release", 2.2},      V{"ember.fx.ensemble", 0.6},
    V{"ember.fx.chorus", 2},         V{"ember.fx.delay", 0.18},        V{"ember.fx.delay_time", 0.55},
    V{"ember.fx.delay_feedback", 0.4},
    V{"ember.out.level", 3},
};

inline constexpr std::array<Preset, 32> presets{{
    {"Init", "Init", init},
    {"Ember Default", "Keys", {}},
    {"Juno Strings", "Strings", juno_strings},
    {"Solina Ensemble", "Strings", solina},
    {"Brass Section", "Brass", brass},
    {"Poly Keys", "Keys", poly_keys},
    {"Sync Lead", "Lead", sync_lead},
    {"Unison Lead", "Lead", unison_lead},
    {"Bass Pluck", "Bass", bass_pluck},
    {"Fat Unison Bass", "Bass", fat_bass},
    {"Sweep Pad", "Pad", sweep_pad},
    {"Dark Pad", "Pad", dark_pad},
    {"Chord Stab", "Keys", chord_stab},
    {"Dream Delay", "Pad", dream_delay},
    {"Age Max", "Circuit", age_max},
    {"Tape Keys", "Keys", tape_keys},
    {"Velvet Strings", "Strings", velvet_strings},
    {"Prophet Poly", "Keys", prophet_poly},
    {"Sunrise Swell", "Pad", sunrise_swell},
    {"Glass Bells", "Keys", glass_bells},
    {"Sync Scream", "Lead", sync_scream},
    {"Liquid Lead", "Lead", liquid_lead},
    {"Round Sub", "Bass", round_sub},
    {"Rubber Bass", "Bass", rubber_bass},
    {"Acid Squelch", "Bass", acid_squelch},
    {"Crystal Arp", "Pluck", crystal_arp},
    {"Analog Organ", "Keys", analog_organ},
    {"Night Keys", "Keys", night_keys},
    {"Cassette Memory", "Circuit", cassette_memory},
    {"Fifths Drone", "Circuit", fifths_drone},
    {"Brass '82", "Brass", brass_82},
    {"Silk Pad", "Pad", silk_pad},
}};

} // namespace oscillate::native_dsp::synth::ember_presets
// clang-format on
