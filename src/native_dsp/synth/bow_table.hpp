#pragma once

#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/bow.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Bow's parameter table (ADR-0111): sampled strings. The section, Solo or
// Section, and the articulation are read when a note starts; the wheel,
// Vibrato, Tone and the outputs play live. Defaults are the "Sul Tasto Pad"
// preset. Ids are persisted and automated: never rename or reuse one.
namespace oscillate::native_dsp::synth::bow_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

// In bow::Section order.
inline constexpr std::array sections{"Violins I"sv, "Violins II"sv, "Violas"sv, "Cellos"sv,
                                     "Basses"sv, "Ensemble"sv};
// In bow::Mode order.
inline constexpr std::array modes{"Solo"sv, "Section"sv};
// In bow::Articulation order.
inline constexpr std::array articulations{"Sustain"sv, "Legato"sv, "Staccato"sv, "Pizzicato"sv,
                                          "Tremolo"sv};

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = synth_glide_seconds;
// The wheel glides a little longer than a knob, so a stepped controller bows.
inline constexpr double wheel_glide_seconds = 0.05;

inline constexpr double default_wheel = 0.42;   // mp.
inline constexpr double default_vibrato = 0.38;
inline constexpr double default_players = 8.0;
inline constexpr double default_humanise = 0.24;
inline constexpr double min_legato_ms = 20.0;
inline constexpr double max_legato_ms = 400.0;
inline constexpr double default_legato_ms = 120.0;
inline constexpr double min_release_seconds = 0.05;
inline constexpr double max_release_seconds = 4.0;
inline constexpr double default_release_seconds = 1.4;
// Tone: 0 sul tasto, 0.5 ordinario, 1 sul ponticello.
inline constexpr double ordinario = 0.5;
inline constexpr double default_tone = 0.22;
inline constexpr double default_space = 0.25;
inline constexpr double max_tune_cents = 50.0;
inline constexpr double min_level_db = -60.0;
inline constexpr double max_level_db = 6.0;

inline constexpr std::array<ParameterDescriptor, 13> parameters{{
    // Ensemble
    {"bow.section", "Section", "Ensemble", "", S::choice, 0, last(sections.size()),
     static_cast<double>(bow::Section::violas), sections, T::next_note, 0, false},
    {"bow.mode", "Players", "Ensemble", "", S::choice, 0, last(modes.size()),
     static_cast<double>(bow::Mode::section), modes, T::next_note, 0, false},
    {"bow.articulation", "Articulation", "Ensemble", "", S::choice, 0, last(articulations.size()),
     static_cast<double>(bow::Articulation::legato), articulations, T::next_note, 0, false},
    // Bow: the mod wheel is the bowing pressure.
    {"bow.perf.wheel", "Dynamics", "Bow", "%", S::linear, 0, 1, default_wheel, {}, T::live,
     wheel_glide_seconds},
    {"bow.vibrato", "Vibrato", "Bow", "%", S::linear, 0, 1, default_vibrato, {}, T::live, g},
    {"bow.tone", "Tone", "Bow", "%", S::linear, 0, 1, default_tone, {}, T::live, g},
    {"bow.players", "Ensemble", "Players", "", S::stepped, bow::min_players, bow::max_players,
     default_players, {}, T::next_note, 0, false},
    {"bow.humanise", "Humanise", "Players", "%", S::linear, 0, 1, default_humanise, {},
     T::next_note, 0, false},
    {"bow.legato", "Legato time", "Players", "ms", S::linear, min_legato_ms, max_legato_ms,
     default_legato_ms, {}, T::live, g},
    {"bow.release", "Release", "Players", "s", S::logarithmic, min_release_seconds,
     max_release_seconds, default_release_seconds, {}, T::live, g},
    // Output
    {"bow.space", "Space", "Output", "%", S::linear, 0, 1, default_space, {}, T::live, g},
    {"bow.tune", "Tune", "Output", "ct", S::linear, -max_tune_cents, max_tune_cents, 0, {},
     T::live, g},
    {"bow.level", "Level", "Output", "dB", S::linear, min_level_db, max_level_db, 0, {}, T::live,
     g},
}};

consteval std::size_t index_of(std::string_view id) {
  return parameter_index(parameters, id);
}

static_assert(sections.size() == bow::section_count, "a name per bow::Section, in order");
static_assert(articulations.size() == bow::articulation_count,
              "a name per bow::Articulation, in order");
static_assert(modes.size() == static_cast<std::size_t>(bow::Mode::section) + 1,
              "a name per bow::Mode, in order");

} // namespace oscillate::native_dsp::synth::bow_table
// clang-format on
