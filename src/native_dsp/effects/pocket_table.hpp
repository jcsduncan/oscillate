#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Pocket v1: kick and bass masking resolver, phase 1 (ADR-0041). Defaults
// are "Clean Low End" without its lookahead. Ids are persisted and automated: never
// rename or reuse one.
namespace oscillate::native_dsp::effects::pocket_table {

using S = synth::ParameterScale;
using T = synth::ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array modes{"Unmask"sv, "Duck"sv, "Pump"sv};
enum Mode : std::uint8_t { unmask, duck, pump };
inline constexpr std::array off_on{"Off"sv, "On"sv};
inline constexpr std::array lookaheads{"Off"sv, "5 ms"sv, "10 ms"sv, "20 ms"sv, "40 ms"sv};
inline constexpr std::array<double, 5> lookahead_ms{0, 5, 10, 20, 40};

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = synth::effect_glide_seconds;

inline constexpr std::array<synth::ParameterDescriptor, 13> parameters{{
    {"pocket.mode", "Mode", "Duck", "", S::choice, 0, last(modes.size()), 0, modes, T::live, 0},
    {"pocket.depth", "Depth", "Duck", "dB", S::linear, 0, 24, 9, {}, T::live, g},
    {"pocket.margin", "Margin", "Duck", "dB", S::linear, 0, 12, 3, {}, T::live, g},
    {"pocket.focus_low", "Focus Low", "Focus", "Hz", S::logarithmic, 20, 200, 30, {}, T::live, g},
    {"pocket.focus_high",
     "Focus High",
     "Focus",
     "Hz",
     S::logarithmic,
     80,
     2'000,
     250,
     {},
     T::live,
     g},
    {"pocket.attack", "Attack", "Timing", "ms", S::logarithmic, 0.1, 30, 2, {}, T::live, g},
    {"pocket.release", "Release", "Timing", "ms", S::logarithmic, 10, 500, 90, {}, T::live, g},
    {"pocket.follow", "Follow Kick", "Timing", "", S::choice, 0, 1, 1, off_on, T::live, 0},
    // Lookahead is a latency: fixed per instance, so a change rebuilds the
    // route (ADR-0040) and it is not automatable. It brings each band's cut
    // into time with the audio despite the analysis's own delay.
    {"pocket.lookahead", "Lookahead", "Timing", "", S::choice, 0, last(lookaheads.size()), 0,
     lookaheads, T::live, 0, false},
    {"pocket.mono_sub", "Mono Sub", "Sub", "", S::choice, 0, 1, 1, off_on, T::live, 0},
    {"pocket.mono_freq", "Below", "Sub", "Hz", S::logarithmic, 40, 250, 120, {}, T::live, g},
    {"pocket.mix", "Mix", "Output", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"pocket.output", "Output", "Output", "dB", S::linear, -24, 12, 0, {}, T::live, g},
}};

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

} // namespace oscillate::native_dsp::effects::pocket_table
