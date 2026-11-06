#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <cstddef>
#include <string_view>

// Lens v1: a spectrum analyser (ADR-0042). It passes audio untouched; its
// parameters shape only the analysis. Ids are persisted: never rename or
// reuse one.
namespace oscillate::native_dsp::effects::lens_table {

using S = synth::ParameterScale;
using T = synth::ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array views{"Mid"sv,   "Side"sv,       "Left"sv,
                                  "Right"sv, "Mid / Side"sv, "Left / Right"sv};
enum View : int { mid, side, left, right, mid_side, left_right };
inline constexpr std::array resolutions{"Fast"sv, "Balanced"sv, "Precise"sv};
inline constexpr std::array averages{"Fast"sv, "Medium"sv, "Slow"sv, "Infinite"sv};
inline constexpr std::array<double, 4> average_seconds{0.025, 0.3, 1.5, 0.0};
inline constexpr std::array smoothings{"1/24 oct"sv, "1/12 oct"sv, "1/6 oct"sv, "1/3 oct"sv};
inline constexpr std::array<double, 4> smoothing_octaves{1.0 / 24, 1.0 / 12, 1.0 / 6, 1.0 / 3};
// Points are band powers, so pink noise is flat untilted; a typical mix
// falls about 1.5 dB per octave.
inline constexpr std::array tilts{"0 dB/oct"sv, "1.5 dB/oct"sv, "3 dB/oct"sv};
inline constexpr std::array<double, 3> tilt_db{0.0, 1.5, 3.0};
inline constexpr std::array off_on{"Off"sv, "On"sv};

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }

inline constexpr std::array<synth::ParameterDescriptor, 9> parameters{{
    {"lens.view", "View", "Analysis", "", S::choice, 0, last(views.size()), 0, views, T::live, 0},
    {"lens.resolution", "Resolution", "Analysis", "", S::choice, 0, last(resolutions.size()), 1,
     resolutions, T::live, 0},
    {"lens.average", "Average", "Analysis", "", S::choice, 0, last(averages.size()), 1, averages,
     T::live, 0},
    {"lens.smoothing", "Smoothing", "Analysis", "", S::choice, 0, last(smoothings.size()), 1,
     smoothings, T::live, 0},
    {"lens.tilt", "Tilt", "Display", "", S::choice, 0, last(tilts.size()), 1, tilts, T::live, 0},
    {"lens.hold", "Peak Hold", "Display", "", S::choice, 0, 1, 1, off_on, T::live, 0},
    {"lens.hold_decay", "Hold Decay", "Display", "dB/s", S::linear, 0, 60, 12, {}, T::live, 0},
    {"lens.overlay", "Sidechain", "Display", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"lens.freeze", "Freeze", "Display", "", S::choice, 0, 1, 0, off_on, T::live, 0},
}};

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

} // namespace oscillate::native_dsp::effects::lens_table
