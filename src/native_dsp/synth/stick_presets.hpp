#pragma once

#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/stick.hpp"
#include "stick_table.hpp"

#include <array>
#include <string_view>
#include <utility>

// Stick's factory presets (ADR-0114, ADR-0121): only values that differ
// from the table defaults (the "Natural" kit: Naked Drums as its SFZ mixes
// it, nothing shortened). Restrained kits; nothing is processed beyond what
// the controls do.
namespace oscillate::native_dsp::synth::stick_presets {

using V = std::pair<std::string_view, double>;
using namespace std::string_view_literals;

constexpr double kit(stick::Kit k) { return static_cast<double>(k); }
constexpr double mix(stick::Mix m) { return static_cast<double>(m); }

// Naked, the close mics with the overheads down and no rooms, each drum
// shortened: a tight, dry kit.
inline constexpr std::array close_mics{
    V{"stick.mix", mix(stick::Mix::dry)}, V{"stick.humanise", 0.08},  V{"stick.kick.decay", 0.45},
    V{"stick.snare.decay", 0.55},         V{"stick.tom1.decay", 0.6}, V{"stick.tom2.decay", 0.6},
    V{"stick.tom3.decay", 0.6},           V{"stick.tom4.decay", 0.6}, V{"stick.tom5.decay", 0.6},
};

// Naked in its rooms: the close mics down, the rooms up.
inline constexpr std::array rooms{
    V{"stick.mix", mix(stick::Mix::room)},
    V{"stick.humanise", 0.12},
};

// DRS Studio, mixed as DrumGizmo's kit sits best: its kick's back mic,
// snare bottom, toms, hi-hat, ride and room down, its cymbals spread.
inline constexpr std::array drs_studio{
    V{"stick.kit", kit(stick::Kit::drs)},
    V{"stick.humanise", 0.15},
    V{"stick.mic.kick_out.level", -3.0},
    V{"stick.mic.snare_bottom.level", -6.0},
    V{"stick.mic.toms.level", -2.0},
    V{"stick.mic.hihat.level", -6.0},
    V{"stick.mic.ride.level", -6.0},
    V{"stick.mic.room.level", -6.0},
    V{"stick.hihat.pan", -0.45},
    V{"stick.tom1.pan", -0.2},
    V{"stick.tom2.pan", 0.1},
    V{"stick.tom3.pan", 0.35},
    V{"stick.crash1.pan", -0.5},
    V{"stick.crash2.pan", 0.45},
    V{"stick.ride.pan", 0.55},
};

// DRS Studio played with brushes (its whisker recordings), light touches
// lifted.
inline constexpr std::array brushes{
    V{"stick.kit", kit(stick::Kit::drs)},
    V{"stick.striker", static_cast<double>(stick::Striker::brush)},
    V{"stick.humanise", 0.25},
    V{"stick.mic.kick_out.level", -3.0},
    V{"stick.mic.snare_bottom.level", -6.0},
    V{"stick.mic.hihat.level", -6.0},
    V{"stick.mic.ride.level", -6.0},
    V{"stick.mic.room.level", -6.0},
    V{"stick.hihat.pan", -0.45},
    V{"stick.ride.pan", 0.55},
    V{"stick.snare.curve", -0.4},
    V{"stick.hihat.curve", -0.4},
    V{"stick.ride.curve", -0.4},
    V{"stick.kick.level", -4.0},
    V{"stick.level", -1.0},
};

inline constexpr std::array natural_tags{"drums"sv, "acoustic"sv, "natural"sv};
inline constexpr std::array close_tags{"drums"sv, "acoustic"sv, "dry"sv};
inline constexpr std::array room_tags{"drums"sv, "acoustic"sv, "room"sv};
inline constexpr std::array drs_tags{"drums"sv, "acoustic"sv, "maple"sv};
inline constexpr std::array brush_tags{"drums"sv, "acoustic"sv, "brushes"sv};

inline constexpr std::array<Preset, 5> presets{{
    {"Natural", "Drums", {}, natural_tags},
    {"Close Mics", "Drums", close_mics, close_tags},
    {"Rooms", "Drums", rooms, room_tags},
    {"DRS Studio", "Drums", drs_studio, drs_tags},
    {"Brushes", "Drums", brushes, brush_tags},
}};

} // namespace oscillate::native_dsp::synth::stick_presets
