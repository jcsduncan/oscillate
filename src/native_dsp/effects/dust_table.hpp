#pragma once

#include "oscillate/native_dsp/effects/dust.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <cstddef>
#include <string_view>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Dust v1 (ADR-0069). Eight layers of background texture, each with On,
// Level, Tone (its colour, dark .. bright) and Width (mono .. decorrelated),
// then the layer's own character; the input's age; the duck; the mix.
// Defaults are the Dusty 33 preset: a worn 33 with a little tape hiss, the
// input gently aged, the bed ducking a little under the kick. Ids are
// persisted and automated: never rename or reuse one.
namespace oscillate::native_dsp::effects::dust_table {

using S = synth::ParameterScale;
using T = synth::ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array switches{"Off"sv, "On"sv};
inline constexpr std::array speeds{"33"sv, "45"sv, "78"sv};
inline constexpr std::array mains{"50 Hz"sv, "60 Hz"sv};
inline constexpr std::array gates{"Off"sv, "1/4"sv, "1/8"sv, "1/16"sv};
enum Gate : int { gate_off, quarter, eighth, sixteenth };
inline constexpr std::array surfaces{"Glass"sv, "Roof"sv, "Leaves"sv};
enum Surface : int { glass, roof, leaves };
inline constexpr std::array machines{"Fan"sv, "Fridge"sv, "Servers"sv};
enum Machine : int { fan, fridge, servers };
inline constexpr std::array sources{"Sidechain"sv, "Key notes"sv};
enum Source : int { sidechain, key_notes };

// Each layer's range, and the age's mono fold (20 Hz is off).
inline constexpr double level_min_db = -60.0;
inline constexpr double level_max_db = 6.0;
// The bed sits low; its fader can bring it well forward when wanted.
inline constexpr double bed_max_db = 24.0;
inline constexpr double mono_off_hz = 20.0;
inline constexpr double mono_max_hz = 300.0;
inline constexpr double seed_max = 999.0;

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = synth::effect_glide_seconds;

// The four controls every layer has; `on` is 1 for the layers Dusty 33
// plays.
#define DUST_LAYER(L, NAME, ON, WIDTH)                                                            \
  {"dust." L ".on", "On", NAME, "", S::choice, 0, 1, ON, switches, T::live, 0},                    \
      {"dust." L ".level", "Level", NAME, "dB", S::linear, level_min_db, level_max_db, 0, {},      \
       T::live, g},                                                                                \
      {"dust." L ".tone", "Tone", NAME, "%", S::linear, 0, 1, 0.5, {}, T::live, g},                \
      {"dust." L ".width", "Width", NAME, "%", S::linear, 0, 1, WIDTH, {}, T::live, g}

inline constexpr std::array<synth::ParameterDescriptor, 62> parameters{{
    DUST_LAYER("vinyl", "Vinyl", 1, 0.5),
    // Pops a second, sparse .. dense.
    {"dust.vinyl.crackle", "Crackle", "Vinyl", "%", S::linear, 0, 1, 0.35, {}, T::live, g},
    // The pops' sizes: fine dust .. big pops.
    {"dust.vinyl.dust", "Dust", "Vinyl", "%", S::linear, 0, 1, 0.4, {}, T::live, g},
    // Surface noise, scratches and a little distortion.
    {"dust.vinyl.wear", "Wear", "Vinyl", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
    {"dust.vinyl.rumble", "Rumble", "Vinyl", "%", S::linear, 0, 1, 0.2, {}, T::live, g},
    // The rotation: scratches and the motor come round once a revolution.
    {"dust.vinyl.speed", "Speed", "Vinyl", "rpm", S::choice, 0, last(speeds.size()), 0, speeds,
     T::live, 0},
    DUST_LAYER("tape", "Tape", 1, 0.8),
    {"dust.tape.hiss", "Hiss", "Tape", "%", S::linear, 0, 1, 0.25, {}, T::live, g},
    // Brief level dips, also heard on the aged input while Tape is on.
    {"dust.tape.dropouts", "Dropouts", "Tape", "%", S::linear, 0, 1, 0.1, {}, T::live, g},
    DUST_LAYER("hum", "Hum", 0, 0.2),
    {"dust.hum.mains", "Mains", "Hum", "", S::choice, 0, last(mains.size()), 0, mains, T::live, 0},
    // Pure hum .. rectified, clipped harmonic buzz.
    {"dust.hum.buzz", "Buzz", "Hum", "%", S::linear, 0, 1, 0.2, {}, T::live, g},
    DUST_LAYER("radio", "Radio", 0, 0.3),
    // Between stations: the heterodyne whistles sweep as it moves.
    {"dust.radio.tuning", "Tuning", "Radio", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    // Tempo-locked interference chops.
    {"dust.radio.gate", "Gate", "Radio", "", S::choice, 0, last(gates.size()), 0, gates, T::live,
     0},
    DUST_LAYER("rain", "Rain", 0, 0.8),
    // Drizzle .. downpour.
    {"dust.rain.intensity", "Intensity", "Rain", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    {"dust.rain.surface", "Surface", "Rain", "", S::choice, 0, last(surfaces.size()), 1, surfaces,
     T::live, 0},
    // Near .. far: darker, and more of it heard through a room.
    {"dust.rain.distance", "Distance", "Rain", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
    DUST_LAYER("wind", "Wind", 0, 0.8),
    {"dust.wind.strength", "Strength", "Wind", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    {"dust.wind.gusts", "Gusts", "Wind", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    DUST_LAYER("room", "Room", 0, 0.7),
    // Small and close .. large and low.
    {"dust.room.size", "Size", "Room", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    DUST_LAYER("machine", "Machine", 0, 0.4),
    {"dust.machine.type", "Type", "Machine", "", S::choice, 0, last(machines.size()), 0, machines,
     T::live, 0},
    {"dust.machine.speed", "Speed", "Machine", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    // The input's age: a worn record's narrowing band and gentle drive.
    {"dust.age.amount", "Age", "Age", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
    {"dust.age.wow", "Wow", "Age", "%", S::linear, 0, 1, 0.15, {}, T::live, g},
    {"dust.age.flutter", "Flutter", "Age", "%", S::linear, 0, 1, 0.1, {}, T::live, g},
    // Lows below this are folded to mono; 20 Hz is off.
    {"dust.age.mono", "Mono Below", "Age", "Hz", S::logarithmic, mono_off_hz, mono_max_hz, 120, {},
     T::live, g},
    // The bed only, never the input.
    {"dust.duck.amount", "Duck", "Duck", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
    {"dust.duck.release", "Release", "Duck", "ms", S::logarithmic, 20, 1'000, 150, {}, T::live, g},
    {"dust.duck.source", "Source", "Duck", "", S::choice, 0, last(sources.size()), 0, sources,
     T::live, 0},
    {"dust.mix.input", "Input", "Mix", "dB", S::linear, level_min_db, level_max_db, 0, {}, T::live,
     g},
    {"dust.mix.bed", "Bed", "Mix", "dB", S::linear, level_min_db, bed_max_db, -6, {}, T::live,
     g},
    // A different record, a different storm.
    {"dust.mix.seed", "Seed", "Mix", "", S::stepped, 1, seed_max, 1, {}, T::live, 0},
    {"dust.mix.output", "Output", "Mix", "dB", S::linear, -24, 12, 0, {}, T::live, g},
}};

#undef DUST_LAYER

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

// Every layer's four controls, in Layer order.
inline constexpr std::array<std::size_t, dust::layer_count> layer_on{
    index_of("dust.vinyl.on"), index_of("dust.tape.on"), index_of("dust.hum.on"),
    index_of("dust.radio.on"), index_of("dust.rain.on"), index_of("dust.wind.on"),
    index_of("dust.room.on"),  index_of("dust.machine.on")};
// Level, Tone and Width follow On in every layer.
inline constexpr std::size_t level_offset = 1;
inline constexpr std::size_t tone_offset = 2;
inline constexpr std::size_t width_offset = 3;
static_assert(index_of("dust.machine.width") == layer_on[7] + width_offset);
static_assert(index_of("dust.rain.level") == layer_on[4] + level_offset);

} // namespace oscillate::native_dsp::effects::dust_table
// clang-format on
