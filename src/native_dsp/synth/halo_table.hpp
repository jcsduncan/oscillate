#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/transport.hpp"

#include <array>
#include <cstddef>
#include <string_view>

// Halo v1 parameter table: six operator blocks with the same thirteen
// controls, then the global section. Defaults are the "E-Piano" preset.
// Ids are persisted and automated: never rename or reuse one.
namespace oscillate::native_dsp::synth::halo_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::size_t operator_parameters = 18;

inline constexpr std::array operator_modes{"Ratio"sv, "Fixed"sv};
// Coarse ratio choice 0 is 0.5; choice n is n.
inline constexpr std::array ratios{"0.5"sv, "1"sv,  "2"sv,  "3"sv,  "4"sv,  "5"sv,  "6"sv,  "7"sv,
                                   "8"sv,   "9"sv,  "10"sv, "11"sv, "12"sv, "13"sv, "14"sv, "15"sv,
                                   "16"sv,  "17"sv, "18"sv, "19"sv, "20"sv, "21"sv, "22"sv, "23"sv,
                                   "24"sv,  "25"sv, "26"sv, "27"sv, "28"sv, "29"sv, "30"sv, "31"sv};
inline constexpr std::array waves{"Sine"sv, "Half"sv, "Abs"sv, "Quarter"sv};
inline constexpr std::array algorithms{
    "1"sv,  "2"sv,  "3"sv,  "4"sv,  "5"sv,  "6"sv,  "7"sv,  "8"sv,  "9"sv,  "10"sv, "11"sv,
    "12"sv, "13"sv, "14"sv, "15"sv, "16"sv, "17"sv, "18"sv, "19"sv, "20"sv, "21"sv, "22"sv,
    "23"sv, "24"sv, "25"sv, "26"sv, "27"sv, "28"sv, "29"sv, "30"sv, "31"sv, "32"sv};
inline constexpr std::array filter_types{"Off"sv,   "LP 12"sv, "LP 24"sv,
                                         "BP 12"sv, "HP 12"sv, "Ladder"sv};
inline constexpr std::array lfo_shapes{"Sine"sv, "Triangle"sv, "Saw"sv, "Square"sv, "S&H"sv};
inline constexpr std::array off_on{"Off"sv, "On"sv};
// Tempo-synced divisions are shared by every engine (tempo_sync.hpp).
inline constexpr const auto& lfo_divisions = sync_divisions;
inline constexpr std::array voice_modes{"Poly"sv, "Mono"sv, "Legato"sv};
inline constexpr std::array chorus_modes{"Off"sv, "I"sv, "II"sv, "I+II"sv};

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = synth_glide_seconds;

// Operator levels are linear amplitudes; a modulator at full level shifts
// its target by two cycles, so the skew gives fine control at low indices.
// Envelopes are the classic chip's four rates and four levels on its own
// 0-99 scales (dx_envelope.hpp), with keyboard rate scaling 0-7. Whole
// numbers are the chip's values; the knobs and automation glide between.
#define HALO_ENV(N, X, NAME, DEFAULT)                                                              \
  {"halo.op" N "." X, NAME, "Op " N, "", S::linear, 0, 99, DEFAULT, {}, T::live, g}
#define HALO_OP(N, RATIO, DETUNE, LEVEL, VELOCITY, KEY, R1, R2, R3, R4, L1, L2, L3, L4)             \
  {"halo.op" N ".mode", "Mode", "Op " N, "", S::choice, 0, 1, 0, operator_modes, T::live, 0},       \
      {"halo.op" N ".ratio", "Ratio", "Op " N, "",      S::choice, 0,                               \
       last(ratios.size()),  RATIO,   ratios,  T::live, 0},                                         \
      {"halo.op" N ".fine", "Fine", "Op " N, "%", S::linear, 0, 0.99, 0, {}, T::live, g},           \
      {"halo.op" N ".detune", "Detune", "Op " N, "ct", S::linear, -10, 10, DETUNE, {}, T::live, g}, \
      {"halo.op" N ".fixed",                                                                        \
       "Fixed",                                                                                     \
       "Op " N,                                                                                     \
       "Hz",                                                                                        \
       S::logarithmic,                                                                              \
       1,                                                                                           \
       10'000,                                                                                      \
       440,                                                                                         \
       {},                                                                                          \
       T::live,                                                                                     \
       g},                                                                                          \
      {"halo.op" N ".level",                                                                        \
       "Level",                                                                                     \
       "Op " N,                                                                                     \
       "%",                                                                                         \
       S::linear,                                                                                   \
       0,                                                                                           \
       1,                                                                                           \
       LEVEL,                                                                                       \
       {},                                                                                          \
       T::live,                                                                                     \
       g,                                                                                           \
       true,                                                                                        \
       2},                                                                                          \
      {"halo.op" N ".velocity",                                                                     \
       "Velocity",                                                                                  \
       "Op " N,                                                                                     \
       "%",                                                                                         \
       S::linear,                                                                                   \
       0,                                                                                           \
       1,                                                                                           \
       VELOCITY,                                                                                    \
       {},                                                                                          \
       T::live,                                                                                     \
       g},                                                                                          \
      {"halo.op" N ".key", "Key Scale", "Op " N, "%", S::linear, -1, 1, KEY, {}, T::live, g},       \
      {"halo.op" N ".wave", "Wave", "Op " N, "", S::choice, 0, 3, 0, waves, T::live, 0},            \
      HALO_ENV(N, "r1", "Rate 1", R1), HALO_ENV(N, "r2", "Rate 2", R2),                             \
      HALO_ENV(N, "r3", "Rate 3", R3), HALO_ENV(N, "r4", "Rate 4", R4),                             \
      HALO_ENV(N, "l1", "Level 1", L1), HALO_ENV(N, "l2", "Level 2", L2),                           \
      HALO_ENV(N, "l3", "Level 3", L3), HALO_ENV(N, "l4", "Level 4", L4),                           \
      {"halo.op" N ".rate_scale", "Rate Scale", "Op " N, "", S::stepped, 0, 7, 0, {}, T::live, 0}

inline constexpr std::array<ParameterDescriptor, 6 * operator_parameters + 32> parameters{{
    // E-Piano: three carrier/modulator pairs (algorithm 5); op 4 is the tine.
    HALO_OP("1", 1, 0, 0.85, 0.5, 0, 99, 35, 35, 52, 99, 0, 0, 0),
    HALO_OP("2", 1, 0, 0.2, 0.8, 0.3, 99, 41, 41, 54, 99, 0, 0, 0),
    HALO_OP("3", 1, 3, 0.55, 0.5, 0, 99, 36, 36, 52, 99, 0, 0, 0),
    HALO_OP("4", 14, 0, 0.05, 1, 0.6, 99, 61, 61, 60, 99, 0, 0, 0),
    HALO_OP("5", 1, -3, 0.35, 0.5, 0, 99, 38, 38, 52, 99, 0, 0, 0),
    HALO_OP("6", 1, 0, 0.12, 0.7, 0.3, 99, 44, 44, 57, 99, 0, 0, 0),
    // FM section
    {"halo.algorithm", "Algorithm", "FM", "", S::choice, 0, last(algorithms.size()), 4, algorithms,
     T::live, 0},
    {"halo.feedback", "Feedback", "FM", "%", S::linear, 0, 1, 0.2, {}, T::live, g},
    {"halo.brightness", "Brightness", "FM", "%", S::linear, -1, 1, 0, {}, T::live, g},
    // Filter (optional)
    {"halo.filter.type", "Type", "Filter", "", S::choice, 0, last(filter_types.size()), 0,
     filter_types, T::live, 0},
    {"halo.filter.cutoff",
     "Cutoff",
     "Filter",
     "Hz",
     S::logarithmic,
     20,
     20'000,
     8'000,
     {},
     T::live,
     g},
    {"halo.filter.resonance", "Resonance", "Filter", "%", S::linear, 0, 1, 0.1, {}, T::live, g},
    {"halo.filter.keytrack", "Key", "Filter", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    {"halo.filter.velocity", "Velocity", "Filter", "%", S::linear, 0, 1, 0.2, {}, T::live, g},
    // LFO
    {"halo.lfo.shape", "Shape", "LFO", "", S::choice, 0, last(lfo_shapes.size()), 1, lfo_shapes,
     T::live, 0},
    {"halo.lfo.rate", "Rate", "LFO", "Hz", S::logarithmic, 0.05, 30, 5, {}, T::live, g},
    {"halo.lfo.sync", "Sync", "LFO", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"halo.lfo.division", "Division", "LFO", "", S::choice, 0, last(lfo_divisions.size()), 8,
     lfo_divisions, T::live, 0},
    {"halo.lfo.delay", "Delay", "LFO", "s", S::linear, 0, 5, 0.5, {}, T::live, 0, true, 2},
    {"halo.lfo.pitch", "To Pitch", "LFO", "%", S::linear, 0, 1, 0, {}, T::live, g, true, 2},
    {"halo.lfo.amp", "To Amp", "LFO", "%", S::linear, 0, 1, 0, {}, T::live, g},
    // Voicing and performance
    {"halo.voice.mode", "Mode", "Voice", "", S::choice, 0, 2, 0, voice_modes, T::next_note, 0},
    {"halo.voice.unison", "Unison", "Voice", "", S::stepped, 1, 4, 1, {}, T::live, 0},
    {"halo.voice.detune", "Detune", "Voice", "ct", S::linear, 0, 50, 10, {}, T::live, g, true, 2},
    {"halo.voice.spread", "Spread", "Voice", "%", S::linear, 0, 1, 0.6, {}, T::live, g},
    {"halo.voice.glide", "Glide", "Voice", "s", S::linear, 0, 3, 0, {}, T::live, 0, true, 3},
    {"halo.voice.bend", "Bend Range", "Voice", "st", S::stepped, 0, 12, 2, {}, T::live, 0},
    {"halo.perf.wheel", "Mod Wheel", "Performance", "%", S::linear, 0, 1, 0, {}, T::live, 0.01},
    {"halo.perf.bend", "Pitch Bend", "Performance", "%", S::linear, -1, 1, 0, {}, T::live, 0.01},
    {"halo.perf.pressure", "Pressure", "Performance", "%", S::linear, 0, 1, 0, {}, T::live, 0.01},
    // Effects and output
    {"halo.fx.chorus", "Chorus", "Effects", "", S::choice, 0, 3, 1, chorus_modes, T::live, 0},
    {"halo.fx.ensemble", "Ensemble", "Effects", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"halo.fx.delay", "Delay", "Effects", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"halo.fx.delay_time",
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
    {"halo.fx.delay_feedback", "Feedback", "Effects", "%", S::linear, 0, 0.9, 0.35, {}, T::live, g},
    {"halo.fx.delay_sync", "Sync", "Effects", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"halo.fx.delay_division", "Division", "Effects", "", S::choice, 0, last(lfo_divisions.size()),
     6, lfo_divisions, T::live, 0},
    {"halo.out.level", "Level", "Output", "dB", S::linear, -60, 6, 0, {}, T::live, g},
}};

#undef HALO_OP
#undef HALO_ENV

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

// First parameter of operator n (0-based); the block order matches HALO_OP.
constexpr std::size_t operator_base(std::size_t n) { return n * operator_parameters; }

} // namespace oscillate::native_dsp::synth::halo_table
