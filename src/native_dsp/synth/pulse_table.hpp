#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <cstddef>
#include <string_view>

// Pulse v1 parameter table: twelve pads in key order from C1 (MIDI 36), each
// with the same eight controls, then the kit bus. Ids are persisted and
// automated: never rename or reuse one.
namespace oscillate::native_dsp::synth::pulse_table {

using S = ParameterScale;
using T = ParameterTiming;

inline constexpr std::size_t pad_count = 12;
inline constexpr std::size_t pad_parameters = 8;
inline constexpr std::uint8_t first_key = 36; // C1

inline constexpr double g = 0.02;

// Pad order matches keys C1..B1.
#define PULSE_PAD(ID, NAME, LEVEL, PAN, DECAY, TONE, DRIVE, CHARACTER)                             \
  {"pulse." ID ".level", "Level", NAME, "%", S::linear, 0, 1, LEVEL, {}, T::live, g},              \
      {"pulse." ID ".pan", "Pan", NAME, "%", S::linear, -1, 1, PAN, {}, T::live, g},               \
      {"pulse." ID ".tune", "Tune", NAME, "st", S::linear, -24, 24, 0, {}, T::live, g},            \
      {"pulse." ID ".decay", "Decay", NAME, "s", S::logarithmic, 0.01, 4, DECAY, {}, T::live, g},  \
      {"pulse." ID ".tone", "Tone", NAME, "%", S::linear, 0, 1, TONE, {}, T::live, g},             \
      {"pulse." ID ".drive", "Drive", NAME, "%", S::linear, 0, 1, DRIVE, {}, T::live, g},          \
      {"pulse." ID ".velocity", "Velocity", NAME, "%", S::linear, 0, 1, 0.6, {}, T::live, g}, {    \
    "pulse." ID ".character", "Character", NAME, "%", S::linear, 0, 1, CHARACTER, {}, T::live, g   \
  }

inline constexpr std::array<ParameterDescriptor, pad_count * pad_parameters + 4> parameters{{
    PULSE_PAD("kick", "Kick", 0.9, 0, 0.6, 0.5, 0.1, 0.3),
    PULSE_PAD("kick2", "Kick 2", 0.85, 0, 0.35, 0.6, 0.2, 0.5),
    PULSE_PAD("snare", "Snare", 0.75, 0, 0.25, 0.5, 0.1, 0.6),
    PULSE_PAD("rim", "Rim", 0.6, 0.1, 0.06, 0.5, 0, 0.4),
    PULSE_PAD("clap", "Clap", 0.7, 0, 0.3, 0.5, 0.1, 0.5),
    PULSE_PAD("tom_low", "Low Tom", 0.7, -0.3, 0.45, 0.4, 0, 0.2),
    PULSE_PAD("chh", "Closed Hat", 0.55, 0.15, 0.06, 0.6, 0, 0.2),
    PULSE_PAD("tom_mid", "Mid Tom", 0.7, 0, 0.4, 0.4, 0, 0.2),
    PULSE_PAD("cowbell", "Cowbell", 0.5, 0.2, 0.3, 0.5, 0, 0.5),
    PULSE_PAD("tom_high", "High Tom", 0.7, 0.3, 0.35, 0.4, 0, 0.2),
    PULSE_PAD("ohh", "Open Hat", 0.5, 0.15, 0.5, 0.6, 0, 0.2),
    PULSE_PAD("cymbal", "Cymbal", 0.45, -0.2, 1.6, 0.6, 0, 0.3),
    {"pulse.bus.glue", "Glue", "Bus", "%", S::linear, 0, 1, 0.25, {}, T::live, g},
    {"pulse.bus.release", "Release", "Bus", "s", S::logarithmic, 0.05, 1, 0.2, {}, T::live, g},
    {"pulse.bus.saturation", "Saturation", "Bus", "%", S::linear, 0, 1, 0.1, {}, T::live, g},
    {"pulse.out.level", "Level", "Output", "dB", S::linear, -60, 6, -1, {}, T::live, g},
}};

#undef PULSE_PAD

consteval std::size_t index_of(std::string_view id) { return parameter_index(parameters, id); }

} // namespace oscillate::native_dsp::synth::pulse_table
