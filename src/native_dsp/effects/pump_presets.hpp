#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>

// Pump factory presets (ADR-0058): values that differ from the table
// defaults (Tempo, 1/4, full depth, full band) and the shapes each draws.
// Choice values are 0-based (see pump_table).
namespace oscillate::native_dsp::effects::pump_presets {

using V = std::pair<std::string_view, double>;

// A shape point: x and y 0..1, and the tension of the segment to the next.
struct Point {
  double x = 0.0;
  double y = 0.0;
  double tension = 0.0;
};
struct NamedShape {
  std::string_view name;
  std::span<const Point> points;
};

// The shape library, Classic Pump first (the default).
inline constexpr std::array<Point, 4> classic{
    {{0, 0, 0}, {0.06, 0.05, 0.5}, {0.42, 0.97, -0.4}, {1, 1, 0}}};
inline constexpr std::array<Point, 4> fast_release{
    {{0, 0, 0}, {0.03, 0.1, 0.6}, {0.2, 1, 0}, {1, 1, 0}}};
inline constexpr std::array<Point, 4> long_tail{
    {{0, 0, 0}, {0.08, 0.05, 0.2}, {0.85, 1, -0.6}, {1, 1, 0}}};
inline constexpr std::array<Point, 4> gate{{{0, 0, 0}, {0.18, 0, 0}, {0.2, 1, 0}, {1, 1, 0}}};
inline constexpr std::array<Point, 4> soft_duck{
    {{0, 0.45, 0}, {0.05, 0.5, 0.3}, {0.5, 1, -0.3}, {1, 1, 0}}};
inline constexpr std::array<Point, 4> hold_snap{
    {{0, 0, 0}, {0.3, 0.05, 0.9}, {0.38, 1, 0}, {1, 1, 0}}};
inline constexpr std::array<Point, 3> breathe{{{0, 0.2, 0}, {0.5, 1, 0.5}, {1, 0.2, 0}}};
inline constexpr std::array<Point, 3> swell{{{0, 0, 0}, {0.9, 1, 0.7}, {1, 1, 0}}};
inline constexpr std::array<Point, 5> off_beat{
    {{0, 1, 0}, {0.45, 1, 0}, {0.5, 0, 0.4}, {0.95, 1, -0.3}, {1, 1, 0}}};
inline constexpr std::array<Point, 7> stutter{
    {{0, 0, 0}, {0.1, 1, 0}, {0.25, 0, 0}, {0.35, 1, 0}, {0.5, 0, 0}, {0.6, 1, 0}, {1, 1, 0}}};
inline constexpr std::array<Point, 4> ducker{
    {{0, 0.2, 0}, {0.04, 0.2, 0.4}, {0.3, 1, -0.2}, {1, 1, 0}}};

inline constexpr std::array<NamedShape, 11> shapes{{
    {"Classic Pump", classic},
    {"Fast Release", fast_release},
    {"Long Tail", long_tail},
    {"Gate", gate},
    {"Soft Duck", soft_duck},
    {"Hold & Snap", hold_snap},
    {"Breathe", breathe},
    {"Swell", swell},
    {"Off-Beat", off_beat},
    {"Stutter", stutter},
    {"Ducker", ducker},
}};

inline constexpr std::array<V, 0> classic_pump{};
inline constexpr std::array fast_pump{V{"pump.anticlick", 1.5}};
inline constexpr std::array long_tail_values{V{"pump.anticlick", 3}};
inline constexpr std::array hard_gate{V{"pump.anticlick", 1}};
inline constexpr std::array soft_duck_values{V{"pump.depth", 0.7}, V{"pump.anticlick", 4}};
inline constexpr std::array hold_snap_values{V{"pump.anticlick", 1.5}};
inline constexpr std::array breathe_values{V{"pump.rate", 3}, V{"pump.anticlick", 5}};
inline constexpr std::array swell_values{V{"pump.rate", 4}, V{"pump.anticlick", 3}};
inline constexpr std::array off_beat_values{V{"pump.rate", 3}};
inline constexpr std::array stutter_values{V{"pump.rate", 3}, V{"pump.anticlick", 1}};
inline constexpr std::array kick_follower{V{"pump.trigger", 1}, V{"pump.lookahead", 1},
                                          V{"pump.depth", 0.85}};
inline constexpr std::array lows_pump{V{"pump.band", 1}, V{"pump.crossover", 150}};
// A drum bus keys it: only the kick's band restarts the cycle, stretched to
// the gap between kicks.
inline constexpr std::array drum_bus_kick{V{"pump.trigger", 1}, V{"pump.lookahead", 1},
                                          V{"pump.follow", 1}, V{"pump.depth", 0.85}};
// A drum rack keys it: only C1 (the kick) triggers, soft hits duck less.
inline constexpr std::array rack_kick{V{"pump.trigger", 2}, V{"pump.note_low", 36},
                                      V{"pump.note_high", 36}, V{"pump.velocity", 0.5},
                                      V{"pump.follow", 1}};
inline constexpr std::array wide_pump{V{"pump.stereo", 1}};
inline constexpr std::array three_band{V{"pump.band", 2}};
inline constexpr std::array swing_pump{V{"pump.rate", 1}, V{"pump.swing", 0.62},
                                       V{"pump.anticlick", 1.5}};
// Three bars of A, then B's stutter as the fill.
inline constexpr std::array bar_fill{V{"pump.pattern", 4}, V{"pump.step.4", 1}};
inline constexpr std::array morph_pump{V{"pump.slot", 5}, V{"pump.morph", 0.5}};
inline constexpr std::array<V, 0> init{};

inline constexpr std::array<synth::SynthPreset, 20> presets{{
    {"Classic Pump", "House", classic_pump},
    {"Fast Pump", "House", fast_pump},
    {"Long Tail", "EDM", long_tail_values},
    {"Hard Gate", "Gate", hard_gate},
    {"Soft Duck", "Gentle", soft_duck_values},
    {"Hold & Snap", "Gate", hold_snap_values},
    {"Breathe", "Movement", breathe_values},
    {"Swell", "Movement", swell_values},
    {"Off-Beat", "Movement", off_beat_values},
    {"Stutter", "Gate", stutter_values},
    {"Kick Follower", "Sidechain", kick_follower},
    {"Lows Pump", "Sidechain", lows_pump},
    {"Drum Bus Kick", "Sidechain", drum_bus_kick},
    {"Rack Kick", "Sidechain", rack_kick},
    {"Wide Pump", "House", wide_pump},
    {"Three Band", "EDM", three_band},
    {"Swing Pump", "House", swing_pump},
    {"Bar Fill", "Movement", bar_fill},
    {"Morph Pump", "Movement", morph_pump},
    {"Init", "Init", init},
}};
// Each preset's shape (slot A), by index into `shapes`.
inline constexpr std::array<std::size_t, presets.size()> preset_shapes{
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 10, 0, 0, 0, 1, 0, 0, 0};
// Each preset's slot B shape, or -1 for none (B follows A).
inline constexpr std::array<int, presets.size()> preset_b_shapes{
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 9, 3, -1};

} // namespace oscillate::native_dsp::effects::pump_presets
