#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/tempo_sync.hpp"

#include <array>
#include <cstddef>
#include <string_view>

// Torque v1 parameter table: shared controls (stable across models), each
// model's extras, LFO, effects, then the sequencer and its sixteen steps.
// Defaults are the "Acid Classic" preset. Ids are persisted and automated:
// never rename or reuse one.
namespace oscillate::native_dsp::synth::torque_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::size_t step_count = 16;
inline constexpr std::size_t step_parameters = 5;

inline constexpr std::array models{"Acid"sv, "Moog"sv, "Reese"sv, "Sub"sv};
inline constexpr std::array acid_waves{"Saw"sv, "Square"sv};
inline constexpr std::array moog_waves{"Saw"sv, "Triangle"sv, "Square"sv};
inline constexpr std::array reese_filters{"LP 24"sv, "BP 12"sv};
inline constexpr std::array sub_layers{"Off"sv, "Fifth"sv, "Octave"sv};
inline constexpr std::array lfo_shapes{"Sine"sv, "Triangle"sv, "Saw"sv, "Square"sv, "S&H"sv};
inline constexpr std::array off_on{"Off"sv, "On"sv};
// Tempo-synced divisions are shared by every engine (tempo_sync.hpp).
inline constexpr const auto& divisions = sync_divisions;
inline constexpr std::array distortion_modes{"Tube"sv, "Fuzz"sv, "Crush"sv};
inline constexpr std::array chorus_modes{"Off"sv, "I"sv, "II"sv, "I+II"sv};
inline constexpr std::array seq_rates{"1/8"sv, "1/16"sv, "1/16T"sv, "1/32"sv};
inline constexpr std::array<double, 4> seq_steps_per_beat{2.0, 4.0, 6.0, 8.0};
inline constexpr std::array step_gates{"Off"sv, "On"sv, "Tie"sv};

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = 0.02;

// One sequencer step: note (-12..12 st), octave, gate (off/on/tie), accent, slide.
#define TORQUE_STEP(N, NOTE, OCTAVE, GATE, ACCENT, SLIDE)                                          \
  {"torque.seq.s" N ".note", "Note", "Step " N, "st", S::stepped, -12, 12, NOTE, {}, T::live, 0},  \
      {"torque.seq.s" N ".octave",                                                                 \
       "Octave",                                                                                   \
       "Step " N,                                                                                  \
       "",                                                                                         \
       S::stepped,                                                                                 \
       -1,                                                                                         \
       1,                                                                                          \
       OCTAVE,                                                                                     \
       {},                                                                                         \
       T::live,                                                                                    \
       0},                                                                                         \
      {"torque.seq.s" N ".gate",                                                                   \
       "Gate",                                                                                     \
       "Step " N,                                                                                  \
       "",                                                                                         \
       S::choice,                                                                                  \
       0,                                                                                          \
       2,                                                                                          \
       GATE,                                                                                       \
       step_gates,                                                                                 \
       T::live,                                                                                    \
       0},                                                                                         \
      {"torque.seq.s" N ".accent",                                                                 \
       "Accent",                                                                                   \
       "Step " N,                                                                                  \
       "",                                                                                         \
       S::choice,                                                                                  \
       0,                                                                                          \
       1,                                                                                          \
       ACCENT,                                                                                     \
       off_on,                                                                                     \
       T::live,                                                                                    \
       0},                                                                                         \
      {"torque.seq.s" N ".slide",                                                                  \
       "Slide",                                                                                    \
       "Step " N,                                                                                  \
       "",                                                                                         \
       S::choice,                                                                                  \
       0,                                                                                          \
       1,                                                                                          \
       SLIDE,                                                                                      \
       off_on,                                                                                     \
       T::live,                                                                                    \
       0}

inline constexpr std::array<ParameterDescriptor, 65 + step_count * step_parameters> parameters{{
    // Model and shared voice controls (stable ids across models).
    {"torque.model", "Model", "Voice", "", S::choice, 0, last(models.size()), 0, models, T::live,
     0},
    {"torque.tune", "Tune", "Voice", "st", S::stepped, -24, 24, 0, {}, T::live, 0},
    {"torque.fine", "Fine", "Voice", "ct", S::linear, -50, 50, 0, {}, T::live, g},
    {"torque.cutoff", "Cutoff", "Filter", "Hz", S::logarithmic, 30, 12'000, 420, {}, T::live, g},
    {"torque.resonance", "Resonance", "Filter", "%", S::linear, 0, 1, 0.72, {}, T::live, g},
    {"torque.envmod", "Env Mod", "Filter", "%", S::linear, 0, 1, 0.6, {}, T::live, g},
    {"torque.decay", "Decay", "Filter", "s", S::logarithmic, 0.02, 4, 0.3, {}, T::live},
    {"torque.env.attack", "Attack", "Filter", "s", S::logarithmic, 0.001, 2, 0.003, {}, T::live},
    {"torque.env.sustain", "Sustain", "Filter", "%", S::linear, 0, 1, 0, {}, T::live},
    {"torque.keytrack", "Key", "Filter", "%", S::linear, 0, 1, 0.2, {}, T::live, g},
    {"torque.accent", "Accent", "Voice", "%", S::linear, 0, 1, 0.6, {}, T::live, g},
    {"torque.accent_threshold", "Accent At", "Voice", "", S::stepped, 1, 127, 100, {}, T::live, 0},
    {"torque.drive", "Drive", "Voice", "%", S::linear, 0, 1, 0.2, {}, T::live, g},
    {"torque.slide", "Slide", "Voice", "s", S::logarithmic, 0.01, 1, 0.06, {}, T::live},
    {"torque.bend", "Bend Range", "Voice", "st", S::stepped, 0, 12, 2, {}, T::live, 0},
    {"torque.perf.bend", "Pitch Bend", "Performance", "%", S::linear, -1, 1, 0, {}, T::live, 0.01},
    {"torque.perf.wheel", "Mod Wheel", "Performance", "%", S::linear, 0, 1, 0, {}, T::live, 0.01},
    // Amplifier
    {"torque.amp.attack", "Attack", "Amp", "s", S::logarithmic, 0.001, 4, 0.002, {}, T::live},
    {"torque.amp.decay", "Decay", "Amp", "s", S::logarithmic, 0.01, 10, 1, {}, T::live},
    {"torque.amp.sustain", "Sustain", "Amp", "%", S::linear, 0, 1, 1, {}, T::live},
    {"torque.amp.release", "Release", "Amp", "s", S::logarithmic, 0.005, 4, 0.03, {}, T::live},
    // Acid
    {"torque.acid.wave", "Wave", "Acid", "", S::choice, 0, 1, 0, acid_waves, T::live, 0},
    {"torque.acid.tilt", "Sag", "Acid", "%", S::linear, 0, 1, 0.35, {}, T::live, g},
    // Moog
    {"torque.moog.wave1", "Osc 1", "Moog", "", S::choice, 0, 2, 0, moog_waves, T::live, 0},
    {"torque.moog.wave2", "Osc 2", "Moog", "", S::choice, 0, 2, 0, moog_waves, T::live, 0},
    {"torque.moog.wave3", "Osc 3", "Moog", "", S::choice, 0, 2, 2, moog_waves, T::live, 0},
    {"torque.moog.level2", "Level 2", "Moog", "%", S::linear, 0, 1, 0.7, {}, T::live, g},
    {"torque.moog.level3", "Level 3", "Moog", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    {"torque.moog.interval2", "Interval 2", "Moog", "st", S::stepped, -24, 24, 0, {}, T::live, 0},
    {"torque.moog.interval3", "Interval 3", "Moog", "st", S::stepped, -24, 24, -12, {}, T::live, 0},
    {"torque.moog.detune", "Detune", "Moog", "ct", S::linear, 0, 50, 7, {}, T::live, g},
    {"torque.moog.noise", "Noise", "Moog", "%", S::linear, 0, 1, 0, {}, T::live, g},
    // Reese
    {"torque.reese.voices", "Voices", "Reese", "", S::stepped, 2, 7, 5, {}, T::live, 0},
    {"torque.reese.detune", "Detune", "Reese", "ct", S::linear, 0, 60, 18, {}, T::live, g},
    {"torque.reese.spread", "Spread", "Reese", "%", S::linear, 0, 1, 0.7, {}, T::live, g},
    {"torque.reese.sub", "Sub", "Reese", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    {"torque.reese.filter", "Filter", "Reese", "", S::choice, 0, 1, 0, reese_filters, T::live, 0},
    {"torque.reese.comb", "Comb", "Reese", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
    // Sub/808
    {"torque.sub.drop", "Drop", "Sub", "st", S::linear, 0, 24, 7, {}, T::live, g},
    {"torque.sub.drop_time", "Drop Time", "Sub", "s", S::logarithmic, 0.005, 1, 0.06, {}, T::live},
    {"torque.sub.layer", "Layer", "Sub", "", S::choice, 0, 2, 0, sub_layers, T::live, 0},
    {"torque.sub.layer_level", "Layer Level", "Sub", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
    // LFO
    {"torque.lfo.shape", "Shape", "LFO", "", S::choice, 0, last(lfo_shapes.size()), 0, lfo_shapes,
     T::live, 0},
    {"torque.lfo.rate", "Rate", "LFO", "Hz", S::logarithmic, 0.05, 30, 2, {}, T::live, g},
    {"torque.lfo.sync", "Sync", "LFO", "", S::choice, 0, 1, 1, off_on, T::live, 0},
    {"torque.lfo.division", "Division", "LFO", "", S::choice, 0, last(divisions.size()), 8,
     divisions, T::live, 0},
    {"torque.lfo.cutoff", "To Cutoff", "LFO", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"torque.lfo.pitch", "To Pitch", "LFO", "%", S::linear, 0, 1, 0, {}, T::live, g, true, 2},
    {"torque.lfo.movement", "Movement", "LFO", "%", S::linear, 0, 1, 0, {}, T::live, g},
    // Effects and output
    {"torque.fx.dist_mode", "Distortion", "Effects", "", S::choice, 0, 2, 0, distortion_modes,
     T::live, 0},
    {"torque.fx.dist_drive", "Dist Drive", "Effects", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
    {"torque.fx.dist_tone", "Tone", "Effects", "%", S::linear, 0, 1, 0.7, {}, T::live, g},
    {"torque.fx.dist_mix", "Dist Mix", "Effects", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
    {"torque.fx.chorus", "Chorus", "Effects", "", S::choice, 0, 3, 0, chorus_modes, T::live, 0},
    {"torque.fx.delay", "Delay", "Effects", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"torque.fx.delay_time",
     "Time",
     "Effects",
     "s",
     S::logarithmic,
     0.02,
     2,
     0.375,
     {},
     T::live,
     0.05},
    {"torque.fx.delay_feedback",
     "Feedback",
     "Effects",
     "%",
     S::linear,
     0,
     0.9,
     0.35,
     {},
     T::live,
     g},
    {"torque.fx.delay_sync", "Sync", "Effects", "", S::choice, 0, 1, 1, off_on, T::live, 0},
    {"torque.fx.delay_division", "Division", "Effects", "", S::choice, 0, last(divisions.size()), 6,
     divisions, T::live, 0},
    {"torque.out.level", "Level", "Output", "dB", S::linear, -60, 6, 0, {}, T::live, g},
    // Sequencer: runs while a note is held, rooted on it (ADR-0032).
    {"torque.seq.on", "Sequencer", "Sequencer", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"torque.seq.length", "Length", "Sequencer", "", S::stepped, 1, 16, 16, {}, T::live, 0},
    {"torque.seq.rate", "Rate", "Sequencer", "", S::choice, 0, 3, 1, seq_rates, T::live, 0},
    {"torque.seq.swing", "Swing", "Sequencer", "%", S::linear, 0, 0.75, 0, {}, T::live, g},
    {"torque.seq.gate", "Gate", "Sequencer", "%", S::linear, 0.05, 1, 0.5, {}, T::live, g},
    // A classic minor acid line, so switching the sequencer on plays at once.
    TORQUE_STEP("1", 0, 0, 1, 1, 0),
    TORQUE_STEP("2", 0, 0, 1, 0, 0),
    TORQUE_STEP("3", 0, 1, 1, 0, 1),
    TORQUE_STEP("4", 0, 0, 1, 0, 0),
    TORQUE_STEP("5", 3, 0, 1, 1, 0),
    TORQUE_STEP("6", 0, 0, 0, 0, 0),
    TORQUE_STEP("7", 7, 0, 1, 0, 1),
    TORQUE_STEP("8", 10, 0, 1, 0, 0),
    TORQUE_STEP("9", 0, 0, 1, 1, 0),
    TORQUE_STEP("10", 0, 0, 2, 0, 0),
    TORQUE_STEP("11", 0, 1, 1, 0, 1),
    TORQUE_STEP("12", 3, 1, 1, 1, 0),
    TORQUE_STEP("13", 0, 0, 1, 0, 0),
    TORQUE_STEP("14", 5, 0, 1, 0, 1),
    TORQUE_STEP("15", 7, 0, 1, 1, 0),
    TORQUE_STEP("16", 0, -1, 1, 0, 0),
}};

#undef TORQUE_STEP

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

inline constexpr std::size_t first_step = index_of("torque.seq.s1.note");

} // namespace oscillate::native_dsp::synth::torque_table
