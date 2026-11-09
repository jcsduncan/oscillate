#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Hush presets: only values that differ from the table defaults. Each lets
// the hits of the source it is for through within a decibel and lowers what
// lies between them by no more than its range, measured in hush_tests.cpp
// (presets_stay_restrained). Lookahead values are 0-based indices into
// hush_table::lookahead_ms.
namespace oscillate::native_dsp::effects::hush_presets {

using V = std::pair<std::string_view, double>;
using namespace std::string_view_literals;

// A kick mic: keyed on the kick's own band, so snare and hat bleed never
// open it; a millisecond of lookahead keeps the beater's click.
inline constexpr std::array tight_kick{V{"hush.threshold", -30}, V{"hush.attack", 0.1},
                                       V{"hush.hold", 40},       V{"hush.release", 80},
                                       V{"hush.lookahead", 2},   V{"hush.key_lp", 200}};
// A snare mic: kick and hats kept out of the key; the bleed only drops
// 30 dB, so the kit still sounds like one room.
inline constexpr std::array snare_bleed{V{"hush.threshold", -28}, V{"hush.range", -30},
                                        V{"hush.attack", 0.2},    V{"hush.hold", 40},
                                        V{"hush.release", 140},   V{"hush.lookahead", 2},
                                        V{"hush.key_hp", 150},    V{"hush.key_lp", 6'000}};
// A tom mic: open for the ring, closed between fills.
inline constexpr std::array tom_bleed{V{"hush.threshold", -32}, V{"hush.range", -24},
                                      V{"hush.attack", 0.2},    V{"hush.hold", 80},
                                      V{"hush.release", 400},   V{"hush.lookahead", 2},
                                      V{"hush.key_hp", 60},     V{"hush.key_lp", 1'500}};
// On a reverb return, keyed by the snare through a sidechain: a long hold
// and a short release cut the tail off square.
inline constexpr std::array gated_verb{V{"hush.threshold", -30}, V{"hush.hysteresis", 4},
                                       V{"hush.attack", 0.1}, V{"hush.hold", 250},
                                       V{"hush.release", 40}};
// Mains hum and buzz under a quiet part: the key ignores the hum's band.
inline constexpr std::array hum_gate{
    V{"hush.threshold", -55}, V{"hush.hysteresis", 8}, V{"hush.range", -20}, V{"hush.attack", 2},
    V{"hush.hold", 50},       V{"hush.release", 300},  V{"hush.key_hp", 200}};
// Gentle downward expansion that lowers the noise between phrases.
inline constexpr std::array soft_expander{
    V{"hush.threshold", -45}, V{"hush.mode", 1},  V{"hush.ratio", 1.5},  V{"hush.range", -15},
    V{"hush.attack", 5},      V{"hush.hold", 20}, V{"hush.release", 250}};
// Breaths between vocal lines a little quieter, never gone.
inline constexpr std::array vocal_breath{
    V{"hush.threshold", -42}, V{"hush.mode", 1},      V{"hush.range", -10}, V{"hush.attack", 3},
    V{"hush.hold", 60},       V{"hush.release", 200}, V{"hush.key_hp", 100}};

inline constexpr std::array kick_tags{"kick"sv, "drums"sv, "bleed"sv, "gate"sv};
inline constexpr std::array snare_tags{"snare"sv, "drums"sv, "bleed"sv, "gate"sv};
inline constexpr std::array tom_tags{"toms"sv, "drums"sv, "bleed"sv, "gate"sv};
inline constexpr std::array verb_tags{"reverb"sv, "sidechain"sv, "gate"sv, "eighties"sv};
inline constexpr std::array hum_tags{"hum"sv, "noise"sv, "cleanup"sv};
inline constexpr std::array expander_tags{"expander"sv, "noise"sv, "gentle"sv};
inline constexpr std::array vocal_tags{"vocal"sv, "breath"sv, "expander"sv};

inline constexpr std::array<Preset, 8> presets{{
    {"Default", "Init", {}},
    {"Tight Kick", "Drums", tight_kick, kick_tags},
    {"Snare Bleed", "Drums", snare_bleed, snare_tags},
    {"Tom Bleed", "Drums", tom_bleed, tom_tags},
    {"Gated Verb Key", "Sidechain", gated_verb, verb_tags},
    {"Hum Gate", "Cleanup", hum_gate, hum_tags},
    {"Soft Expander", "Cleanup", soft_expander, expander_tags},
    {"Vocal Breath", "Vocals", vocal_breath, vocal_tags},
}};

} // namespace oscillate::native_dsp::effects::hush_presets
