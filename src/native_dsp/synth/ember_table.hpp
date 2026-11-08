#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/transport.hpp"

#include <array>
#include <cstddef>
#include <string_view>

// Ember v1 parameter table. Ids are persisted and automated: never rename or
// reuse one. Discrete parameters are never automatable (validate_engine).
namespace oscillate::native_dsp::synth::ember_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array dco_ranges{"16'"sv, "8'"sv, "4'"sv};
inline constexpr std::array pwm_sources{"Manual"sv, "LFO"sv, "Env"sv};
inline constexpr std::array vco_waves{"Saw"sv, "Pulse"sv, "Triangle"sv};
inline constexpr std::array off_on{"Off"sv, "On"sv};
inline constexpr std::array filter_types{"OTA 24"sv, "SEM 12"sv};
inline constexpr std::array amp_modes{"Env"sv, "Gate"sv, "Own"sv};
inline constexpr std::array lfo_shapes{"Triangle"sv, "Square"sv, "S&H"sv, "Saw"sv};
// Tempo-synced divisions are shared by every engine (tempo_sync.hpp).
inline constexpr const auto& lfo_divisions = sync_divisions;
inline constexpr std::array voice_modes{"Poly"sv, "Unison"sv, "Chord"sv};
inline constexpr std::array chord_shapes{"Major"sv, "Minor"sv,  "Maj 7"sv,  "Min 7"sv,
                                         "Sus 4"sv, "Fifths"sv, "Octaves"sv};
inline constexpr std::array chorus_modes{"Off"sv, "I"sv, "II"sv, "I+II"sv};
inline constexpr std::array mains_frequencies{"50 Hz"sv, "60 Hz"sv};

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = 0.02;

inline constexpr std::array<ParameterDescriptor, 72> parameters{{
    // DCO (Juno-style): mixable waves from one counter.
    {"ember.dco.range", "Range", "DCO", "", S::choice, 0, 2, 1, dco_ranges, T::live, 0},
    {"ember.dco.saw", "Saw", "DCO", "%", S::linear, 0, 1, 0.8, {}, T::live, g},
    {"ember.dco.pulse", "Pulse", "DCO", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"ember.dco.sub", "Sub", "DCO", "%", S::linear, 0, 1, 0.35, {}, T::live, g},
    {"ember.dco.noise", "Noise", "DCO", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"ember.dco.pw", "Width", "DCO", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"ember.dco.pwm_source", "PWM Source", "DCO", "", S::choice, 0, 2, 1, pwm_sources, T::live, 0},
    {"ember.dco.pwm", "PWM", "DCO", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
    // VCO B (Prophet-style)
    {"ember.vco.wave", "Wave", "VCO B", "", S::choice, 0, 2, 0, vco_waves, T::live, 0},
    {"ember.vco.level", "Level", "VCO B", "%", S::linear, 0, 1, 0.45, {}, T::live, g},
    {"ember.vco.semi", "Semi", "VCO B", "st", S::stepped, -24, 24, 0, {}, T::live, 0},
    {"ember.vco.fine", "Fine", "VCO B", "ct", S::linear, -50, 50, 7, {}, T::live, g},
    {"ember.vco.pw", "Width", "VCO B", "%", S::linear, 0.05, 0.95, 0.5, {}, T::live, g},
    {"ember.vco.sync", "Sync", "VCO B", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"ember.vco.low", "Lo Freq", "VCO B", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    // Poly-mod: filter envelope and VCO B modulate the DCO and filter.
    {"ember.pm.env", "Filter Env", "Poly Mod", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"ember.pm.osc", "VCO B", "Poly Mod", "%", S::linear, 0, 1, 0, {}, T::live, g, true, 2},
    {"ember.pm.freq", "To Freq", "Poly Mod", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"ember.pm.pw", "To Width", "Poly Mod", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"ember.pm.cutoff", "To Filter", "Poly Mod", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    // Filter section
    {"ember.hpf", "HPF", "Filter", "", S::stepped, 0, 3, 0, {}, T::live, 0},
    {"ember.filter.type", "Type", "Filter", "", S::choice, 0, 1, 0, filter_types, T::live, 0},
    {"ember.filter.cutoff",
     "Cutoff",
     "Filter",
     "Hz",
     S::logarithmic,
     20,
     20'000,
     1'800,
     {},
     T::live,
     g},
    {"ember.filter.resonance", "Resonance", "Filter", "%", S::linear, 0, 1, 0.2, {}, T::live, g},
    {"ember.filter.morph", "Morph", "Filter", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"ember.filter.env", "Env", "Filter", "%", S::linear, -1, 1, 0.35, {}, T::live, g, true, 1.5},
    {"ember.filter.lfo", "LFO", "Filter", "%", S::linear, 0, 1, 0, {}, T::live, g, true, 2},
    {"ember.filter.keytrack", "Key", "Filter", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    {"ember.filter.velocity", "Velocity", "Filter", "%", S::linear, 0, 1, 0.2, {}, T::live, g},
    {"ember.filter.drive", "Drive", "Filter", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
    // Envelope (filter and shared amp) and amp
    {"ember.env.attack", "Attack", "Env", "s", S::logarithmic, 0.001, 10, 0.004, {}, T::live},
    {"ember.env.decay", "Decay", "Env", "s", S::logarithmic, 0.002, 12, 0.7, {}, T::live},
    {"ember.env.sustain", "Sustain", "Env", "%", S::linear, 0, 1, 0.45, {}, T::live},
    {"ember.env.release", "Release", "Env", "s", S::logarithmic, 0.002, 12, 0.45, {}, T::live},
    {"ember.amp.mode", "Amp", "Amp", "", S::choice, 0, 2, 2, amp_modes, T::live, 0},
    {"ember.amp.attack", "Attack", "Amp", "s", S::logarithmic, 0.001, 10, 0.003, {}, T::live},
    {"ember.amp.decay", "Decay", "Amp", "s", S::logarithmic, 0.002, 12, 1.2, {}, T::live},
    {"ember.amp.sustain", "Sustain", "Amp", "%", S::linear, 0, 1, 0.75, {}, T::live},
    {"ember.amp.release", "Release", "Amp", "s", S::logarithmic, 0.002, 12, 0.5, {}, T::live},
    {"ember.amp.velocity", "Velocity", "Amp", "%", S::linear, 0, 1, 0.35, {}, T::live, g},
    // LFO
    {"ember.lfo.shape", "Shape", "LFO", "", S::choice, 0, 3, 0, lfo_shapes, T::live, 0},
    {"ember.lfo.rate", "Rate", "LFO", "Hz", S::logarithmic, 0.05, 30, 4.5, {}, T::live, g},
    {"ember.lfo.sync", "Sync", "LFO", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"ember.lfo.division", "Division", "LFO", "", S::choice, 0, last(lfo_divisions.size()), 8,
     lfo_divisions, T::live, 0},
    {"ember.lfo.delay", "Delay", "LFO", "s", S::linear, 0, 5, 0.6, {}, T::live, 0, true, 2},
    {"ember.lfo.pitch", "To Pitch", "LFO", "%", S::linear, 0, 1, 0, {}, T::live, g, true, 2},
    // The circuit (ADR-0066): how far each voice card has wandered from its
    // schematic, how well the VCOs hold their tune, what leaks, the mains,
    // and which instrument (unit) this is.
    {"ember.age", "Age", "Circuit", "%", S::linear, 0, 1, 0.35, {}, T::live, g},
    {"ember.circuit.tuning", "Tuning", "Circuit", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
    {"ember.circuit.bleed", "Bleed", "Circuit", "%", S::linear, 0, 1, 0.25, {}, T::live, g},
    {"ember.circuit.hum", "Hum", "Circuit", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"ember.circuit.mains", "Mains", "Circuit", "", S::choice, 0, 1, 0, mains_frequencies, T::live,
     0},
    {"ember.circuit.unit", "Unit", "Circuit", "", S::stepped, 1, 16, 1, {}, T::next_note, 0},
    // Voicing and performance
    {"ember.voice.mode", "Mode", "Voice", "", S::choice, 0, 2, 0, voice_modes, T::next_note, 0},
    {"ember.voice.chord", "Chord", "Voice", "", S::choice, 0, last(chord_shapes.size()), 0,
     chord_shapes, T::next_note, 0},
    {"ember.voice.detune",
     "Unison Detune",
     "Voice",
     "ct",
     S::linear,
     0,
     50,
     12,
     {},
     T::live,
     g,
     true,
     2},
    {"ember.voice.glide", "Glide", "Voice", "s", S::linear, 0, 3, 0, {}, T::live, 0, true, 3},
    {"ember.voice.bend", "Bend Range", "Voice", "st", S::stepped, 0, 12, 2, {}, T::live, 0},
    {"ember.perf.wheel", "Mod Wheel", "Performance", "%", S::linear, 0, 1, 0, {}, T::live, 0.01},
    {"ember.perf.bend", "Pitch Bend", "Performance", "%", S::linear, -1, 1, 0, {}, T::live, 0.01},
    {"ember.perf.pressure", "Pressure", "Performance", "%", S::linear, 0, 1, 0, {}, T::live, 0.01},
    // Effects and output
    {"ember.fx.ensemble", "Ensemble", "Effects", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"ember.fx.chorus", "Chorus", "Effects", "", S::choice, 0, 3, 1, chorus_modes, T::live, 0},
    {"ember.fx.hiss", "BBD Hiss", "Effects", "%", S::linear, 0, 1, 0.2, {}, T::live, g},
    {"ember.fx.delay", "Delay", "Effects", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"ember.fx.delay_time",
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
    {"ember.fx.delay_feedback",
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
    {"ember.fx.tape", "Tape", "Effects", "%", S::linear, 0, 1, 0.15, {}, T::live, g},
    {"ember.fx.delay_sync", "Sync", "Effects", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"ember.fx.delay_division", "Division", "Effects", "", S::choice, 0, last(lfo_divisions.size()),
     6, lfo_divisions, T::live, 0},
    {"ember.fx.delay_pingpong", "Ping-Pong", "Effects", "", S::choice, 0, 1, 1, off_on, T::live, 0},
    {"ember.fx.wobble", "Wow & Flutter", "Effects", "%", S::linear, 0, 1, 0.1, {}, T::live, g},
    {"ember.out.level", "Level", "Output", "dB", S::linear, -60, 6, 0, {}, T::live, g},
}};

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

} // namespace oscillate::native_dsp::synth::ember_table
