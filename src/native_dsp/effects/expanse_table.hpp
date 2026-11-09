#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Expanse v1: an algorithmic reverb (ADR-0040). Defaults are the "Concert
// Hall" preset. Ids are persisted and automated: never rename or reuse one.
namespace oscillate::native_dsp::effects::expanse_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array modes{"Room"sv,    "Hall"sv,      "Plate"sv,
                                  "Chamber"sv, "Cathedral"sv, "Cloud"sv};
enum Mode : std::uint8_t { room, hall, plate, chamber, cathedral, cloud };
inline constexpr std::array intervals{"-12"sv, "+5"sv, "+7"sv, "+12"sv, "+19"sv, "+24"sv};
inline constexpr std::array<double, 6> interval_semitones{-12, 5, 7, 12, 19, 24};
inline constexpr std::array off_on{"Off"sv, "On"sv};

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = effect_glide_seconds;
// Size and decay glide slowly: a size change sweeps every line like a real
// space growing, never steps.
inline constexpr double slow = 0.25;

inline constexpr std::array<ParameterDescriptor, 22> parameters{{
    {"expanse.mode", "Mode", "Space", "", S::choice, 0, last(modes.size()), 1, modes, T::live, 0},
    {"expanse.size", "Size", "Space", "%", S::linear, 0, 1, 0.6, {}, T::live, slow},
    {"expanse.decay", "Decay", "Space", "s", S::logarithmic, 0.1, 30, 2.4, {}, T::live, g},
    {"expanse.predelay",
     "Pre-Delay",
     "Space",
     "ms",
     S::linear,
     0,
     500,
     18,
     {},
     T::live,
     g,
     true,
     2},
    {"expanse.low_decay",
     "Low Decay",
     "Damping",
     "x",
     S::logarithmic,
     0.25,
     4,
     1.2,
     {},
     T::live,
     g},
    {"expanse.low_freq",
     "Low Freq",
     "Damping",
     "Hz",
     S::logarithmic,
     50,
     1'000,
     300,
     {},
     T::live,
     g},
    {"expanse.high_decay",
     "High Decay",
     "Damping",
     "x",
     S::logarithmic,
     0.1,
     1.5,
     0.5,
     {},
     T::live,
     g},
    {"expanse.high_freq",
     "High Freq",
     "Damping",
     "Hz",
     S::logarithmic,
     1'000,
     16'000,
     4'500,
     {},
     T::live,
     g},
    {"expanse.early_diffusion",
     "Early Diffusion",
     "Diffusion",
     "%",
     S::linear,
     0,
     1,
     0.7,
     {},
     T::live,
     g},
    {"expanse.late_diffusion",
     "Late Diffusion",
     "Diffusion",
     "%",
     S::linear,
     0,
     1,
     0.6,
     {},
     T::live,
     g},
    {"expanse.mod_depth", "Mod Depth", "Modulation", "%", S::linear, 0, 1, 0.35, {}, T::live, g},
    {"expanse.mod_rate",
     "Mod Rate",
     "Modulation",
     "Hz",
     S::logarithmic,
     0.05,
     5,
     0.6,
     {},
     T::live,
     g},
    {"expanse.early", "Early", "Output", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    {"expanse.late", "Late", "Output", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"expanse.width", "Width", "Output", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"expanse.low_cut", "Low Cut", "Output", "Hz", S::logarithmic, 20, 1'000, 60, {}, T::live, g},
    {"expanse.high_cut",
     "High Cut",
     "Output",
     "Hz",
     S::logarithmic,
     1'000,
     20'000,
     14'000,
     {},
     T::live,
     g},
    {"expanse.shimmer", "Shimmer", "Shimmer", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"expanse.shimmer_pitch", "Interval", "Shimmer", "st", S::choice, 0, last(intervals.size()), 3,
     intervals, T::live, 0},
    {"expanse.freeze", "Freeze", "Space", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"expanse.mix", "Mix", "Output", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
    {"expanse.output", "Output", "Output", "dB", S::linear, -24, 12, 0, {}, T::live, g},
}};

consteval std::size_t index_of(std::string_view id) { return parameter_index(parameters, id); }

} // namespace oscillate::native_dsp::effects::expanse_table
