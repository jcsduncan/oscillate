#pragma once

#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/wren_breath.hpp"
#include "wren_table.hpp"

#include <array>
#include <string_view>
#include <utility>

// Wren's factory presets (ADR-0089): only values that differ from the table
// defaults (the "Lyrical Air": Lyrical, Ornaments Some, Space 30 %).
// Choices are 0-based, in the table's order.
namespace oscillate::native_dsp::synth::wren_presets {

using V = std::pair<std::string_view, double>;
using namespace std::string_view_literals;
namespace wt = wren_table;
namespace b = wren::breath;

constexpr double style(b::Style s) { return static_cast<double>(s); }
constexpr double ornaments(b::Ornaments o) { return static_cast<double>(o); }
constexpr double articulation(b::Articulation a) { return static_cast<double>(a); }

// Airy, with an ornament on most long notes after a step.
inline constexpr std::array folk_breeze{
    V{"wren.breath.style", style(b::Style::airy)},
    V{"wren.breath.ornaments", ornaments(b::Ornaments::lots)},
};

// The recordings as drawn, dry.
inline constexpr std::array plain_flute{
    V{"wren.breath.style", style(b::Style::as_drawn)},
    V{"wren.tone.space", 0.0},
};

// Lyrical, breathier and further back.
inline constexpr std::array breathy_pad_lead{
    V{"wren.tone.air", 4.0},
    V{"wren.tone.space", 0.6},
};

// Every note short and light, a little uneven.
inline constexpr std::array staccato_birds{
    V{"wren.articulation", articulation(b::Articulation::staccato)},
    V{"wren.breath.humanize", 4.0},
};

// Two flutes: every note tongued, chords allowed.
inline constexpr std::array two_flutes{
    V{"wren.voices", static_cast<double>(wt::poly)},
};

inline constexpr std::array lyrical_tags{"flute"sv, "lyrical"sv, "lead"sv};
inline constexpr std::array folk_tags{"flute"sv, "folk"sv, "ornaments"sv};
inline constexpr std::array plain_tags{"flute"sv, "dry"sv, "plain"sv};
inline constexpr std::array pad_tags{"flute"sv, "breathy"sv, "pad"sv};
inline constexpr std::array birds_tags{"flute"sv, "staccato"sv, "short"sv};
inline constexpr std::array duet_tags{"flute"sv, "poly"sv, "duet"sv};

inline constexpr std::array<Preset, 6> presets{{
    {"Lyrical Air", "Flute", {}, lyrical_tags},
    {"Folk Breeze", "Flute", folk_breeze, folk_tags},
    {"Plain Flute", "Flute", plain_flute, plain_tags},
    {"Breathy Pad Lead", "Flute", breathy_pad_lead, pad_tags},
    {"Staccato Birds", "Flute", staccato_birds, birds_tags},
    {"Two Flutes", "Flute", two_flutes, duet_tags},
}};

} // namespace oscillate::native_dsp::synth::wren_presets
