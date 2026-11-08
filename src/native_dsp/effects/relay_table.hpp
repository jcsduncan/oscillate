#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/tempo_sync.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Relay v1: a delay (ADR-0040). Defaults are the "Quarter Echo" preset. Ids
// are persisted and automated: never rename or reuse one.
namespace oscillate::native_dsp::effects::relay_table {

using S = synth::ParameterScale;
using T = synth::ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array characters{"Digital"sv, "Tape"sv, "Analog"sv};
enum Character : std::uint8_t { digital, tape, analog };
inline constexpr std::array routings{"Stereo"sv, "Ping-Pong"sv, "Mono"sv};
enum Routing : std::uint8_t { stereo, ping_pong, mono };
inline constexpr std::array off_on{"Off"sv, "On"sv};
inline constexpr const auto& divisions = synth::sync_divisions;

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = synth::effect_glide_seconds;

inline constexpr std::array<synth::ParameterDescriptor, 17> parameters{{
    {"relay.character", "Character", "Delay", "", S::choice, 0, last(characters.size()), 0,
     characters, T::live, 0},
    {"relay.routing", "Routing", "Delay", "", S::choice, 0, last(routings.size()), 0, routings,
     T::live, 0},
    {"relay.time", "Time", "Delay", "ms", S::logarithmic, 1, 4'000, 375, {}, T::live, 0},
    {"relay.sync", "Sync", "Delay", "", S::choice, 0, 1, 1, off_on, T::live, 0},
    {"relay.division", "Division", "Delay", "", S::choice, 0, last(divisions.size()), 8, divisions,
     T::live, 0},
    {"relay.offset", "Offset", "Delay", "%", S::linear, -0.5, 0.5, 0, {}, T::live, 0},
    {"relay.feedback", "Feedback", "Feedback", "%", S::linear, 0, 1.1, 0.35, {}, T::live, g},
    {"relay.low_cut", "Low Cut", "Feedback", "Hz", S::logarithmic, 20, 2'000, 80, {}, T::live, g},
    {"relay.high_cut",
     "High Cut",
     "Feedback",
     "Hz",
     S::logarithmic,
     1'000,
     20'000,
     9'000,
     {},
     T::live,
     g},
    {"relay.drive", "Drive", "Feedback", "dB", S::linear, 0, 24, 0, {}, T::live, g},
    {"relay.diffusion", "Diffusion", "Feedback", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"relay.mod_depth", "Wow", "Modulation", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"relay.mod_rate", "Rate", "Modulation", "Hz", S::logarithmic, 0.05, 5, 0.5, {}, T::live, g},
    {"relay.ducking", "Ducking", "Output", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"relay.width", "Width", "Output", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"relay.mix", "Mix", "Output", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
    {"relay.output", "Output", "Output", "dB", S::linear, -24, 12, 0, {}, T::live, g},
}};

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

} // namespace oscillate::native_dsp::effects::relay_table
