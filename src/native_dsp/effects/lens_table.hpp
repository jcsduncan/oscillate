#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Lens v1: a spectrum analyser (ADR-0042) with match EQ (ADR-0120). It
// passes audio untouched; its parameters shape only the analysis and what
// it proposes. Ids are persisted: never rename or
// reuse one.
namespace oscillate::native_dsp::effects::lens_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array views{"Mid"sv,   "Side"sv,       "Left"sv,
                                  "Right"sv, "Mid / Side"sv, "Left / Right"sv};
enum View : std::uint8_t { mid, side, left, right, mid_side, left_right };
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
// Match EQ (ADR-0120): what the mix is matched to, and the smoothing of the
// correction (the half-power width of its Hann window).
inline constexpr std::array match_sources{"Sidechain"sv, "Target"sv};
enum MatchSource : std::uint8_t { sidechain, target };
inline constexpr std::array match_smoothings{"1/6 oct"sv, "1/3 oct"sv, "1/2 oct"sv, "1 oct"sv};
inline constexpr std::array<double, 4> match_smoothing_octaves{1.0 / 6, 1.0 / 3, 1.0 / 2, 1.0};

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }

// The match settings shape only what Lens proposes; they are not automated.
inline constexpr bool fixed = false;
inline constexpr double no_glide = 0.0;

inline constexpr std::array<ParameterDescriptor, 16> parameters{{
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
    {"lens.match_source", "Match To", "Match", "", S::choice, 0, last(match_sources.size()),
     MatchSource::sidechain, match_sources, T::live, no_glide, fixed},
    {"lens.match_amount", "Amount", "Match", "%", S::linear, 0, 1, 1, {}, T::live, no_glide, fixed},
    {"lens.match_smoothing", "Smoothing", "Match", "", S::choice, 0, last(match_smoothings.size()),
     1, match_smoothings, T::live, no_glide, fixed},
    {"lens.match_low",
     "Low Limit",
     "Match",
     "Hz",
     S::logarithmic,
     20,
     1'000,
     30,
     {},
     T::live,
     no_glide,
     fixed},
    {"lens.match_high",
     "High Limit",
     "Match",
     "Hz",
     S::logarithmic,
     1'000,
     20'000,
     16'000,
     {},
     T::live,
     no_glide,
     fixed},
    {"lens.match_boost",
     "Max Boost",
     "Match",
     "dB",
     S::linear,
     0,
     12,
     6,
     {},
     T::live,
     no_glide,
     fixed},
    {"lens.match_cut", "Max Cut", "Match", "dB", S::linear, 0, 12, 6, {}, T::live, no_glide, fixed},
}};

consteval std::size_t index_of(std::string_view id) { return parameter_index(parameters, id); }

} // namespace oscillate::native_dsp::effects::lens_table
