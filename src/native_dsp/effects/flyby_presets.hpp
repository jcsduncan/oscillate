#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Flyby factory presets (ADR-0067): only values that differ from the table
// defaults (the Train: a straight pass 14 m in front, left to right, 90
// km/h, looping). Choice values are 0-based (see flyby_table).
namespace oscillate::native_dsp::effects::flyby_presets {

using V = std::pair<std::string_view, double>;
using namespace std::string_view_literals;

// Straight passes run from x = -reach to +reach, y metres in front.
inline constexpr std::array car_pass{V{"flyby.x1", -90}, V{"flyby.y1", 6}, V{"flyby.x2", -30}, V{"flyby.y2", 6},
    V{"flyby.x3", 30}, V{"flyby.y3", 6}, V{"flyby.x4", 90}, V{"flyby.y4", 6}, V{"flyby.speed", 140}};
inline constexpr std::array racing{V{"flyby.x1", -120}, V{"flyby.y1", 4}, V{"flyby.x2", -40}, V{"flyby.y2", 4},
    V{"flyby.x3", 40}, V{"flyby.y3", 4}, V{"flyby.x4", 120}, V{"flyby.y4", 4}, V{"flyby.speed", 300},
                                   V{"flyby.room", 0.2}};
// Overhead and a little to the side, from behind to in front: the classic
// jet roar arriving after the plane.
inline constexpr std::array jet_flyover{
    V{"flyby.x1", 30},  V{"flyby.y1", -150}, V{"flyby.x2", 20},   V{"flyby.y2", -50},
    V{"flyby.x3", 10},  V{"flyby.y3", 50},   V{"flyby.x4", 0},    V{"flyby.y4", 150},
    V{"flyby.height", 90}, V{"flyby.speed", 700}, V{"flyby.air", 1.5}, V{"flyby.room", 0.15},
    V{"flyby.reference", 30}};
inline constexpr std::array bee{V{"flyby.path", 1},      V{"flyby.radius", 1.5}, V{"flyby.speed", 14},
                                V{"flyby.room", 0.15},   V{"flyby.near", 0.5},   V{"flyby.reference", 1.5}};
inline constexpr std::array circle_overhead{V{"flyby.path", 1}, V{"flyby.radius", 10}, V{"flyby.height", 6},
                                            V{"flyby.speed", 40}, V{"flyby.direction", 1}};
// A vocal or riser thrown across the stereo field once a bar, in time.
inline constexpr std::array vocal_throw{V{"flyby.x1", -25}, V{"flyby.y1", 3}, V{"flyby.x2", -25.0 / 3}, V{"flyby.y2", 3},
    V{"flyby.x3", 25.0 / 3}, V{"flyby.y3", 3}, V{"flyby.x4", 25}, V{"flyby.y4", 3}, V{"flyby.run", 2}, V{"flyby.length", 2},
                                        V{"flyby.doppler", 0.5}, V{"flyby.room", 0.3},
                                        V{"flyby.reference", 3}};
// From far ahead to right beside you, once per note on the keying track.
inline constexpr std::array fly_in{
    V{"flyby.x1", -20}, V{"flyby.y1", 150}, V{"flyby.x2", -10}, V{"flyby.y2", 60},
    V{"flyby.x3", 6},   V{"flyby.y3", 10},  V{"flyby.x4", 2},   V{"flyby.y4", 1},
    V{"flyby.run", 1},  V{"flyby.speed", 120}, V{"flyby.rest", 0}};
inline constexpr std::array slow_drift{V{"flyby.x1", -60}, V{"flyby.y1", 20}, V{"flyby.x2", -20}, V{"flyby.y2", 20},
    V{"flyby.x3", 20}, V{"flyby.y3", 20}, V{"flyby.x4", 60}, V{"flyby.y4", 20}, V{"flyby.speed", 6}, V{"flyby.room", 0.5},
                                       V{"flyby.room_size", 0.8}};
// For drawing the pass yourself with Position automation.
inline constexpr std::array automate{V{"flyby.x1", -60}, V{"flyby.y1", 10}, V{"flyby.x2", -20}, V{"flyby.y2", 10},
    V{"flyby.x3", 20}, V{"flyby.y3", 10}, V{"flyby.x4", 60}, V{"flyby.y4", 10}, V{"flyby.run", 3}};
inline constexpr std::array speakers_pass{V{"flyby.listen", 1}};

inline constexpr std::array vehicle_tags{"vehicle"sv, "doppler"sv};
inline constexpr std::array orbit_tags{"orbit"sv, "circle"sv};
inline constexpr std::array music_tags{"sync"sv, "throw"sv};

inline constexpr std::array<synth::SynthPreset, 11> presets{{
    {"Train", "Vehicle", {}, vehicle_tags},
    {"Car Pass", "Vehicle", car_pass, vehicle_tags},
    {"Racing", "Vehicle", racing, vehicle_tags},
    {"Jet Flyover", "Vehicle", jet_flyover, vehicle_tags},
    {"Bee", "Orbit", bee, orbit_tags},
    {"Circle Overhead", "Orbit", circle_overhead, orbit_tags},
    {"Vocal Throw", "Music", vocal_throw, music_tags},
    {"Fly-In", "Music", fly_in, music_tags},
    {"Slow Drift", "Music", slow_drift, music_tags},
    {"Draw It Yourself", "Utility", automate},
    {"Train on Speakers", "Utility", speakers_pass, vehicle_tags},
}};

} // namespace oscillate::native_dsp::effects::flyby_presets
// clang-format on
