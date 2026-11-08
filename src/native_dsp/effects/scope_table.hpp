#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Scope v1: a tempo-locked oscilloscope and alignment meter (ADR-0042). It
// passes audio untouched. Ids are persisted: never rename or reuse one.
namespace oscillate::native_dsp::effects::scope_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array lengths{"1/16"sv,  "1/8"sv,    "1/4"sv,   "1/2"sv,
                                    "1 Bar"sv, "2 Bars"sv, "4 Bars"sv};
// Quarter-note beats, or bars from index 4.
inline constexpr std::array<double, 7> length_values{0.25, 0.5, 1.0, 2.0, 1.0, 2.0, 4.0};
inline constexpr std::size_t first_bar_length = 4;
inline constexpr std::array channels{"Mid"sv, "Side"sv, "Left"sv, "Right"sv};
enum Channel : std::uint8_t { mid, side, left, right };
inline constexpr std::array off_on{"Off"sv, "On"sv};
inline constexpr std::array align_bands{"Full"sv, "Low (< 200 Hz)"sv};

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }

inline constexpr std::array<ParameterDescriptor, 7> parameters{{
    {"scope.sync", "Sync", "Window", "", S::choice, 0, 1, 1, off_on, T::live, 0},
    {"scope.length", "Length", "Window", "", S::choice, 0, last(lengths.size()), 2, lengths,
     T::live, 0},
    {"scope.time", "Time", "Window", "ms", S::logarithmic, 1, 4'000, 50, {}, T::live, 0},
    {"scope.channel", "Channel", "Display", "", S::choice, 0, last(channels.size()), 0, channels,
     T::live, 0},
    {"scope.overlay", "Sidechain", "Display", "", S::choice, 0, 1, 1, off_on, T::live, 0},
    {"scope.align", "Align", "Alignment", "", S::choice, 0, 1, 1, off_on, T::live, 0},
    {"scope.align_band", "Band", "Alignment", "", S::choice, 0, last(align_bands.size()), 1,
     align_bands, T::live, 0},
}};

consteval std::size_t index_of(std::string_view id) { return parameter_index(parameters, id); }

} // namespace oscillate::native_dsp::effects::scope_table
