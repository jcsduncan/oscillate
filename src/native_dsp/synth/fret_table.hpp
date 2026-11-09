#pragma once

#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/fret_feel.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Fret's parameter table (ADR-0088): a sampled fingered bass. Feel plays
// drawn notes like a bassist; Articulation forces one sound; Tone is a DI
// and amp voicing. Defaults are the "Funk Pocket" preset. Ids are persisted
// and automated: never rename or reuse one. Choices and the next-note
// controls are read when a note starts and are not automatable.
namespace oscillate::native_dsp::synth::fret_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

// In fret::feel::Style order.
inline constexpr std::array styles{"As drawn"sv, "Funk"sv, "Driving"sv, "Laid back"sv};
inline constexpr std::array ghost_modes{"Off"sv, "Auto"sv};
enum GhostMode : std::uint8_t { ghosts_off, ghosts_auto };
// Auto lets Feel choose; the rest force one articulation on every note.
inline constexpr std::array articulations{"Auto"sv, "Pluck"sv, "Staccato"sv, "Ghost"sv,
                                          "Behind the bridge"sv};
enum ArticulationMode : std::uint8_t { auto_articulation, pluck, staccato, ghost, bridge };

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = 0.05;

// Release noise is the library's own voicing at 50 % and twice it at 100 %.
inline constexpr double library_level = 0.5;
inline constexpr double max_slide_ms = 200.0;
inline constexpr double max_tilt_db = 6.0;
inline constexpr double max_tune_cents = 50.0;
inline constexpr double min_level_db = -60.0;
inline constexpr double max_level_db = 6.0;

// The "Funk Pocket" defaults.
inline constexpr double default_dynamics = 20.0;
inline constexpr double default_release = 0.6;
inline constexpr double default_amp = 0.35;
inline constexpr double default_squash = 0.45;

inline constexpr std::array<ParameterDescriptor, 15> parameters{{
    // Feel
    {"fret.feel.style", "Style", "Feel", "", S::choice, 0, last(styles.size()),
     static_cast<double>(fret::feel::Style::funk), styles, T::next_note, 0, false},
    {"fret.feel.amount", "Amount", "Feel", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"fret.feel.dynamics", "Dynamics", "Feel", "", S::linear, 0, fret::feel::max_dynamics,
     default_dynamics, {}, T::live, g},
    {"fret.feel.pocket", "Pocket", "Feel", "ms", S::linear, 0, fret::feel::max_pocket_ms, 0, {},
     T::live, g},
    {"fret.feel.ghosts", "Ghost notes", "Feel", "", S::choice, 0, last(ghost_modes.size()),
     static_cast<double>(ghosts_auto), ghost_modes, T::next_note, 0, false},
    {"fret.feel.humanize", "Humanize", "Feel", "", S::linear, 0, fret::feel::max_humanize, 0, {},
     T::live, g},
    // Articulation
    {"fret.articulation", "Articulation", "Articulation", "", S::choice, 0,
     last(articulations.size()), static_cast<double>(auto_articulation), articulations,
     T::next_note, 0, false},
    {"fret.release", "Release", "Articulation", "%", S::linear, 0, 1, default_release, {},
     T::live, g},
    {"fret.slide", "Slide", "Articulation", "ms", S::linear, 0, max_slide_ms, 0, {}, T::live, g},
    // Tone
    {"fret.tone.amp", "DI ↔ Amp", "Tone", "%", S::linear, 0, 1, default_amp, {}, T::live, g},
    {"fret.tone.tilt", "Warm ↔ Bright", "Tone", "dB", S::linear, -max_tilt_db, max_tilt_db, 0,
     {}, T::live, g},
    {"fret.tone.squash", "Squash", "Tone", "%", S::linear, 0, 1, default_squash, {}, T::live, g},
    {"fret.tone.grit", "Grit", "Tone", "%", S::linear, 0, 1, 0, {}, T::live, g},
    // Output
    {"fret.tune", "Tune", "Output", "ct", S::linear, -max_tune_cents, max_tune_cents, 0, {},
     T::live, g},
    {"fret.level", "Level", "Output", "dB", S::linear, min_level_db, max_level_db, 0, {}, T::live,
     g},
}};

consteval std::size_t index_of(std::string_view id) {
  return parameter_index(parameters, id);
}

static_assert(styles.size() == static_cast<std::size_t>(fret::feel::Style::laid_back) + 1,
              "a style per Feel style, in order");

} // namespace oscillate::native_dsp::synth::fret_table
// clang-format on
