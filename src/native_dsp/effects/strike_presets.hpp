#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Strike presets: only values that differ from the table defaults. On the
// drum hits each is for, each moves the onset contrast (the first 15 ms
// against 30-150 ms on) the way its name says, by 1 to 10 dB, and the
// clipped ones keep the output under their ceiling: measured in
// strike_tests.cpp (presets_stay_restrained). Band counts, lookaheads
// and phases are 0-based choice indices (strike_table::band_counts,
// lookahead_ms, phases).
namespace oscillate::native_dsp::effects::strike_presets {

using V = std::pair<std::string_view, double>;
using namespace std::string_view_literals;

// The beater's click forward, the boom a little shorter; a millisecond of
// lookahead lifts the very first cycle.
inline constexpr std::array kick_snap{V{"strike.band_1.attack", 6}, V{"strike.band_1.sustain", -2},
                                      V{"strike.attack_time", 6}, V{"strike.sustain_time", 200},
                                      V{"strike.lookahead", 2}};
// The stick on the head, over a snare left as long as it was.
inline constexpr std::array snare_crack{V{"strike.band_1.attack", 7}, V{"strike.attack_time", 8},
                                        V{"strike.lookahead", 2}};
// Room and overhead mics: the ring between hits pulled down, the hits
// untouched.
inline constexpr std::array tighten_room{V{"strike.band_1.sustain", -8},
                                         V{"strike.sustain_time", 500}};
// A drum bus in three linear-phase bands (the split itself blurs no
// onset): the kick's thump, the snare's crack and the hats each lifted on
// their own, clipped under -0.3 dBFS.
inline constexpr std::array drum_bus_punch{
    V{"strike.bands", 2},         V{"strike.phase", 1},           V{"strike.xover_1", 150},
    V{"strike.xover_2", 2'500},   V{"strike.band_1.attack", 3},   V{"strike.band_1.sustain", -1},
    V{"strike.band_2.attack", 4}, V{"strike.band_2.sustain", -2}, V{"strike.band_3.attack", 2},
    V{"strike.clip", 1},          V{"strike.ceiling", -0.3}};
// The master in four linear-phase bands: a decibel or two of attack where
// the drums live, clipped under -1 dBFS.
inline constexpr std::array master_impact{
    V{"strike.bands", 3},           V{"strike.phase", 1},
    V{"strike.band_1.attack", 1.5}, V{"strike.band_2.attack", 2},
    V{"strike.band_3.attack", 1.5}, V{"strike.band_4.attack", 1},
    V{"strike.attack_time", 15},    V{"strike.clip", 1},
    V{"strike.ceiling", -1}};
// Hits rounded off, their bodies a little fuller: for harsh samples and
// spiky percussion.
inline constexpr std::array soften_attack{
    V{"strike.band_1.attack", -6}, V{"strike.band_1.sustain", 2}, V{"strike.attack_time", 15}};

inline constexpr std::array kick_tags{"kick"sv, "drums"sv, "punch"sv, "attack"sv};
inline constexpr std::array snare_tags{"snare"sv, "drums"sv, "crack"sv, "attack"sv};
inline constexpr std::array room_tags{"room"sv, "overheads"sv, "drums"sv, "sustain"sv};
inline constexpr std::array bus_tags{"drums"sv, "bus"sv, "punch"sv, "multiband"sv};
inline constexpr std::array master_tags{"master"sv, "impact"sv, "multiband"sv};
inline constexpr std::array soften_tags{"soft"sv, "percussion"sv, "harsh"sv};

inline constexpr std::array<Preset, 7> presets{{
    {"Default", "Init", {}},
    {"Kick Snap", "Drums", kick_snap, kick_tags},
    {"Snare Crack", "Drums", snare_crack, snare_tags},
    {"Tighten Room", "Drums", tighten_room, room_tags},
    {"Drum Bus Punch", "Bus", drum_bus_punch, bus_tags},
    {"Master Impact", "Master", master_impact, master_tags},
    {"Soften Attack", "Drums", soften_attack, soften_tags},
}};

} // namespace oscillate::native_dsp::effects::strike_presets
