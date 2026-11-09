#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Cast presets: only values that differ from the table defaults, and the
// bundled IR each plays (preset_irs, by preset). The mix stays at or under
// 40 % and no preset lifts the wet gain, measured in cast_tests.cpp
// (presets_stay_restrained).
namespace oscillate::native_dsp::effects::cast_presets {

using V = std::pair<std::string_view, double>;
using namespace std::string_view_literals;

// A plate for vocals: pre-delay keeps the words clear, the cuts keep it out
// of the mud and the sibilance.
inline constexpr std::array vocal_plate{V{"cast.predelay", 30}, V{"cast.low_cut", 180},
                                        V{"cast.high_cut", 9'000}, V{"cast.mix", 0.22}};
// A wooden live room around a kit, short and dark.
inline constexpr std::array drum_room{V{"cast.low_cut", 120}, V{"cast.high_cut", 7'000},
                                      V{"cast.mix", 0.2}};
// A plaster chamber on a snare, the classic studio echo chamber.
inline constexpr std::array snare_chamber{V{"cast.predelay", 10}, V{"cast.low_cut", 200},
                                          V{"cast.mix", 0.25}};
// The hall, with a little pre-delay.
inline constexpr std::array concert_hall{V{"cast.predelay", 20}, V{"cast.mix", 0.25}};
// The cathedral, ducked under its source so a part stays readable.
inline constexpr std::array cathedral_wash{V{"cast.predelay", 40}, V{"cast.low_cut", 150},
                                           V{"cast.duck", 9}, V{"cast.mix", 0.35}};
// A spring on a guitar or keys.
inline constexpr std::array spring_tank{V{"cast.low_cut", 150}, V{"cast.mix", 0.25}};
// The hall reversed: a swell into each note.
inline constexpr std::array reverse_swell{V{"cast.reverse", 1}, V{"cast.length", 0.6},
                                          V{"cast.mix", 0.3}};
// A vocal booth's few milliseconds, to put a dry voice in a space.
inline constexpr std::array booth{V{"cast.mix", 0.2}};
// The warehouse stretched, cut short and pushed back for a big, dark
// space behind drums.
inline constexpr std::array big_space{V{"cast.stretch", 1.3}, V{"cast.decay", 0.6},
                                      V{"cast.high_cut", 6'000}, V{"cast.mix", 0.2}};

inline constexpr std::array vocal_tags{"vocal"sv, "plate"sv, "bright"sv};
inline constexpr std::array drum_tags{"drums"sv, "room"sv, "short"sv};
inline constexpr std::array snare_tags{"snare"sv, "chamber"sv};
inline constexpr std::array hall_tags{"hall"sv, "orchestral"sv, "natural"sv};
inline constexpr std::array wash_tags{"ambient"sv, "long"sv, "ducked"sv};
inline constexpr std::array spring_tags{"guitar"sv, "spring"sv, "vintage"sv};
inline constexpr std::array swell_tags{"reverse"sv, "swell"sv, "fx"sv};
inline constexpr std::array booth_tags{"vocal"sv, "short"sv, "dry"sv};
inline constexpr std::array big_tags{"drums"sv, "dark"sv, "big"sv};

inline constexpr std::array<Preset, 10> presets{{
    {"Default", "Init", {}},
    {"Vocal Plate", "Vocals", vocal_plate, vocal_tags},
    {"Drum Room", "Drums", drum_room, drum_tags},
    {"Snare Chamber", "Drums", snare_chamber, snare_tags},
    {"Concert Hall", "Halls", concert_hall, hall_tags},
    {"Cathedral Wash", "Halls", cathedral_wash, wash_tags},
    {"Spring Tank", "Vintage", spring_tank, spring_tags},
    {"Reverse Swell", "Effects", reverse_swell, swell_tags},
    {"Booth", "Rooms", booth, booth_tags},
    {"Big Space", "Rooms", big_space, big_tags},
}};

inline constexpr std::array<std::string_view, presets.size()> preset_irs{
    "hall"sv,      "plate"sv,  "live_room"sv, "chamber"sv, "hall"sv,
    "cathedral"sv, "spring"sv, "hall"sv,      "booth"sv,   "warehouse"sv};

} // namespace oscillate::native_dsp::effects::cast_presets
