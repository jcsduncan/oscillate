#pragma once

#include "fret_table.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/fret_feel.hpp"

#include <array>
#include <string_view>
#include <utility>

// Fret's factory presets (ADR-0088): only values that differ from the table
// defaults (the "Funk Pocket"). Choices are 0-based, in the table's order.
namespace oscillate::native_dsp::synth::fret_presets {

using V = std::pair<std::string_view, double>;
using namespace std::string_view_literals;
namespace ft = fret_table;

constexpr double style(fret::feel::Style s) { return static_cast<double>(s); }
constexpr double articulation(ft::ArticulationMode a) { return static_cast<double>(a); }

// The samples as recorded: notes as drawn, no tone stage.
inline constexpr std::array straight_di{
    V{"fret.feel.style", style(fret::feel::Style::as_drawn)},
    V{"fret.tone.amp", 0.0},
    V{"fret.tone.squash", 0.0},
    V{"fret.release", ft::library_level},
};

// Every eighth up, more amp and a little hair.
inline constexpr std::array driving_rock{
    V{"fret.feel.style", style(fret::feel::Style::driving)},
    V{"fret.feel.dynamics", 16.0},
    V{"fret.tone.amp", 0.6},
    V{"fret.tone.squash", 0.6},
    V{"fret.tone.grit", 0.2},
};

// Behind the beat, warm and round, with slides.
inline constexpr std::array laid_back_soul{
    V{"fret.feel.style", style(fret::feel::Style::laid_back)},
    V{"fret.feel.pocket", 8.0},
    V{"fret.feel.humanize", 4.0},
    V{"fret.slide", 60.0},
    V{"fret.tone.tilt", -2.5},
    V{"fret.tone.amp", 0.45},
};

// A palm-muted thumb part: every note a ghost.
inline constexpr std::array muted_thumb{
    V{"fret.articulation", articulation(ft::ghost)},
    V{"fret.tone.amp", 0.5},
    V{"fret.tone.tilt", -1.5},
    V{"fret.level", 4.0},
};

// Plucked by the bridge: tight and nasal, driven.
inline constexpr std::array bridge_bite{
    V{"fret.articulation", articulation(ft::bridge)},
    V{"fret.tone.grit", 0.3},
    V{"fret.tone.tilt", 1.5},
    V{"fret.tone.squash", 0.55},
};

inline constexpr std::array funk_tags{"bass"sv, "funk"sv, "fingered"sv};
inline constexpr std::array clean_tags{"bass"sv, "clean"sv, "di"sv};
inline constexpr std::array rock_tags{"bass"sv, "rock"sv, "driven"sv};
inline constexpr std::array soul_tags{"bass"sv, "soul"sv, "warm"sv};
inline constexpr std::array muted_tags{"bass"sv, "muted"sv, "thumb"sv};
inline constexpr std::array bridge_tags{"bass"sv, "bridge"sv, "bite"sv};

inline constexpr std::array<Preset, 6> presets{{
    {"Funk Pocket", "Bass", {}, funk_tags},
    {"Straight DI", "Bass", straight_di, clean_tags},
    {"Driving Rock", "Bass", driving_rock, rock_tags},
    {"Laid Back Soul", "Bass", laid_back_soul, soul_tags},
    {"Muted Thumb", "Bass", muted_thumb, muted_tags},
    {"Bridge Bite", "Bass", bridge_bite, bridge_tags},
}};

} // namespace oscillate::native_dsp::synth::fret_presets
