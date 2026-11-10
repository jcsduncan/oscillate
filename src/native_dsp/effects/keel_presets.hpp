#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Keel presets: only values that differ from the table defaults. Each stays
// within 4 dB either way and holds the loudness with Level Match, measured
// in keel_tests.cpp (presets_stay_restrained). Targets are 0-based choice
// indices (keel_table::targets).
namespace oscillate::native_dsp::effects::keel_presets {

using V = std::pair<std::string_view, double>;
using namespace std::string_view_literals;

// A slow, light pull toward the balance: two decibels at most.
inline constexpr std::array gentle_polish{V{"keel.amount", 0.35}, V{"keel.speed", 10},
                                          V{"keel.max_boost", 2}, V{"keel.max_cut", 2}};
// Only the top end, and only down: a mix brighter than the target is
// tamed, a dull one left alone.
inline constexpr std::array tame_harsh{
    V{"keel.amount", 0.7}, V{"keel.max_boost", 0},      V{"keel.max_cut", 4}, V{"keel.low", 0},
    V{"keel.mid", 0.3},    V{"keel.high_split", 3'000}, V{"keel.speed", 3}};
// A darker target, the lows lifted and the top eased, within 3 dB.
inline constexpr std::array warm_it_up{V{"keel.tilt", -3.0}, V{"keel.max_boost", 3},
                                       V{"keel.max_cut", 3}, V{"keel.speed", 6}};
// The whole range toward the target within 3 dB, slowly, at equal loudness.
inline constexpr std::array streaming_balance{V{"keel.speed", 8}, V{"keel.max_boost", 3},
                                              V{"keel.max_cut", 3}};
// The low end alone held to the target: bass and kick balance from track
// to track.
inline constexpr std::array low_end_control{V{"keel.amount", 0.6}, V{"keel.mid", 0},
                                            V{"keel.high", 0}, V{"keel.low_split", 250},
                                            V{"keel.speed", 6}};

inline constexpr std::array polish_tags{"master"sv, "gentle"sv, "balance"sv};
inline constexpr std::array harsh_tags{"master"sv, "harsh"sv, "bright"sv, "top"sv};
inline constexpr std::array warm_tags{"master"sv, "warm"sv, "dark"sv, "balance"sv};
inline constexpr std::array streaming_tags{"master"sv, "streaming"sv, "balance"sv};
inline constexpr std::array low_tags{"master"sv, "low"sv, "bass"sv, "kick"sv};

inline constexpr std::array<Preset, 6> presets{{
    {"Default", "Init", {}},
    {"Gentle Polish", "Master", gentle_polish, polish_tags},
    {"Tame Harsh", "Master", tame_harsh, harsh_tags},
    {"Warm It Up", "Master", warm_it_up, warm_tags},
    {"Streaming Balance", "Master", streaming_balance, streaming_tags},
    {"Low End Control", "Master", low_end_control, low_tags},
}};

} // namespace oscillate::native_dsp::effects::keel_presets
