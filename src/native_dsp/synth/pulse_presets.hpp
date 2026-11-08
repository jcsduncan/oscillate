#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Pulse kits: only values that differ from the table defaults, which are the
// "808 Classic" kit. Golden fingerprints pin each.
namespace oscillate::native_dsp::synth::pulse_presets {

using V = std::pair<std::string_view, double>;

inline constexpr std::array boom{
    V{"pulse.kick.decay", 1.8}, V{"pulse.kick.tone", 0.35}, V{"pulse.kick.drive", 0.35},
    V{"pulse.kick.tune", -2},   V{"pulse.snare.tone", 0.3}, V{"pulse.clap.decay", 0.4},
};

inline constexpr std::array house{
    V{"pulse.kick.decay", 0.45},     V{"pulse.kick.tone", 0.7},   V{"pulse.kick.character", 0.6},
    V{"pulse.kick.drive", 0.25},     V{"pulse.kick2.level", 0.9}, V{"pulse.snare.tone", 0.7},
    V{"pulse.snare.character", 0.8}, V{"pulse.snare.decay", 0.2}, V{"pulse.chh.tone", 0.8},
    V{"pulse.chh.character", 0.6},   V{"pulse.ohh.decay", 0.35},  V{"pulse.ohh.character", 0.6},
    V{"pulse.clap.decay", 0.25},     V{"pulse.bus.glue", 0.4},
};

inline constexpr std::array techno{
    V{"pulse.kick.drive", 0.6},      V{"pulse.kick.decay", 0.5},    V{"pulse.kick.tone", 0.8},
    V{"pulse.kick.character", 0.7},  V{"pulse.kick.tune", -1},      V{"pulse.chh.decay", 0.04},
    V{"pulse.chh.character", 0.7},   V{"pulse.ohh.character", 0.7}, V{"pulse.bus.glue", 0.6},
    V{"pulse.bus.saturation", 0.35},
};

inline constexpr std::array electro{
    V{"pulse.kick.decay", 1.1},     V{"pulse.kick.tune", 2},       V{"pulse.kick.tone", 0.6},
    V{"pulse.snare.tune", 3},       V{"pulse.snare.tone", 0.8},    V{"pulse.snare.character", 0.5},
    V{"pulse.clap.tone", 0.7},      V{"pulse.cowbell.level", 0.6}, V{"pulse.bus.glue", 0.3},
    V{"pulse.bus.saturation", 0.2},
};

inline constexpr std::array lofi{
    V{"pulse.bus.saturation", 0.7}, V{"pulse.chh.tone", 0.3},   V{"pulse.ohh.tone", 0.3},
    V{"pulse.cymbal.tone", 0.3},    V{"pulse.snare.tone", 0.3}, V{"pulse.out.level", -3},
    V{"pulse.bus.glue", 0.5},
};

inline constexpr std::array trap{
    V{"pulse.kick.decay", 2.5},   V{"pulse.kick.tone", 0.3},       V{"pulse.kick.tune", -5},
    V{"pulse.kick.drive", 0.3},   V{"pulse.kick.character", 0.2},  V{"pulse.snare.tune", 5},
    V{"pulse.snare.decay", 0.18}, V{"pulse.snare.character", 0.9}, V{"pulse.chh.decay", 0.035},
    V{"pulse.chh.tone", 0.9},     V{"pulse.clap.level", 0.8},      V{"pulse.clap.decay", 0.35},
};

inline constexpr std::array industrial{
    V{"pulse.kick.drive", 0.9},    V{"pulse.kick.character", 0.9}, V{"pulse.kick.tone", 0.9},
    V{"pulse.snare.drive", 0.8},   V{"pulse.clap.drive", 0.7},     V{"pulse.tom_low.drive", 0.6},
    V{"pulse.tom_mid.drive", 0.6}, V{"pulse.tom_high.drive", 0.6}, V{"pulse.bus.saturation", 0.8},
    V{"pulse.bus.glue", 0.8},      V{"pulse.out.level", -3},
};

inline constexpr std::array minimal{
    V{"pulse.kick.decay", 0.3},  V{"pulse.kick.tone", 0.4}, V{"pulse.kick.character", 0.15},
    V{"pulse.snare.level", 0.5}, V{"pulse.rim.level", 0.8}, V{"pulse.chh.decay", 0.03},
    V{"pulse.chh.level", 0.45},  V{"pulse.bus.glue", 0.15},
};

inline constexpr std::array dub{
    V{"pulse.kick.decay", 0.9},  V{"pulse.kick.tone", 0.45}, V{"pulse.snare.decay", 0.45},
    V{"pulse.snare.tone", 0.35}, V{"pulse.rim.decay", 0.1},  V{"pulse.rim.level", 0.7},
    V{"pulse.bus.glue", 0.3},
};

inline constexpr std::array breaks{
    V{"pulse.snare.decay", 0.3},    V{"pulse.snare.character", 0.75}, V{"pulse.snare.tone", 0.6},
    V{"pulse.kick.decay", 0.5},     V{"pulse.kick.character", 0.5},   V{"pulse.chh.character", 0.5},
    V{"pulse.chh.decay", 0.05},     V{"pulse.bus.glue", 0.55},        V{"pulse.bus.release", 0.1},
    V{"pulse.bus.saturation", 0.3},
};

inline constexpr std::array init{
    V{"pulse.bus.glue", 0},    V{"pulse.bus.saturation", 0}, V{"pulse.kick.drive", 0},
    V{"pulse.kick2.drive", 0}, V{"pulse.snare.drive", 0},    V{"pulse.clap.drive", 0},
};

inline constexpr std::array<Preset, 12> presets{{
    {"808 Classic", "808", {}},
    {"808 Boom", "808", boom},
    {"909 House", "909", house},
    {"909 Techno", "909", techno},
    {"Electro", "Electro", electro},
    {"Lo-Fi", "Character", lofi},
    {"Trap", "808", trap},
    {"Industrial", "Character", industrial},
    {"Minimal", "909", minimal},
    {"Dub", "Character", dub},
    {"Breaks", "Electro", breaks},
    {"Init", "Init", init},
}};

} // namespace oscillate::native_dsp::synth::pulse_presets
