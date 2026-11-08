#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/transport.hpp"

#include <array>
#include <cstddef>
#include <string_view>

// Drift v1 parameter table. Defaults are the "Nebula" preset. Ids are
// persisted and automated: never rename or reuse one. Every parameter is
// automatable; discrete ones as stepped lanes (ADR-0034).
namespace oscillate::native_dsp::synth::drift_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

// The shared generated wavetables (wavetable.hpp), in their order.
inline constexpr std::array tables{
    "Basic Shapes"sv,  "PWM Sweep"sv,     "Harmonic Series"sv, "Odd Harmonics"sv,
    "Vowel"sv,         "Sync Sweep"sv,    "FM Sweep"sv,        "Phase Distortion"sv,
    "Bit Crush"sv,     "Noise Spectra"sv, "Bell Partials"sv,   "Organ Drawbars"sv,
    "Digital Stack"sv, "Hollow"sv,        "Pluck Decay"sv,     "Saw Fold"sv};
inline constexpr std::array shapes{"Hann"sv, "Tukey"sv, "Decay"sv, "Swell"sv};
inline constexpr std::array interval_sets{"Off"sv, "Octaves"sv, "Fifths"sv, "Pentatonic"sv};
inline constexpr std::array off_on{"Off"sv, "On"sv};
inline constexpr std::array filter_types{"LP 12"sv, "LP 24"sv, "BP 12"sv, "HP 12"sv};
inline constexpr std::array lfo_shapes{"Sine"sv, "Triangle"sv, "Saw"sv, "Square"sv, "S&H"sv};
// Tempo-synced divisions are shared by every engine (tempo_sync.hpp).
inline constexpr const auto& divisions = sync_divisions;
inline constexpr std::array chorus_modes{"Off"sv, "I"sv, "II"sv, "I+II"sv};

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = synth_glide_seconds;

#define DRIFT_LFO(X, RATE, DIVISION)                                                               \
  {"drift.lfo" X ".shape",  "Shape", "LFO " X,   "",     S::choice, 0,                             \
   last(lfo_shapes.size()), 0,       lfo_shapes, T::live},                                         \
      {"drift.lfo" X ".rate",                                                                      \
       "Rate",                                                                                     \
       "LFO " X,                                                                                   \
       "Hz",                                                                                       \
       S::logarithmic,                                                                             \
       0.01,                                                                                       \
       20,                                                                                         \
       RATE,                                                                                       \
       {},                                                                                         \
       T::live,                                                                                    \
       g},                                                                                         \
      {"drift.lfo" X ".sync", "Sync", "LFO " X, "", S::choice, 0, 1, 0, off_on, T::live}, {        \
    "drift.lfo" X ".division", "Division", "LFO " X, "", S::choice, 0, last(divisions.size()),     \
        DIVISION, divisions, T::live                                                               \
  }

inline constexpr std::array<ParameterDescriptor, 64> parameters{{
    // Source: what each note writes into its buffer.
    {"drift.source.octave", "Octave", "Source", "oct", S::stepped, -2, 1, 0, {}, T::live},
    {"drift.wt.table", "Table", "Source", "", S::choice, 0, last(tables.size()), 4, tables,
     T::live},
    {"drift.wt.level", "Wavetable", "Source", "%", S::linear, 0, 1, 0.6, {}, T::live, g},
    {"drift.wt.position", "Position", "Source", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
    {"drift.wt.drift", "Drift", "Source", "%", S::linear, 0, 1, 0.4, {}, T::live, g},
    {"drift.partials.level", "Partials", "Source", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    {"drift.partials.tilt", "Tilt", "Source", "", S::linear, 0, 3, 1.2, {}, T::live, g},
    {"drift.partials.odd", "Odd/Even", "Source", "%", S::linear, -1, 1, 0, {}, T::live, g},
    {"drift.partials.stretch",
     "Stretch",
     "Source",
     "",
     S::linear,
     0,
     0.002,
     0.0003,
     {},
     T::live,
     g,
     true,
     2},
    {"drift.air.level", "Air", "Source", "%", S::linear, 0, 1, 0.15, {}, T::live, g},
    {"drift.air.width", "Air Width", "Source", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    // Grains
    {"drift.grain.size", "Size", "Grains", "s", S::logarithmic, 0.005, 0.5, 0.18, {}, T::live, g},
    {"drift.grain.density",
     "Density",
     "Grains",
     "Hz",
     S::logarithmic,
     0.5,
     200,
     24,
     {},
     T::live,
     g},
    {"drift.grain.chaos", "Chaos", "Grains", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
    {"drift.grain.shape", "Shape", "Grains", "", S::choice, 0, last(shapes.size()), 0, shapes,
     T::live},
    {"drift.grain.position",
     "Position",
     "Grains",
     "s",
     S::linear,
     0,
     3,
     0.6,
     {},
     T::live,
     g,
     true,
     2},
    {"drift.grain.scan", "Scan", "Grains", "x", S::linear, -2, 2, 0.25, {}, T::live, g},
    {"drift.grain.spray", "Spray", "Grains", "s", S::linear, 0, 1, 0.15, {}, T::live, g, true, 2},
    {"drift.grain.pitch", "Pitch", "Grains", "st", S::stepped, -24, 24, 0, {}, T::live},
    {"drift.grain.intervals", "Intervals", "Grains", "", S::choice, 0, last(interval_sets.size()),
     1, interval_sets, T::live},
    {"drift.grain.interval_amount",
     "Harmony",
     "Grains",
     "%",
     S::linear,
     0,
     1,
     0.25,
     {},
     T::live,
     g},
    {"drift.grain.jitter", "Jitter", "Grains", "ct", S::linear, 0, 100, 8, {}, T::live, g, true, 2},
    {"drift.grain.reverse", "Reverse", "Grains", "%", S::linear, 0, 1, 0.2, {}, T::live, g},
    {"drift.grain.spread", "Spread", "Grains", "%", S::linear, 0, 1, 0.8, {}, T::live, g},
    {"drift.blur", "Blur", "Grains", "%", S::linear, 0, 1, 0.2, {}, T::live, g},
    {"drift.freeze", "Freeze", "Grains", "", S::choice, 0, 1, 0, off_on, T::live},
    {"drift.sync", "Sync", "Grains", "", S::choice, 0, 1, 0, off_on, T::live},
    {"drift.sync.division", "Division", "Grains", "", S::choice, 0, last(divisions.size()), 2,
     divisions, T::live},
    // Amplifier and filter
    {"drift.amp.attack", "Attack", "Amp", "s", S::logarithmic, 0.001, 10, 1.2, {}, T::live},
    {"drift.amp.decay", "Decay", "Amp", "s", S::logarithmic, 0.01, 20, 2, {}, T::live},
    {"drift.amp.sustain", "Sustain", "Amp", "%", S::linear, 0, 1, 0.8, {}, T::live},
    {"drift.amp.release", "Release", "Amp", "s", S::logarithmic, 0.01, 20, 3, {}, T::live},
    {"drift.amp.velocity", "Velocity", "Amp", "%", S::linear, 0, 1, 0.4, {}, T::live, g},
    {"drift.filter.type", "Type", "Filter", "", S::choice, 0, last(filter_types.size()), 0,
     filter_types, T::live},
    {"drift.filter.cutoff",
     "Cutoff",
     "Filter",
     "Hz",
     S::logarithmic,
     40,
     20'000,
     7'000,
     {},
     T::live,
     g},
    {"drift.filter.resonance", "Resonance", "Filter", "%", S::linear, 0, 1, 0.1, {}, T::live, g},
    {"drift.filter.keytrack", "Key", "Filter", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
    // Modulation: LFO 1 moves position and density, LFO 2 filter and size.
    DRIFT_LFO("1", 0.07, 13),
    {"drift.lfo1.position", "To Position", "LFO 1", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
    {"drift.lfo1.density", "To Density", "LFO 1", "%", S::linear, 0, 1, 0, {}, T::live, g},
    DRIFT_LFO("2", 0.13, 14),
    {"drift.lfo2.filter", "To Filter", "LFO 2", "%", S::linear, 0, 1, 0.15, {}, T::live, g},
    {"drift.lfo2.size", "To Size", "LFO 2", "%", S::linear, 0, 1, 0, {}, T::live, g},
    // Performance
    {"drift.bend", "Bend Range", "Performance", "st", S::stepped, 0, 12, 2, {}, T::live},
    {"drift.perf.bend", "Pitch Bend", "Performance", "%", S::linear, -1, 1, 0, {}, T::live, 0.01},
    {"drift.perf.wheel", "Mod Wheel", "Performance", "%", S::linear, 0, 1, 0, {}, T::live, 0.01},
    // Effects and output
    {"drift.fx.chorus", "Chorus", "Effects", "", S::choice, 0, 3, 0, chorus_modes, T::live},
    {"drift.fx.delay", "Delay", "Effects", "%", S::linear, 0, 1, 0.15, {}, T::live, g},
    {"drift.fx.delay_time",
     "Time",
     "Effects",
     "s",
     S::logarithmic,
     0.02,
     2,
     0.45,
     {},
     T::live,
     0.05},
    {"drift.fx.delay_feedback",
     "Feedback",
     "Effects",
     "%",
     S::linear,
     0,
     0.9,
     0.45,
     {},
     T::live,
     g},
    {"drift.fx.delay_sync", "Sync", "Effects", "", S::choice, 0, 1, 1, off_on, T::live},
    {"drift.fx.delay_division", "Division", "Effects", "", S::choice, 0, last(divisions.size()), 6,
     divisions, T::live},
    {"drift.verb.mix", "Reverb", "Shimmer", "%", S::linear, 0, 1, 0.35, {}, T::live, g},
    {"drift.verb.size", "Size", "Shimmer", "%", S::linear, 0, 1, 0.7, {}, T::live, g},
    {"drift.verb.decay", "Decay", "Shimmer", "s", S::logarithmic, 0.5, 30, 6, {}, T::live, g},
    {"drift.verb.damping", "Damping", "Shimmer", "%", S::linear, 0, 1, 0.4, {}, T::live, g},
    {"drift.verb.shimmer", "Shimmer", "Shimmer", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
    {"drift.out.level", "Level", "Output", "dB", S::linear, -60, 6, 0, {}, T::live, g},
}};

#undef DRIFT_LFO

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

} // namespace oscillate::native_dsp::synth::drift_table
