#pragma once

#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/stick.hpp"
#include "stick_table.hpp"

#include <array>
#include <string_view>
#include <utility>

// Stick's factory presets (ADR-0114): only values that differ from the
// table defaults (the "Tight Funk" kit: DRS Studio, the studio mix, the kick
// and snare shortened, a little humanised). Restrained kits for house, funk
// and lo-fi; nothing is processed beyond what the controls do.
namespace oscillate::native_dsp::synth::stick_presets {

using V = std::pair<std::string_view, double>;
using namespace std::string_view_literals;

constexpr double kit(stick::Kit k) { return static_cast<double>(k); }
constexpr double mix(stick::Mix m) { return static_cast<double>(m); }
constexpr double off = 0.0;
constexpr double on = 1.0;

// Close mics only, no bleed: a tight, dry kit for chopped breaks.
inline constexpr std::array dry_break{
    V{"stick.mix", mix(stick::Mix::dry)}, V{"stick.bleed", off},        V{"stick.humanise", 0.1},
    V{"stick.kick.decay", 0.35},          V{"stick.snare.decay", 0.45}, V{"stick.snare.level", 2.0},
    V{"stick.tom1.decay", 0.5},           V{"stick.tom2.decay", 0.5},   V{"stick.tom3.decay", 0.5},
    V{"stick.tom4.decay", 0.5},
};

// The rock kit in its room, every hit ringing out.
inline constexpr std::array room_kit{
    V{"stick.kit", kit(stick::Kit::muldjord)},
    V{"stick.mix", mix(stick::Mix::room)},
    V{"stick.humanise", 0.22},
    V{"stick.kick.decay", 1.0},
    V{"stick.snare.decay", 1.0},
    V{"stick.tom1.decay", 1.0},
    V{"stick.tom2.decay", 1.0},
    V{"stick.tom3.decay", 1.0},
    V{"stick.tom4.decay", 1.0},
};

// The small kit, raw, its snare a semitone down.
inline constexpr std::array garage_lofi{
    V{"stick.kit", kit(stick::Kit::garage)},
    V{"stick.mix", mix(stick::Mix::room)},
    V{"stick.humanise", 0.3},
    V{"stick.kick.decay", 0.6},
    V{"stick.snare.tune", -1.0},
    V{"stick.snare.decay", 0.7},
    V{"stick.hihat.decay", 0.8},
    V{"stick.level", -2.0},
};

// The studio kit played with brushes (its whisker recordings), light
// touches lifted.
inline constexpr std::array brushy_soft{
    V{"stick.striker", static_cast<double>(stick::Striker::brush)},
    V{"stick.humanise", 0.25},
    V{"stick.snare.decay", 1.0},
    V{"stick.snare.curve", -0.4},
    V{"stick.hihat.curve", -0.4},
    V{"stick.ride.curve", -0.4},
    V{"stick.kick.level", -4.0},
    V{"stick.level", -1.0},
};

// The studio kit as the mics heard it: nothing shortened, nothing moved.
inline constexpr std::array as_recorded{
    V{"stick.humanise", 0.0},   V{"stick.kick.decay", 1.0}, V{"stick.snare.decay", 1.0},
    V{"stick.tom1.decay", 1.0}, V{"stick.tom2.decay", 1.0}, V{"stick.tom3.decay", 1.0},
    V{"stick.tom4.decay", 1.0},
};

inline constexpr std::array funk_tags{"drums"sv, "acoustic"sv, "funk"sv};
inline constexpr std::array break_tags{"drums"sv, "acoustic"sv, "dry"sv};
inline constexpr std::array room_tags{"drums"sv, "acoustic"sv, "room"sv};
inline constexpr std::array lofi_tags{"drums"sv, "acoustic"sv, "lo-fi"sv};
inline constexpr std::array brush_tags{"drums"sv, "acoustic"sv, "brushes"sv};
inline constexpr std::array recorded_tags{"drums"sv, "acoustic"sv, "natural"sv};

inline constexpr std::array<Preset, 6> presets{{
    {"Tight Funk", "Drums", {}, funk_tags},
    {"Dry Break", "Drums", dry_break, break_tags},
    {"Room Kit", "Drums", room_kit, room_tags},
    {"Garage Lo-Fi", "Drums", garage_lofi, lofi_tags},
    {"Brushy Soft", "Drums", brushy_soft, brush_tags},
    {"As Recorded", "Drums", as_recorded, recorded_tags},
}};

} // namespace oscillate::native_dsp::synth::stick_presets
