#pragma once

#include "oscillate/native_dsp/parameters.hpp"
#include "strata_table.hpp"

#include <array>
#include <string_view>
#include <utility>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Strata presets: only values that differ from
// the defaults (four bands at 120 Hz, 1 kHz and 6 kHz; every band -18 dB,
// 2:1 above, 1:1 below, 6 dB knee, 24 dB range, 10 ms / 120 ms).
namespace oscillate::native_dsp::effects::strata_presets {

using V = std::pair<std::string_view, double>;
using namespace std::string_view_literals;

// Band counts are choice indices: "3" is index 2.
inline constexpr double three_bands = 2.0;
inline constexpr double two_bands = 1.0;
inline constexpr double five_bands = 4.0;

inline constexpr std::array glue_the_drums{
    V{"strata.bands", three_bands},          V{"strata.xover_1", 150},
    V{"strata.xover_2", 2'500},              V{"strata.mix", 0.7},
    V{"strata.lookahead", 2},                V{"strata.band_1.threshold", -20},
    V{"strata.band_1.attack", 30},           V{"strata.band_1.release", 150},
    V{"strata.band_2.threshold", -22},       V{"strata.band_2.above", 2.5},
    V{"strata.band_2.release", 120},         V{"strata.band_3.threshold", -26},
    V{"strata.band_3.attack", 5},            V{"strata.band_3.release", 80}};

// The two low bands key from the sidechain (route the kick's send here),
// split into the same bands.
inline constexpr std::array tame_the_bass{
    V{"strata.xover_1", 60},
    V{"strata.xover_2", 180},
    V{"strata.xover_3", 600},
    V{"strata.band_1.threshold", -24},
    V{"strata.band_1.above", 3},
    V{"strata.band_1.attack", 20},
    V{"strata.band_1.release", 200},
    V{"strata.band_1.key", strata_table::Key::sidechain_key},
    V{"strata.band_2.threshold", -22},
    V{"strata.band_2.above", 4},
    V{"strata.band_2.attack", 15},
    V{"strata.band_2.release", 160},
    V{"strata.band_2.key", strata_table::Key::sidechain_key},
    V{"strata.band_3.above", 1},
    V{"strata.band_4.above", 1}};

inline constexpr std::array air_on_the_hats{
    V{"strata.bands", two_bands},        V{"strata.xover_1", 6'500},
    V{"strata.band_1.above", 1},         V{"strata.band_2.threshold", -30},
    V{"strata.band_2.above", 1.5},       V{"strata.band_2.below", 2.5},
    V{"strata.band_2.attack", 2},        V{"strata.band_2.release", 60},
    V{"strata.band_2.makeup", 1.5}};

// Depth's default is 30%.
inline constexpr std::array ott_30{V{"strata.mode", strata_table::Mode::ott_mode}};

inline constexpr std::array master_polish{
    V{"strata.bands", five_bands},
    V{"strata.phase", strata_table::Phase::linear},
    V{"strata.lookahead", 5},
    V{"strata.xover_1", 100},
    V{"strata.xover_2", 400},
    V{"strata.xover_3", 2'500},
    V{"strata.xover_4", 9'000},
    V{"strata.band_1.threshold", -16},
    V{"strata.band_1.above", 1.5},
    V{"strata.band_1.attack", 30},
    V{"strata.band_1.release", 250},
    V{"strata.band_1.range", 4},
    V{"strata.band_2.threshold", -18},
    V{"strata.band_2.above", 1.5},
    V{"strata.band_2.attack", 30},
    V{"strata.band_2.release", 250},
    V{"strata.band_2.range", 4},
    V{"strata.band_3.threshold", -20},
    V{"strata.band_3.above", 1.5},
    V{"strata.band_3.attack", 25},
    V{"strata.band_3.release", 200},
    V{"strata.band_3.range", 4},
    V{"strata.band_4.threshold", -22},
    V{"strata.band_4.above", 1.5},
    V{"strata.band_4.attack", 15},
    V{"strata.band_4.release", 160},
    V{"strata.band_4.range", 4},
    V{"strata.band_5.threshold", -24},
    V{"strata.band_5.above", 1.4},
    V{"strata.band_5.below", 1.3},
    V{"strata.band_5.attack", 8},
    V{"strata.band_5.release", 120},
    V{"strata.band_5.range", 4}};

inline constexpr std::array de_mud_the_pad{
    V{"strata.bands", three_bands},
    V{"strata.xover_1", 180},
    V{"strata.xover_2", 520},
    V{"strata.band_1.above", 1},
    V{"strata.band_2.threshold", -26},
    V{"strata.band_2.above", 3},
    V{"strata.band_2.attack", 15},
    V{"strata.band_2.release", 180},
    V{"strata.band_2.range", 6},
    V{"strata.band_2.process", strata_table::Process::mid},
    V{"strata.band_3.above", 1}};

inline constexpr std::array init_tags{"init"sv, "flat"sv};
inline constexpr std::array glue_tags{"drums"sv, "bus"sv, "glue"sv};
inline constexpr std::array bass_tags{"bass"sv, "kick"sv, "sidechain"sv, "low end"sv};
inline constexpr std::array hats_tags{"hats"sv, "air"sv, "upward"sv};
inline constexpr std::array ott_tags{"ott"sv, "squash"sv, "upward"sv};
inline constexpr std::array master_tags{"master"sv, "linear phase"sv, "polish"sv};
inline constexpr std::array pad_tags{"pad"sv, "mud"sv, "mid"sv};

inline constexpr std::array<Preset, 7> presets{{
    {"Init", "Utility", {}, init_tags},
    {"Glue the drums", "Drums", glue_the_drums, glue_tags},
    {"Tame the bass", "Bass", tame_the_bass, bass_tags},
    {"Air on the hats", "Hats", air_on_the_hats, hats_tags},
    {"OTT 30%", "Character", ott_30, ott_tags},
    {"Master polish", "Master", master_polish, master_tags},
    {"De-mud the pad", "Pads", de_mud_the_pad, pad_tags},
}};

} // namespace oscillate::native_dsp::effects::strata_presets
// clang-format on
