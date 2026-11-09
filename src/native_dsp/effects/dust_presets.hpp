#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Dust factory presets (ADR-0069): only values that differ from the table
// defaults (Dusty 33: a worn 33 with a little tape hiss, the input gently
// aged, a light duck). Choice values are 0-based (see dust_table). Mixed
// for minimal and dub techno: the bed sits under the track, not on it.
namespace oscillate::native_dsp::effects::dust_presets {

using V = std::pair<std::string_view, double>;
using namespace std::string_view_literals;

// The weather and room presets leave the input as it is.
#define DUST_CLEAN_INPUT                                                                           \
  V{"dust.age.amount", 0}, V{"dust.age.wow", 0}, V{"dust.age.flutter", 0}, V{"dust.age.mono", 20}

inline constexpr std::array worn_78{
    V{"dust.vinyl.speed", 2},    V{"dust.vinyl.crackle", 0.6}, V{"dust.vinyl.dust", 0.75},
    V{"dust.vinyl.wear", 0.7},   V{"dust.vinyl.rumble", 0.35}, V{"dust.vinyl.tone", 0.3},
    V{"dust.tape.on", 0},        V{"dust.hum.on", 1},          V{"dust.hum.level", -16},
    V{"dust.hum.buzz", 0.1},     V{"dust.age.amount", 0.65},   V{"dust.age.wow", 0.3},
    V{"dust.age.flutter", 0.25}, V{"dust.age.mono", 200},      V{"dust.mix.bed", -5}};
inline constexpr std::array fresh_45{
    V{"dust.vinyl.speed", 1},    V{"dust.vinyl.crackle", 0.15}, V{"dust.vinyl.dust", 0.2},
    V{"dust.vinyl.wear", 0.1},   V{"dust.vinyl.rumble", 0.1},   V{"dust.vinyl.tone", 0.6},
    V{"dust.tape.on", 0},        V{"dust.age.amount", 0.1},     V{"dust.age.wow", 0.05},
    V{"dust.age.flutter", 0.05}, V{"dust.mix.bed", -8}};
inline constexpr std::array cassette_deck{
    V{"dust.vinyl.on", 0},       V{"dust.tape.hiss", 0.5},  V{"dust.tape.dropouts", 0.35},
    V{"dust.tape.tone", 0.55},   V{"dust.hum.on", 1},       V{"dust.hum.level", -18},
    V{"dust.hum.buzz", 0.05},    V{"dust.age.amount", 0.4}, V{"dust.age.wow", 0.35},
    V{"dust.age.flutter", 0.45}, V{"dust.age.mono", 150},   V{"dust.mix.bed", -8}};
inline constexpr std::array late_night_radio{
    V{"dust.vinyl.on", 0},     V{"dust.tape.on", 0},       V{"dust.radio.on", 1},
    V{"dust.radio.level", -2}, V{"dust.radio.tone", 0.45}, V{"dust.radio.tuning", 0.42},
    V{"dust.hum.on", 1},       V{"dust.hum.level", -14},   V{"dust.hum.buzz", 0.3},
    V{"dust.age.amount", 0.7}, V{"dust.age.wow", 0},       V{"dust.age.flutter", 0},
    V{"dust.age.mono", 300}};
inline constexpr std::array static_pulse{
    V{"dust.vinyl.on", 0},       V{"dust.tape.on", 0},    V{"dust.radio.on", 1},
    V{"dust.radio.tuning", 0.7}, V{"dust.radio.gate", 3}, V{"dust.radio.width", 0.5},
    V{"dust.duck.amount", 0.5},  V{"dust.mix.bed", -8},   DUST_CLEAN_INPUT};
inline constexpr std::array rain_on_the_window{
    V{"dust.vinyl.on", 0},     V{"dust.tape.on", 0},          V{"dust.rain.on", 1},
    V{"dust.rain.surface", 0}, V{"dust.rain.intensity", 0.4}, V{"dust.rain.distance", 0.15},
    V{"dust.rain.tone", 0.6},  V{"dust.room.on", 1},          V{"dust.room.level", -10},
    DUST_CLEAN_INPUT};
inline constexpr std::array storm_front{
    V{"dust.vinyl.on", 0},         V{"dust.tape.on", 0},          V{"dust.rain.on", 1},
    V{"dust.rain.intensity", 0.9}, V{"dust.rain.distance", 0.4},  V{"dust.wind.on", 1},
    V{"dust.wind.level", -3},      V{"dust.wind.strength", 0.75}, V{"dust.wind.gusts", 0.8},
    V{"dust.duck.amount", 0.4},    V{"dust.mix.bed", -13},        DUST_CLEAN_INPUT};
inline constexpr std::array wind_through_the_yard{
    V{"dust.vinyl.on", 0},         V{"dust.tape.on", 0},      V{"dust.wind.on", 1},
    V{"dust.wind.strength", 0.45}, V{"dust.wind.gusts", 0.6}, V{"dust.wind.tone", 0.4},
    V{"dust.room.on", 1},          V{"dust.room.level", -12}, DUST_CLEAN_INPUT};
// Vinyl, a big room and a hum, ducked hard under the kick.
inline constexpr std::array dub_chamber{
    V{"dust.vinyl.crackle", 0.45}, V{"dust.vinyl.dust", 0.5},   V{"dust.vinyl.wear", 0.4},
    V{"dust.vinyl.rumble", 0.3},   V{"dust.vinyl.tone", 0.4},   V{"dust.tape.on", 0},
    V{"dust.room.on", 1},          V{"dust.room.size", 0.8},    V{"dust.room.level", -6},
    V{"dust.hum.on", 1},           V{"dust.hum.level", -10},    V{"dust.hum.buzz", 0.25},
    V{"dust.duck.amount", 0.8},    V{"dust.duck.release", 300}, V{"dust.age.amount", 0.35},
    V{"dust.age.wow", 0.1},        V{"dust.age.flutter", 0.05}, V{"dust.age.mono", 150},
    V{"dust.mix.bed", -4}};
inline constexpr std::array basement_hum{V{"dust.vinyl.on", 0},    V{"dust.tape.on", 0},
                                         V{"dust.hum.on", 1},      V{"dust.hum.buzz", 0.45},
                                         V{"dust.hum.tone", 0.35}, V{"dust.room.on", 1},
                                         V{"dust.room.size", 0.3}, V{"dust.room.level", -8},
                                         V{"dust.mix.bed", -10},   DUST_CLEAN_INPUT};
inline constexpr std::array server_room{
    V{"dust.vinyl.on", 0},     V{"dust.tape.on", 0},         V{"dust.machine.on", 1},
    V{"dust.machine.type", 2}, V{"dust.machine.speed", 0.6}, V{"dust.room.on", 1},
    V{"dust.room.size", 0.4},  V{"dust.room.level", -10},    V{"dust.hum.on", 1},
    V{"dust.hum.mains", 1},    V{"dust.hum.level", -18},     V{"dust.mix.bed", -8},
    DUST_CLEAN_INPUT};
inline constexpr std::array warehouse{
    V{"dust.vinyl.on", 0},       V{"dust.tape.on", 0},      V{"dust.room.on", 1},
    V{"dust.room.size", 1},      V{"dust.machine.on", 1},   V{"dust.machine.speed", 0.3},
    V{"dust.machine.level", -8}, V{"dust.wind.on", 1},      V{"dust.wind.strength", 0.2},
    V{"dust.wind.gusts", 0.3},   V{"dust.wind.level", -12}, V{"dust.duck.amount", 0.5},
    V{"dust.duck.release", 250}, V{"dust.age.amount", 0.1}, V{"dust.age.wow", 0},
    V{"dust.age.flutter", 0},    V{"dust.age.mono", 20}};
inline constexpr std::array lo_fi_bed{
    V{"dust.vinyl.crackle", 0.5}, V{"dust.vinyl.dust", 0.3},  V{"dust.vinyl.wear", 0.5},
    V{"dust.vinyl.rumble", 0.1},  V{"dust.vinyl.tone", 0.35}, V{"dust.tape.hiss", 0.4},
    V{"dust.tape.dropouts", 0.2}, V{"dust.tape.level", -4},   V{"dust.age.amount", 0.6},
    V{"dust.age.wow", 0.25},      V{"dust.age.flutter", 0.2}, V{"dust.age.mono", 200},
    V{"dust.mix.bed", -5}};
// Rain far off over leaves, a breeze and the space around it.
inline constexpr std::array field_day{V{"dust.vinyl.on", 0},          V{"dust.tape.on", 0},
                                      V{"dust.rain.on", 1},           V{"dust.rain.surface", 2},
                                      V{"dust.rain.intensity", 0.35}, V{"dust.rain.distance", 0.85},
                                      V{"dust.wind.on", 1},           V{"dust.wind.strength", 0.3},
                                      V{"dust.wind.gusts", 0.4},      V{"dust.wind.level", -6},
                                      V{"dust.room.on", 1},           V{"dust.room.size", 0.9},
                                      V{"dust.room.level", -10},      DUST_CLEAN_INPUT};
// Nothing but the input, passed through untouched (delayed by the latency).
inline constexpr std::array init{V{"dust.vinyl.on", 0}, V{"dust.tape.on", 0},
                                 V{"dust.duck.amount", 0}, DUST_CLEAN_INPUT};

#undef DUST_CLEAN_INPUT

inline constexpr std::array vinyl_tags{"vinyl"sv, "crackle"sv};
inline constexpr std::array tape_tags{"tape"sv, "hiss"sv};
inline constexpr std::array radio_tags{"radio"sv, "static"sv};
inline constexpr std::array weather_tags{"rain"sv, "wind"sv, "field"sv};
inline constexpr std::array space_tags{"room"sv, "hum"sv, "machine"sv};
inline constexpr std::array dub_tags{"dub"sv, "vinyl"sv, "room"sv};

inline constexpr std::array<Preset, 16> presets{{
    {"Dusty 33", "Vinyl", {}, vinyl_tags},
    {"Worn 78", "Vinyl", worn_78, vinyl_tags},
    {"Fresh 45", "Vinyl", fresh_45, vinyl_tags},
    {"Cassette Deck", "Tape", cassette_deck, tape_tags},
    {"Late Night Radio", "Radio", late_night_radio, radio_tags},
    {"Static Pulse", "Radio", static_pulse, radio_tags},
    {"Rain on the Window", "Weather", rain_on_the_window, weather_tags},
    {"Storm Front", "Weather", storm_front, weather_tags},
    {"Wind Through the Yard", "Weather", wind_through_the_yard, weather_tags},
    {"Dub Chamber", "Space", dub_chamber, dub_tags},
    {"Basement Hum", "Space", basement_hum, space_tags},
    {"Server Room", "Space", server_room, space_tags},
    {"Warehouse", "Space", warehouse, space_tags},
    {"Lo-fi Bed", "Vinyl", lo_fi_bed, vinyl_tags},
    {"Field Day", "Weather", field_day, weather_tags},
    {"Init", "Utility", init},
}};

} // namespace oscillate::native_dsp::effects::dust_presets
