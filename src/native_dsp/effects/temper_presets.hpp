#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Temper factory presets (ADR-0064): only values that differ from the table
// defaults (Depth 5, Detail 50, Soft, Natural, a flat sensitivity curve with
// nodes at 60, 250, 1k, 3.2k, 7.5k and 15k, every band off). Choice values
// are 0-based (see temper_table).
namespace oscillate::native_dsp::effects::temper_presets {

using V = std::pair<std::string_view, double>;
using namespace std::string_view_literals;

// Harsh vocal: listen hardest at 2-5 kHz, leave the body alone.
inline constexpr std::array tame_harshness{
    V{"temper.depth", 6},          V{"temper.detail", 0.6},        V{"temper.attack", 3},
    V{"temper.release", 60},       V{"temper.sens_gain_1", -12},  V{"temper.sens_gain_2", -6},
    V{"temper.sens_gain_4", 6},    V{"temper.sens_gain_5", 3},    V{"temper.sens_gain_6", -3}};
// Sibilance: the suppressor listens only up top, and a dynamic high shelf
// takes the esses down when they come.
inline constexpr std::array de_ess{
    V{"temper.depth", 8},           V{"temper.detail", 0.4},         V{"temper.attack", 1},
    V{"temper.release", 40},        V{"temper.sens_gain_1", -12},   V{"temper.sens_gain_2", -12},
    V{"temper.sens_gain_3", -9},    V{"temper.sens_freq_4", 4'500}, V{"temper.sens_gain_4", 3},
    V{"temper.sens_gain_5", 9},     V{"temper.sens_gain_6", 3},     V{"temper.band_on_1", 1},
    V{"temper.band_kind_1", 2},     V{"temper.band_freq_1", 6'500}, V{"temper.band_q_1", 0.7},
    V{"temper.band_range_1", -8},   V{"temper.band_threshold_1", -36},
    V{"temper.band_ratio_1", 4}};
// Boxy, muddy vocal: a dynamic bell on the box, the suppressor on the lows.
inline constexpr std::array mud_and_box{
    V{"temper.depth", 5},         V{"temper.detail", 0.45},          V{"temper.release", 120},
    V{"temper.sens_gain_1", -6},  V{"temper.sens_gain_2", 6},      V{"temper.sens_freq_3", 600},
    V{"temper.sens_gain_3", 4},   V{"temper.sens_gain_4", -6},     V{"temper.sens_gain_5", -9},
    V{"temper.sens_gain_6", -12}, V{"temper.band_on_1", 1},        V{"temper.band_freq_1", 320},
    V{"temper.band_q_1", 1.4},    V{"temper.band_range_1", -5},    V{"temper.band_threshold_1", -38}};
inline constexpr std::array acoustic_guitar{
    V{"temper.depth", 4},          V{"temper.detail", 0.55},         V{"temper.release", 100},
    V{"temper.sens_gain_1", -9},   V{"temper.sens_gain_2", 3},     V{"temper.sens_gain_4", 4},
    V{"temper.sens_gain_6", -6}};
// Cymbals and hats: Tilt leans detail and speed toward the top.
inline constexpr std::array glass_off{
    V{"temper.depth", 7},         V{"temper.detail", 0.75},         V{"temper.max_cut", 15},
    V{"temper.tilt", 0.3},         V{"temper.attack", 2},          V{"temper.release", 40},
    V{"temper.sens_gain_1", -12}, V{"temper.sens_gain_2", -9},    V{"temper.sens_gain_3", -4},
    V{"temper.sens_gain_4", 3},   V{"temper.sens_gain_5", 7},     V{"temper.sens_gain_6", 5}};
// Drums: tame the ring that follows each hit and leave the hit alone.
inline constexpr std::array ring_control{
    V{"temper.depth", 8},        V{"temper.detail", 0.8},     V{"temper.attacks", 0.1},
    V{"temper.sustains", 1},   V{"temper.separate", 0.6},   V{"temper.release", 150},
    V{"temper.sens_gain_1", -6}, V{"temper.sens_gain_2", 4}, V{"temper.sens_gain_3", 3}};
inline constexpr std::array snare_ring{
    V{"temper.depth", 10},      V{"temper.detail", 0.9},       V{"temper.attacks", 0},
    V{"temper.separate", 0.7},   V{"temper.release", 200},     V{"temper.sens_freq_2", 200},
    V{"temper.sens_gain_2", 6}, V{"temper.sens_freq_3", 700}, V{"temper.sens_gain_3", 4},
    V{"temper.sens_gain_5", -6}, V{"temper.sens_gain_6", -9}};
inline constexpr std::array mix_bus{
    V{"temper.depth", 2.5},     V{"temper.detail", 0.35},   V{"temper.max_cut", 6},
    V{"temper.attack", 8},      V{"temper.release", 120}, V{"temper.attacks", 0.6},
    V{"temper.sustains", 0.8},   V{"temper.phase", 2},     V{"temper.sens_gain_1", -6},
    V{"temper.sens_gain_4", 2}, V{"temper.sens_gain_5", 2}};
inline constexpr std::array master_air{
    V{"temper.depth", 3},          V{"temper.detail", 0.5},        V{"temper.max_cut", 6},
    V{"temper.phase", 2},          V{"temper.stereo", 1},         V{"temper.sens_gain_1", -12},
    V{"temper.sens_gain_2", -9},   V{"temper.sens_gain_3", -3},   V{"temper.sens_gain_4", 3},
    V{"temper.sens_gain_5", 6},    V{"temper.sens_gain_6", 4}};
// The kick keys it: the bass gives way exactly where the kick sits, while
// it plays.
inline constexpr std::array unmask_kick{
    V{"temper.depth", 2},           V{"temper.unmask", 1},          V{"temper.unmask_amount", 0.8},
    V{"temper.unmask_focus", 0.6},   V{"temper.attack", 2},          V{"temper.release", 90},
    V{"temper.sens_gain_1", 2},     V{"temper.sens_gain_2", 4},     V{"temper.sens_gain_3", -2},
    V{"temper.sens_gain_4", -8},    V{"temper.sens_gain_5", -12},   V{"temper.sens_gain_6", -12}};
// The vocal keys it: pads and guitars make room in the vocal's own bands.
inline constexpr std::array unmask_vocal{
    V{"temper.depth", 3},         V{"temper.unmask", 1},  V{"temper.unmask_amount", 0.6},
    V{"temper.unmask_focus", 0.4}, V{"temper.release", 150}, V{"temper.stereo", 1}};
inline constexpr std::array synth_fizz{
    V{"temper.depth", 6},        V{"temper.detail", 0.65},       V{"temper.tilt", 0.4},
    V{"temper.sens_gain_1", -12}, V{"temper.sens_gain_2", -9}, V{"temper.sens_gain_3", -3},
    V{"temper.sens_freq_4", 4'000}, V{"temper.sens_gain_4", 5}, V{"temper.sens_gain_5", 6}};
inline constexpr std::array live_low_latency{V{"temper.phase", 0}, V{"temper.depth", 5},
                                             V{"temper.attack", 2}, V{"temper.release", 50}};
// Room modes: narrow, slow cuts in the low mids, on the sustain.
inline constexpr std::array room_resonance{
    V{"temper.depth", 7},       V{"temper.detail", 0.85},        V{"temper.attacks", 0.3},
    V{"temper.release", 250},   V{"temper.sens_freq_1", 80},   V{"temper.sens_gain_1", 4},
    V{"temper.sens_gain_2", 6}, V{"temper.sens_freq_3", 500},  V{"temper.sens_gain_3", 3},
    V{"temper.sens_gain_4", -6}, V{"temper.sens_gain_5", -9},  V{"temper.sens_gain_6", -12}};
// The side channel only: tame harsh width without touching the centre.
inline constexpr std::array side_harshness{
    V{"temper.depth", 6},       V{"temper.stereo", 2},      V{"temper.sens_gain_1", -12},
    V{"temper.sens_gain_2", -6}, V{"temper.sens_gain_4", 4}, V{"temper.sens_gain_5", 4}};

inline constexpr std::array vocal_tags{"vocal"sv, "harsh"sv};
inline constexpr std::array de_ess_tags{"vocal"sv, "sibilance"sv, "de-ess"sv};
inline constexpr std::array mud_tags{"vocal"sv, "mud"sv, "box"sv};
inline constexpr std::array guitar_tags{"guitar"sv, "acoustic"sv};
inline constexpr std::array cymbal_tags{"drums"sv, "cymbals"sv, "harsh"sv};
inline constexpr std::array drum_tags{"drums"sv, "ring"sv, "split"sv};
inline constexpr std::array bus_tags{"bus"sv, "gentle"sv, "linear"sv};
inline constexpr std::array master_tags{"master"sv, "air"sv, "linear"sv};
inline constexpr std::array unmask_tags{"unmask"sv, "sidechain"sv, "bass"sv};
inline constexpr std::array unmask_vocal_tags{"unmask"sv, "sidechain"sv, "vocal"sv};
inline constexpr std::array synth_tags{"synth"sv, "harsh"sv};
inline constexpr std::array live_tags{"live"sv, "tracking"sv};
inline constexpr std::array room_tags{"room"sv, "resonance"sv};
inline constexpr std::array stereo_tags{"stereo"sv, "side"sv};

inline constexpr std::array<Preset, 16> presets{{
    {"Default", "Utility", {}},
    {"Tame Harshness", "Vocal", tame_harshness, vocal_tags},
    {"De-Ess", "Vocal", de_ess, de_ess_tags},
    {"Mud & Box", "Vocal", mud_and_box, mud_tags},
    {"Acoustic Guitar", "Instrument", acoustic_guitar, guitar_tags},
    {"Glass Off", "Drums", glass_off, cymbal_tags},
    {"Ring Control", "Drums", ring_control, drum_tags},
    {"Snare Ring", "Drums", snare_ring, drum_tags},
    {"Mix Bus Gentle", "Bus", mix_bus, bus_tags},
    {"Master Air", "Master", master_air, master_tags},
    {"Unmask Kick", "Unmask", unmask_kick, unmask_tags},
    {"Unmask Vocal", "Unmask", unmask_vocal, unmask_vocal_tags},
    {"Synth Fizz", "Instrument", synth_fizz, synth_tags},
    {"Live Low Latency", "Utility", live_low_latency, live_tags},
    {"Room Resonance", "Instrument", room_resonance, room_tags},
    {"Side Harshness", "Stereo", side_harshness, stereo_tags},
}};

} // namespace oscillate::native_dsp::effects::temper_presets
// clang-format on
