#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/wren_breath.hpp"

#include <array>
#include <cstddef>
#include <string_view>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Wren's parameter table (ADR-0089): a sampled concert flute. Breath plays
// drawn notes like a flautist; Articulation sets how notes join; Tone is
// Air, Warmth and Space. Defaults are the "Lyrical Air" preset. Ids are
// persisted and automated: never rename or reuse one. Choices and the
// next-note controls are read when a note starts and are not automatable;
// Voices changes when no note sounds.
namespace oscillate::native_dsp::synth::wren_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

// In wren::breath::Style order.
inline constexpr std::array styles{"As drawn"sv, "Lyrical"sv, "Airy"sv};
// In wren::breath::Ornaments order.
inline constexpr std::array ornaments{"Off"sv, "Some"sv, "Lots"sv};
// In wren::breath::Articulation order.
inline constexpr std::array articulations{"Auto"sv, "Legato"sv, "Tongued"sv, "Staccato"sv};
inline constexpr std::array voice_modes{"Mono"sv, "Poly 4"sv};
enum VoiceMode : int { mono, poly };

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = 0.05;

inline constexpr double min_slur_ms = 10.0;
inline constexpr double max_slur_ms = 150.0;
inline constexpr double default_slur_ms = 45.0;
inline constexpr double max_air_db = 6.0;
inline constexpr double max_warmth_db = 4.0;
inline constexpr double default_space = 0.3;
inline constexpr double max_tune_cents = 50.0;
inline constexpr double min_level_db = -60.0;
inline constexpr double max_level_db = 6.0;

namespace b = wren::breath;

inline constexpr std::array<ParameterDescriptor, 16> parameters{{
    // Breath
    {"wren.breath.style", "Style", "Breath", "", S::choice, 0, last(styles.size()),
     static_cast<double>(b::Style::lyrical), styles, T::next_note, 0, false},
    {"wren.breath.amount", "Amount", "Breath", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"wren.breath.dynamics", "Dynamics", "Breath", "", S::linear, 0, b::max_dynamics,
     b::default_dynamics, {}, T::live, g},
    {"wren.breath.vibrato", "Vibrato", "Breath", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"wren.breath.bloom", "Bloom", "Breath", "ms", S::linear, b::min_bloom_ms, b::max_bloom_ms,
     b::default_bloom_ms, {}, T::next_note, 0, false},
    {"wren.breath.swell", "Swell", "Breath", "dB", S::linear, 0, b::max_swell_db,
     b::default_swell_db, {}, T::next_note, 0, false},
    {"wren.breath.ornaments", "Ornaments", "Breath", "", S::choice, 0, last(ornaments.size()),
     static_cast<double>(b::Ornaments::some), ornaments, T::next_note, 0, false},
    {"wren.breath.humanize", "Humanize", "Breath", "", S::linear, 0, b::max_humanize, 0, {},
     T::live, g},
    // Articulation
    {"wren.articulation", "Articulation", "Articulation", "", S::choice, 0,
     last(articulations.size()), static_cast<double>(b::Articulation::automatic), articulations,
     T::next_note, 0, false},
    {"wren.slur", "Slur time", "Articulation", "ms", S::linear, min_slur_ms, max_slur_ms,
     default_slur_ms, {}, T::live, g},
    {"wren.voices", "Voices", "Articulation", "", S::choice, 0, last(voice_modes.size()),
     static_cast<double>(mono), voice_modes, T::next_note, 0, false},
    // Tone
    {"wren.tone.air", "Air", "Tone", "dB", S::linear, -max_air_db, max_air_db, 0, {}, T::live, g},
    {"wren.tone.warmth", "Warmth", "Tone", "dB", S::linear, -max_warmth_db, max_warmth_db, 0, {},
     T::live, g},
    {"wren.tone.space", "Space", "Tone", "%", S::linear, 0, 1, default_space, {}, T::live, g},
    // Output
    {"wren.tune", "Tune", "Output", "ct", S::linear, -max_tune_cents, max_tune_cents, 0, {},
     T::live, g},
    {"wren.level", "Level", "Output", "dB", S::linear, min_level_db, max_level_db, 0, {}, T::live,
     g},
}};

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

static_assert(styles.size() == static_cast<std::size_t>(b::Style::airy) + 1,
              "a style per Breath style, in order");
static_assert(ornaments.size() == static_cast<std::size_t>(b::Ornaments::lots) + 1,
              "an ornament mode per Breath mode, in order");
static_assert(articulations.size() == static_cast<std::size_t>(b::Articulation::staccato) + 1,
              "an articulation per Breath articulation, in order");

} // namespace oscillate::native_dsp::synth::wren_table
// clang-format on
