#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/tempo_sync.hpp"

#include <array>
#include <cstddef>
#include <string_view>

// Orbit v1: a modulation effect (ADR-0040). Defaults are the "Classic
// Chorus" preset. Ids are persisted and automated: never rename or reuse one.
namespace oscillate::native_dsp::effects::orbit_table {

using S = synth::ParameterScale;
using T = synth::ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array modes{"Chorus"sv, "Ensemble"sv, "Dimension"sv, "Flanger"sv,
                                  "Phaser"sv, "Vibrato"sv,  "Tremolo"sv};
enum Mode : int { chorus, ensemble, dimension, flanger, phaser, vibrato, tremolo };
inline constexpr std::array shapes{"Sine"sv, "Triangle"sv};
inline constexpr std::array off_on{"Off"sv, "On"sv};
inline constexpr std::array voice_counts{"1"sv, "2"sv, "3"sv, "4"sv};
inline constexpr std::array stage_counts{"2"sv, "4"sv, "6"sv, "8"sv, "12"sv};
inline constexpr std::array<int, 5> stage_values{2, 4, 6, 8, 12};
inline constexpr const auto& divisions = synth::sync_divisions;

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = synth::effect_glide_seconds;

inline constexpr std::array<synth::ParameterDescriptor, 15> parameters{{
    {"orbit.mode", "Mode", "Mode", "", S::choice, 0, last(modes.size()), 0, modes, T::live, 0},
    {"orbit.rate", "Rate", "LFO", "Hz", S::logarithmic, 0.01, 20, 0.6, {}, T::live, g},
    {"orbit.sync", "Sync", "LFO", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"orbit.division", "Division", "LFO", "", S::choice, 0, last(divisions.size()), 8, divisions,
     T::live, 0},
    {"orbit.shape", "Shape", "LFO", "", S::choice, 0, 1, 0, shapes, T::live, 0},
    {"orbit.depth", "Depth", "LFO", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    {"orbit.stereo", "Stereo", "LFO", "deg", S::linear, 0, 180, 90, {}, T::live, g},
    {"orbit.time", "Time", "Voice", "ms", S::logarithmic, 0.1, 30, 12, {}, T::live, g},
    {"orbit.feedback", "Feedback", "Voice", "%", S::linear, -0.95, 0.95, 0, {}, T::live, g},
    {"orbit.voices", "Voices", "Voice", "", S::choice, 0, last(voice_counts.size()), 1,
     voice_counts, T::live, 0},
    {"orbit.stages", "Stages", "Phaser", "", S::choice, 0, last(stage_counts.size()), 1,
     stage_counts, T::live, 0},
    {"orbit.centre", "Centre", "Phaser", "Hz", S::logarithmic, 100, 8'000, 800, {}, T::live, g},
    {"orbit.tone", "Tone", "Output", "dB", S::linear, -18, 0, 0, {}, T::live, g},
    {"orbit.mix", "Mix", "Output", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    {"orbit.output", "Output", "Output", "dB", S::linear, -24, 12, 0, {}, T::live, g},
}};

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

} // namespace oscillate::native_dsp::effects::orbit_table
