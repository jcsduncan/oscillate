#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Flyby v1 (ADR-0067). The path is in metres around the listener, who faces
// +y with ears on the x axis: a Pass is a cubic Bézier through four points,
// an Orbit a circle. Defaults are the Train scene: a straight pass 14 m in
// front, left to right, at 90 km/h. Ids are persisted and automated: never
// rename or reuse one.
namespace oscillate::native_dsp::effects::flyby_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array paths{"Pass"sv, "Orbit"sv};
enum Path : std::uint8_t { pass, orbit };
inline constexpr std::array directions{"Clockwise"sv, "Anticlockwise"sv};
// Loop: round and round at Speed, on the song's clock. Once: one pass at
// Speed from each note-on of the tracks keying Flyby. Sync: one pass every
// Length of the song. Position: where Position says, for automation.
inline constexpr std::array runs{"Loop"sv, "Once"sv, "Sync"sv, "Position"sv};
enum Run : std::uint8_t { loop, once, sync, position };
inline constexpr std::array lengths{"1/4 Bar"sv, "1/2 Bar"sv, "1 Bar"sv, "2 Bars"sv,
                                    "4 Bars"sv,  "8 Bars"sv,  "16 Bars"sv};
inline constexpr std::array<double, 7> length_bars{0.25, 0.5, 1, 2, 4, 8, 16};
// Headphones: each ear its own delay (Woodworth) and head shadow (Brown and
// Duda). Speakers: a constant-power pan instead, since both ears hear both
// speakers.
inline constexpr std::array listens{"Headphones"sv, "Speakers"sv};
enum Listen : std::uint8_t { headphones, speakers };
// Where the source rests between Once passes: at the start, or gone.
inline constexpr std::array rests{"At Start"sv, "Silent"sv};
enum Rest : std::uint8_t { at_start, silent };

// The map's reach: every path point lies within this many metres.
inline constexpr double reach = 150.0;
inline constexpr std::size_t points = 4;

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = effect_glide_seconds;

inline constexpr std::array<ParameterDescriptor, 27> parameters{{
    {"flyby.path", "Path", "Path", "", S::choice, 0, last(paths.size()), 0, paths, T::live, 0},
    {"flyby.x1", "Start X", "Path", "m", S::linear, -reach, reach, -90, {}, T::live, g},
    {"flyby.y1", "Start Y", "Path", "m", S::linear, -reach, reach, 14, {}, T::live, g},
    {"flyby.x2", "Handle 1 X", "Path", "m", S::linear, -reach, reach, -30, {}, T::live, g},
    {"flyby.y2", "Handle 1 Y", "Path", "m", S::linear, -reach, reach, 14, {}, T::live, g},
    {"flyby.x3", "Handle 2 X", "Path", "m", S::linear, -reach, reach, 30, {}, T::live, g},
    {"flyby.y3", "Handle 2 Y", "Path", "m", S::linear, -reach, reach, 14, {}, T::live, g},
    {"flyby.x4", "End X", "Path", "m", S::linear, -reach, reach, 90, {}, T::live, g},
    {"flyby.y4", "End Y", "Path", "m", S::linear, -reach, reach, 14, {}, T::live, g},
    {"flyby.radius", "Radius", "Path", "m", S::logarithmic, 1, 100, 18, {}, T::live, g},
    {"flyby.direction", "Direction", "Path", "", S::choice, 0, 1, 0, directions, T::live, 0},
    {"flyby.height", "Height", "Path", "m", S::linear, 0, reach, 0, {}, T::live, g},
    {"flyby.run", "Run", "Motion", "", S::choice, 0, last(runs.size()), 0, runs, T::live, 0},
    {"flyby.speed", "Speed", "Motion", "km/h", S::logarithmic, 1, 1'000, 90, {}, T::live, g},
    {"flyby.length", "Length", "Motion", "", S::choice, 0, last(lengths.size()), 3, lengths,
     T::live, 0},
    // Position: 0 the start of the path, 1 its end (or once round an orbit).
    {"flyby.position", "Position", "Motion", "%", S::linear, 0, 1, 0, {}, T::live, 0.005},
    // 100% is the physics; less narrows the pitch swing (the travel delay
    // shrinks with it), more exaggerates it.
    {"flyby.doppler", "Doppler", "Physics", "%", S::linear, 0, 2, 1, {}, T::live, g},
    // 100% is air at 20 °C and 50% humidity; more for haze.
    {"flyby.air", "Air", "Physics", "%", S::linear, 0, 4, 1, {}, T::live, g},
    // 100% is the inverse-distance law (-6 dB per doubling).
    {"flyby.rolloff", "Distance", "Physics", "%", S::linear, 0, 2, 1, {}, T::live, g},
    {"flyby.room", "Room", "Room", "%", S::linear, 0, 1, 0.35, {}, T::live, g},
    {"flyby.room_size", "Size", "Room", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    {"flyby.listen", "Listen", "Output", "", S::choice, 0, 1, 0, listens, T::live, 0},
    {"flyby.mix", "Mix", "Output", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"flyby.output", "Output", "Output", "dB", S::linear, -24, 12, 0, {}, T::live, g},
    {"flyby.rest", "Between", "Motion", "", S::choice, 0, 1, 1, rests, T::live, 0},
    // The level at Reference metres is the input's level.
    {"flyby.reference", "Reference", "Physics", "m", S::logarithmic, 1, 50, 5, {}, T::live, g},
    // Closer than this the level stops rising (a source is not a point).
    {"flyby.near", "Nearest", "Physics", "m", S::logarithmic, 0.25, 10, 1, {}, T::live, g},
}};

consteval std::size_t index_of(std::string_view id) {
  return parameter_index(parameters, id);
}

} // namespace oscillate::native_dsp::effects::flyby_table
// clang-format on
