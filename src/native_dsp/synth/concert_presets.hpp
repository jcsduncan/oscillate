#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Concert's factory presets: one sampled grand voiced several ways, only
// values that differ from the table defaults (the "Concert Grand").
// Perspective is 0-based (0 Player, 1 Audience). Golden fingerprints, taken
// on the test library, pin each.
namespace oscillate::native_dsp::synth::concert_presets {

using V = std::pair<std::string_view, double>;
using namespace std::string_view_literals;

// Harder voicing and a quicker rise into the loud layers.
inline constexpr std::array bright{
    V{"concert.tone", 3.0},
    V{"concert.touch", 0.8},
    V{"concert.dynamics", 1.15},
};

// A gentle touch: the soft layers for longer and a narrower range.
inline constexpr std::array soft{
    V{"concert.tone", -1.5},
    V{"concert.touch", 1.35},
    V{"concert.dynamics", 0.75},
    V{"concert.noise.release", 0.35},
};

// The player's bench: wider, with more of the mechanism.
inline constexpr std::array close{
    V{"concert.width", 1.3},
    V{"concert.noise.release", 0.7},
    V{"concert.noise.strings", 0.65},
    V{"concert.noise.pedal", 0.7},
    V{"concert.resonance", 0.4},
};

// Darker and hushed, like a felt strip over the strings.
inline constexpr std::array felt{
    V{"concert.tone", -6.0},       V{"concert.touch", 1.4},       V{"concert.dynamics", 0.6},
    V{"concert.noise.release", 0.8}, V{"concert.resonance", 0.2}, V{"concert.width", 0.8},
    V{"concert.level", 3.0},
};

// From the hall: mirrored and narrower.
inline constexpr std::array audience{
    V{"concert.perspective", 1.0},
    V{"concert.width", 0.75},
    V{"concert.noise.release", 0.3},
    V{"concert.noise.pedal", 0.3},
};

// The samples alone: no mechanism, no added resonance.
inline constexpr std::array dry{
    V{"concert.resonance", 0.0},     V{"concert.noise.release", 0.0},
    V{"concert.noise.strings", 0.0}, V{"concert.noise.pedal", 0.0},
};

inline constexpr std::array grand_tags{"piano"sv, "grand"sv, "acoustic"sv};
inline constexpr std::array bright_tags{"piano"sv, "bright"sv, "pop"sv};
inline constexpr std::array soft_tags{"piano"sv, "soft"sv, "intimate"sv};
inline constexpr std::array felt_tags{"piano"sv, "felt"sv, "dark"sv};

inline constexpr std::array<Preset, 7> presets{{
    {"Concert Grand", "Grand", {}, grand_tags},
    {"Bright", "Grand", bright, bright_tags},
    {"Soft", "Grand", soft, soft_tags},
    {"Close", "Grand", close, grand_tags},
    {"Felt", "Felt", felt, felt_tags},
    {"Audience", "Grand", audience, grand_tags},
    {"Dry", "Grand", dry, grand_tags},
}};

} // namespace oscillate::native_dsp::synth::concert_presets
// clang-format on
