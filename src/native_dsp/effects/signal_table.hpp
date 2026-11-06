#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <cstddef>
#include <string_view>

// Signal v1: a test generator (ADR-0042). Off passes the input bit-exact.
// Ids are persisted: never rename or reuse one.
namespace oscillate::native_dsp::effects::signal_table {

using S = synth::ParameterScale;
using T = synth::ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array waves{"Sine"sv, "Log Sweep"sv, "Pink Noise"sv, "White Noise"sv,
                                  "Impulses"sv};
enum Wave : int { sine, sweep, pink, white, impulses };
inline constexpr std::array modes{"Replace"sv, "Add"sv};
inline constexpr std::array outputs{"Both"sv, "Left"sv, "Right"sv, "Alternate"sv,
                                    "Right Inverted"sv};
enum Output : int { both, left, right, alternate, inverted };
inline constexpr std::array off_on{"Off"sv, "On"sv};

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }

inline constexpr std::array<synth::ParameterDescriptor, 8> parameters{{
    {"signal.on", "On", "Signal", "", S::choice, 0, 1, 1, off_on, T::live, 0},
    {"signal.wave", "Wave", "Signal", "", S::choice, 0, last(waves.size()), 0, waves, T::live, 0},
    {"signal.frequency",
     "Frequency",
     "Signal",
     "Hz",
     S::logarithmic,
     20,
     20'000,
     1'000,
     {},
     T::live,
     0},
    // Peak for sines, sweeps and impulses; RMS for noise.
    {"signal.level", "Level", "Signal", "dBFS", S::linear, -60, 0, -18, {}, T::live, 0.03},
    {"signal.sweep_seconds", "Sweep Time", "Sweep", "s", S::logarithmic, 1, 60, 10, {}, T::live, 0},
    {"signal.impulse_rate", "Rate", "Impulses", "Hz", S::logarithmic, 0.5, 20, 1, {}, T::live, 0},
    {"signal.mode", "Mode", "Routing", "", S::choice, 0, 1, 0, modes, T::live, 0},
    {"signal.output", "Output", "Routing", "", S::choice, 0, last(outputs.size()), 0, outputs,
     T::live, 0},
}};

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

} // namespace oscillate::native_dsp::effects::signal_table
