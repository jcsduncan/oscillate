#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/transport.hpp"

#include <array>
#include <cstddef>
#include <string_view>

// Prism v1 parameter table. Ids are persisted and automated: never rename or
// reuse one. Append new parameters and bump the engine version with an
// upgrader when a meaning changes.
namespace oscillate::native_dsp::synth::prism_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array waveforms{"Saw"sv, "Pulse"sv, "Triangle"sv, "Sine"sv};
inline constexpr std::array phase_modes{"Random"sv, "Reset"sv};
inline constexpr std::array off_on{"Off"sv, "On"sv};
inline constexpr std::array sub_waves{"Sine"sv, "Square"sv};
inline constexpr std::array sub_octaves{"-1 Oct"sv, "-2 Oct"sv};
inline constexpr std::array noise_colours{"White"sv, "Pink"sv};
inline constexpr std::array filter_modes{"LP 12"sv,    "LP 24"sv,    "BP 12"sv,
                                         "BP 24"sv,    "HP 12"sv,    "HP 24"sv,
                                         "Notch 12"sv, "Notch 24"sv, "Ladder"sv};
inline constexpr std::array lfo_shapes{"Sine"sv,     "Triangle"sv, "Saw Up"sv,
                                       "Saw Down"sv, "Square"sv,   "S&H"sv};
// Tempo-synced divisions are shared by every engine (tempo_sync.hpp).
inline constexpr const auto& lfo_divisions = sync_divisions;
// Division lengths in quarter-note beats, matching lfo_divisions.
inline constexpr std::array lfo_modes{"Poly"sv, "Mono"sv};
inline constexpr std::array voice_modes{"Poly"sv, "Mono"sv, "Legato"sv};
inline constexpr std::array glide_modes{"Time"sv, "Rate"sv};
inline constexpr std::array glide_when{"Always"sv, "Legato"sv};
inline constexpr std::array chorus_modes{"Off"sv, "I"sv, "II"sv, "I+II"sv};
inline constexpr std::array mod_sources{"Off"sv,        "Env 2"sv,     "LFO 1"sv,    "LFO 2"sv,
                                        "Velocity"sv,   "Mod Wheel"sv, "Pressure"sv, "Key"sv,
                                        "Pitch Bend"sv, "Amp Env"sv};
inline constexpr std::array mod_targets{
    "Off"sv,     "Pitch"sv, "Osc 1 Pitch"sv, "Osc 2 Pitch"sv, "Pulse Width"sv,
    "Osc Mix"sv, "FM"sv,    "Cutoff"sv,      "Resonance"sv,   "Drive"sv,
    "Level"sv,   "Pan"sv,   "LFO 1 Rate"sv,  "LFO 2 Rate"sv,  "Noise"sv};

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }

// Panel knob glide; long enough to hide zipper noise, short enough to feel direct.
inline constexpr double g = synth_glide_seconds;

inline constexpr std::array<ParameterDescriptor, 89> parameters{{
    // Oscillator 1
    {"prism.osc1.wave", "Wave", "Osc 1", "", S::choice, 0, last(waveforms.size()), 0, waveforms,
     T::live, 0},
    {"prism.osc1.octave", "Octave", "Osc 1", "oct", S::stepped, -3, 3, 0, {}, T::live, 0},
    {"prism.osc1.semi", "Semi", "Osc 1", "st", S::stepped, -12, 12, 0, {}, T::live, 0},
    {"prism.osc1.fine", "Fine", "Osc 1", "ct", S::linear, -100, 100, 0, {}, T::live, g},
    {"prism.osc1.pw", "Width", "Osc 1", "%", S::linear, 0.05, 0.95, 0.5, {}, T::live, g},
    {"prism.osc1.level", "Level", "Osc 1", "%", S::linear, 0, 1, 0.75, {}, T::live, g},
    {"prism.osc1.unison", "Unison", "Osc 1", "", S::stepped, 1, 8, 1, {}, T::live, 0},
    {"prism.osc1.detune", "Detune", "Osc 1", "ct", S::linear, 0, 100, 12, {}, T::live, g, true, 2},
    {"prism.osc1.width", "Spread", "Osc 1", "%", S::linear, 0, 1, 0.6, {}, T::live, g},
    {"prism.osc1.phase", "Phase", "Osc 1", "", S::choice, 0, last(phase_modes.size()), 0,
     phase_modes, T::next_note, 0},
    // Oscillator 2
    {"prism.osc2.wave", "Wave", "Osc 2", "", S::choice, 0, last(waveforms.size()), 0, waveforms,
     T::live, 0},
    {"prism.osc2.octave", "Octave", "Osc 2", "oct", S::stepped, -3, 3, 0, {}, T::live, 0},
    {"prism.osc2.semi", "Semi", "Osc 2", "st", S::stepped, -12, 12, 0, {}, T::live, 0},
    {"prism.osc2.fine", "Fine", "Osc 2", "ct", S::linear, -100, 100, 7, {}, T::live, g},
    {"prism.osc2.pw", "Width", "Osc 2", "%", S::linear, 0.05, 0.95, 0.5, {}, T::live, g},
    {"prism.osc2.level", "Level", "Osc 2", "%", S::linear, 0, 1, 0.55, {}, T::live, g},
    {"prism.osc2.unison", "Unison", "Osc 2", "", S::stepped, 1, 8, 1, {}, T::live, 0},
    {"prism.osc2.detune", "Detune", "Osc 2", "ct", S::linear, 0, 100, 12, {}, T::live, g, true, 2},
    {"prism.osc2.width", "Spread", "Osc 2", "%", S::linear, 0, 1, 0.6, {}, T::live, g},
    {"prism.osc2.phase", "Phase", "Osc 2", "", S::choice, 0, last(phase_modes.size()), 0,
     phase_modes, T::next_note, 0},
    {"prism.osc2.sync", "Sync", "Osc 2", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"prism.osc2.fm", "FM", "Osc 2", "%", S::linear, 0, 1, 0, {}, T::live, g, true, 2},
    // Sub, noise and ring
    {"prism.sub.wave", "Wave", "Sub", "", S::choice, 0, 1, 0, sub_waves, T::live, 0},
    {"prism.sub.octave", "Octave", "Sub", "", S::choice, 0, 1, 0, sub_octaves, T::live, 0},
    {"prism.sub.level", "Level", "Sub", "%", S::linear, 0, 1, 0.25, {}, T::live, g},
    {"prism.noise.colour", "Colour", "Noise", "", S::choice, 0, 1, 0, noise_colours, T::live, 0},
    {"prism.noise.level", "Level", "Noise", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"prism.ring.level", "Ring", "Mixer", "%", S::linear, 0, 1, 0, {}, T::live, g},
    // Filter
    {"prism.filter.mode", "Type", "Filter", "", S::choice, 0, last(filter_modes.size()), 1,
     filter_modes, T::live, 0},
    {"prism.filter.cutoff",
     "Cutoff",
     "Filter",
     "Hz",
     S::logarithmic,
     20,
     20'000,
     2'400,
     {},
     T::live,
     g},
    {"prism.filter.resonance", "Resonance", "Filter", "%", S::linear, 0, 1, 0.15, {}, T::live, g},
    {"prism.filter.drive", "Drive", "Filter", "dB", S::linear, 0, 24, 3, {}, T::live, g},
    {"prism.filter.keytrack", "Key Track", "Filter", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    {"prism.filter.env",
     "Env Amount",
     "Filter",
     "st",
     S::linear,
     -96,
     96,
     24,
     {},
     T::live,
     g,
     true,
     1.5},
    {"prism.filter.velocity", "Velocity", "Filter", "%", S::linear, 0, 1, 0.25, {}, T::live, g},
    // Amp envelope
    {"prism.amp.attack", "Attack", "Amp Env", "s", S::logarithmic, 0.0005, 20, 0.003, {}, T::live},
    {"prism.amp.decay", "Decay", "Amp Env", "s", S::logarithmic, 0.0005, 20, 0.6, {}, T::live},
    {"prism.amp.sustain", "Sustain", "Amp Env", "%", S::linear, 0, 1, 0.7, {}, T::live},
    {"prism.amp.release", "Release", "Amp Env", "s", S::logarithmic, 0.0005, 20, 0.35, {}, T::live},
    {"prism.amp.curve", "Curve", "Amp Env", "%", S::linear, 0, 1, 0.5, {}, T::live},
    {"prism.amp.velocity", "Velocity", "Amp Env", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    // Mod envelope (also the filter envelope)
    {"prism.env2.attack", "Attack", "Env 2", "s", S::logarithmic, 0.0005, 20, 0.002, {}, T::live},
    {"prism.env2.decay", "Decay", "Env 2", "s", S::logarithmic, 0.0005, 20, 0.45, {}, T::live},
    {"prism.env2.sustain", "Sustain", "Env 2", "%", S::linear, 0, 1, 0.25, {}, T::live},
    {"prism.env2.release", "Release", "Env 2", "s", S::logarithmic, 0.0005, 20, 0.35, {}, T::live},
    {"prism.env2.curve", "Curve", "Env 2", "%", S::linear, 0, 1, 0.6, {}, T::live},
    // LFO 1 (hard-wired to vibrato)
    {"prism.lfo1.shape", "Shape", "LFO 1", "", S::choice, 0, last(lfo_shapes.size()), 0, lfo_shapes,
     T::live, 0},
    {"prism.lfo1.rate", "Rate", "LFO 1", "Hz", S::logarithmic, 0.01, 40, 5.5, {}, T::live, g},
    {"prism.lfo1.sync", "Sync", "LFO 1", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"prism.lfo1.division", "Division", "LFO 1", "", S::choice, 0, last(lfo_divisions.size()), 8,
     lfo_divisions, T::live, 0},
    {"prism.lfo1.retrigger", "Retrigger", "LFO 1", "", S::choice, 0, 1, 1, off_on, T::live, 0},
    {"prism.lfo1.fade", "Fade In", "LFO 1", "s", S::linear, 0, 10, 0.4, {}, T::live, 0, true, 3},
    {"prism.lfo1.mode", "Mode", "LFO 1", "", S::choice, 0, 1, 0, lfo_modes, T::next_note, 0},
    {"prism.lfo1.vibrato", "Vibrato", "LFO 1", "ct", S::linear, 0, 100, 0, {}, T::live, g, true, 2},
    {"prism.lfo1.wheel",
     "Wheel Vibrato",
     "LFO 1",
     "ct",
     S::linear,
     0,
     100,
     25,
     {},
     T::live,
     g,
     true,
     2},
    // LFO 2 (hard-wired to cutoff and pulse width)
    {"prism.lfo2.shape", "Shape", "LFO 2", "", S::choice, 0, last(lfo_shapes.size()), 1, lfo_shapes,
     T::live, 0},
    {"prism.lfo2.rate", "Rate", "LFO 2", "Hz", S::logarithmic, 0.01, 40, 0.35, {}, T::live, g},
    {"prism.lfo2.sync", "Sync", "LFO 2", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"prism.lfo2.division", "Division", "LFO 2", "", S::choice, 0, last(lfo_divisions.size()), 11,
     lfo_divisions, T::live, 0},
    {"prism.lfo2.retrigger", "Retrigger", "LFO 2", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"prism.lfo2.fade", "Fade In", "LFO 2", "s", S::linear, 0, 10, 0, {}, T::live, 0, true, 3},
    {"prism.lfo2.mode", "Mode", "LFO 2", "", S::choice, 0, 1, 1, lfo_modes, T::next_note, 0},
    {"prism.lfo2.cutoff", "To Cutoff", "LFO 2", "st", S::linear, 0, 48, 0, {}, T::live, g, true, 2},
    {"prism.lfo2.pw", "To Width", "LFO 2", "%", S::linear, 0, 0.45, 0, {}, T::live, g},
    // Modulation matrix
    {"prism.mod1.source", "Source", "Mod 1", "", S::choice, 0, last(mod_sources.size()), 0,
     mod_sources, T::live, 0},
    {"prism.mod1.target", "Target", "Mod 1", "", S::choice, 0, last(mod_targets.size()), 0,
     mod_targets, T::live, 0},
    {"prism.mod1.amount", "Amount", "Mod 1", "%", S::linear, -1, 1, 0, {}, T::live, g},
    {"prism.mod2.source", "Source", "Mod 2", "", S::choice, 0, last(mod_sources.size()), 0,
     mod_sources, T::live, 0},
    {"prism.mod2.target", "Target", "Mod 2", "", S::choice, 0, last(mod_targets.size()), 0,
     mod_targets, T::live, 0},
    {"prism.mod2.amount", "Amount", "Mod 2", "%", S::linear, -1, 1, 0, {}, T::live, g},
    {"prism.mod3.source", "Source", "Mod 3", "", S::choice, 0, last(mod_sources.size()), 0,
     mod_sources, T::live, 0},
    {"prism.mod3.target", "Target", "Mod 3", "", S::choice, 0, last(mod_targets.size()), 0,
     mod_targets, T::live, 0},
    {"prism.mod3.amount", "Amount", "Mod 3", "%", S::linear, -1, 1, 0, {}, T::live, g},
    {"prism.mod4.source", "Source", "Mod 4", "", S::choice, 0, last(mod_sources.size()), 0,
     mod_sources, T::live, 0},
    {"prism.mod4.target", "Target", "Mod 4", "", S::choice, 0, last(mod_targets.size()), 0,
     mod_targets, T::live, 0},
    {"prism.mod4.amount", "Amount", "Mod 4", "%", S::linear, -1, 1, 0, {}, T::live, g},
    // Voicing
    {"prism.voice.mode", "Voice Mode", "Voice", "", S::choice, 0, last(voice_modes.size()), 0,
     voice_modes, T::next_note, 0},
    {"prism.voice.glide", "Glide", "Voice", "s", S::linear, 0, 5, 0, {}, T::live, 0, true, 3},
    {"prism.voice.glide_mode", "Glide Mode", "Voice", "", S::choice, 0, 1, 0, glide_modes, T::live,
     0},
    {"prism.voice.glide_when", "Glide When", "Voice", "", S::choice, 0, 1, 1, glide_when, T::live,
     0},
    {"prism.voice.bend_up", "Bend Up", "Voice", "st", S::stepped, 0, 24, 2, {}, T::live, 0},
    {"prism.voice.bend_down", "Bend Down", "Voice", "st", S::stepped, 0, 24, 2, {}, T::live, 0},
    // Performance controls, automatable until clips carry controller data.
    {"prism.perf.wheel", "Mod Wheel", "Performance", "%", S::linear, 0, 1, 0, {}, T::live, 0.01},
    {"prism.perf.bend", "Pitch Bend", "Performance", "%", S::linear, -1, 1, 0, {}, T::live, 0.01},
    {"prism.perf.pressure", "Pressure", "Performance", "%", S::linear, 0, 1, 0, {}, T::live, 0.01},
    // Effects and output
    {"prism.fx.drive", "Drive", "Effects", "dB", S::linear, 0, 24, 0, {}, T::live, g},
    {"prism.fx.chorus", "Chorus", "Effects", "", S::choice, 0, last(chorus_modes.size()), 1,
     chorus_modes, T::live, 0},
    {"prism.fx.chorus_mix", "Chorus Mix", "Effects", "%", S::linear, 0, 1, 0.6, {}, T::live, g},
    {"prism.out.level", "Level", "Output", "dB", S::linear, -60, 6, -2, {}, T::live, g},
}};

// Compile-time index lookup: a misspelt id fails to compile.
consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

} // namespace oscillate::native_dsp::synth::prism_table
