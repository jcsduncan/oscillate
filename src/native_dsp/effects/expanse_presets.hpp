#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Expanse factory presets: only values that differ from the table defaults
// (the "Concert Hall"). Choice values are 0-based (see expanse_table).
namespace oscillate::native_dsp::effects::expanse_presets {

using V = std::pair<std::string_view, double>;

inline constexpr std::array small_room{V{"expanse.mode", 0},         V{"expanse.size", 0.3},
                                       V{"expanse.decay", 0.6},      V{"expanse.predelay", 4},
                                       V{"expanse.early", 0.8},      V{"expanse.high_decay", 0.6},
                                       V{"expanse.mod_depth", 0.15}, V{"expanse.high_freq", 6'000}};
inline constexpr std::array drum_room{V{"expanse.mode", 0},         V{"expanse.size", 0.45},
                                      V{"expanse.decay", 0.9},      V{"expanse.predelay", 8},
                                      V{"expanse.early", 1},        V{"expanse.low_decay", 0.8},
                                      V{"expanse.high_decay", 0.7}, V{"expanse.mix", 0.25}};
inline constexpr std::array vocal_plate{
    V{"expanse.mode", 2},          V{"expanse.size", 0.5},    V{"expanse.decay", 1.8},
    V{"expanse.predelay", 30},     V{"expanse.early", 0},     V{"expanse.high_decay", 0.8},
    V{"expanse.high_freq", 7'000}, V{"expanse.low_cut", 150}, V{"expanse.mod_depth", 0.25}};
inline constexpr std::array bright_plate{
    V{"expanse.mode", 2},     V{"expanse.size", 0.65},      V{"expanse.decay", 2.6},
    V{"expanse.early", 0},    V{"expanse.high_decay", 1.1}, V{"expanse.high_cut", 18'000},
    V{"expanse.low_cut", 200}};
inline constexpr std::array stone_chamber{V{"expanse.mode", 3},       V{"expanse.size", 0.5},
                                          V{"expanse.decay", 1.6},    V{"expanse.predelay", 12},
                                          V{"expanse.early", 0.7},    V{"expanse.high_decay", 0.65},
                                          V{"expanse.mod_depth", 0.2}};
inline constexpr std::array cathedral{V{"expanse.mode", 4},          V{"expanse.size", 0.9},
                                      V{"expanse.decay", 7},         V{"expanse.predelay", 60},
                                      V{"expanse.low_decay", 1.4},   V{"expanse.high_decay", 0.4},
                                      V{"expanse.high_freq", 3'500}, V{"expanse.mix", 0.35}};
inline constexpr std::array shimmer_heaven{
    V{"expanse.mode", 4},        V{"expanse.size", 0.85},   V{"expanse.decay", 9},
    V{"expanse.shimmer", 0.45},  V{"expanse.predelay", 40}, V{"expanse.high_decay", 0.7},
    V{"expanse.mod_depth", 0.5}, V{"expanse.mix", 0.45},    V{"expanse.low_cut", 180}};
inline constexpr std::array fifth_shimmer{
    V{"expanse.mode", 1},          V{"expanse.decay", 6},        V{"expanse.shimmer", 0.35},
    V{"expanse.shimmer_pitch", 2}, V{"expanse.mod_depth", 0.45}, V{"expanse.mix", 0.4}};
inline constexpr std::array endless_cloud{V{"expanse.mode", 5},
                                          V{"expanse.size", 1},
                                          V{"expanse.decay", 16},
                                          V{"expanse.early_diffusion", 1},
                                          V{"expanse.late_diffusion", 0.9},
                                          V{"expanse.mod_depth", 0.7},
                                          V{"expanse.mod_rate", 0.3},
                                          V{"expanse.early", 0},
                                          V{"expanse.mix", 0.5}};
inline constexpr std::array dark_hall{V{"expanse.decay", 3.2}, V{"expanse.high_decay", 0.3},
                                      V{"expanse.high_freq", 2'500}, V{"expanse.high_cut", 7'000},
                                      V{"expanse.low_decay", 1.5}};
inline constexpr std::array ambient_wash{V{"expanse.mode", 5},    V{"expanse.size", 0.8},
                                         V{"expanse.decay", 8},   V{"expanse.mod_depth", 0.6},
                                         V{"expanse.early", 0.2}, V{"expanse.predelay", 80},
                                         V{"expanse.mix", 0.4}};
inline constexpr std::array init{V{"expanse.mod_depth", 0},     V{"expanse.low_decay", 1},
                                 V{"expanse.high_decay", 1},    V{"expanse.low_cut", 20},
                                 V{"expanse.high_cut", 20'000}, V{"expanse.predelay", 0},
                                 V{"expanse.mix", 0.5}};

inline constexpr std::array<synth::SynthPreset, 13> presets{{
    {"Concert Hall", "Hall", {}},
    {"Small Room", "Room", small_room},
    {"Drum Room", "Room", drum_room},
    {"Vocal Plate", "Plate", vocal_plate},
    {"Bright Plate", "Plate", bright_plate},
    {"Stone Chamber", "Chamber", stone_chamber},
    {"Cathedral", "Cathedral", cathedral},
    {"Dark Hall", "Hall", dark_hall},
    {"Shimmer Heaven", "Shimmer", shimmer_heaven},
    {"Fifth Shimmer", "Shimmer", fifth_shimmer},
    {"Endless Cloud", "Cloud", endless_cloud},
    {"Ambient Wash", "Cloud", ambient_wash},
    {"Init", "Init", init},
}};

} // namespace oscillate::native_dsp::effects::expanse_presets
