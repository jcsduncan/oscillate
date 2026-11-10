#pragma once

#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/stick.hpp"
#include "stick_table.hpp"

#include <array>
#include <string_view>
#include <utility>

// Stick's factory presets (ADR-0114, ADR-0121): only values that differ
// from the table defaults (the "Natural" kit: Naked Drums as its SFZ mixes
// it, nothing shortened, the chain off). The shaped kits use the mic
// chain, the parallel bus and the kit bus as an engineer would on these
// mics: record-ready, not effected.
namespace oscillate::native_dsp::synth::stick_presets {

using V = std::pair<std::string_view, double>;
using namespace std::string_view_literals;

constexpr double kit(stick::Kit k) { return static_cast<double>(k); }
constexpr double mix(stick::Mix m) { return static_cast<double>(m); }
constexpr double model(stick::SatModel m) { return static_cast<double>(m); }
constexpr double on = 1.0;
constexpr double off_db = stick_table::off_db;

// House: a short, punchy kick with its sub forward, a crisp snare, the
// rooms closed, glued.
inline constexpr std::array house_tight{
    V{"stick.humanise", 0.04},
    V{"stick.kick.decay", 0.32},
    V{"stick.snare.decay", 0.42},
    V{"stick.tom1.decay", 0.5},
    V{"stick.tom2.decay", 0.5},
    V{"stick.tom3.decay", 0.5},
    V{"stick.tom4.decay", 0.5},
    V{"stick.tom5.decay", 0.5},
    V{"stick.hihat.decay", 0.6},
    V{"stick.mic.kick_sub.level", 3.0},
    V{"stick.mic.kick_in.eq", on},
    V{"stick.mic.kick_in.low_cut", 30.0},
    V{"stick.mic.kick_in.mid_freq", 380.0},
    V{"stick.mic.kick_in.mid", -5.0},
    V{"stick.mic.kick_in.high", 2.5},
    V{"stick.mic.kick_in.comp", on},
    V{"stick.mic.kick_in.comp_threshold", -22.0},
    V{"stick.mic.kick_in.comp_ratio", 4.0},
    V{"stick.mic.kick_in.comp_attack", 12.0},
    V{"stick.mic.kick_in.comp_release", 70.0},
    V{"stick.mic.kick_in.comp_makeup", 4.0},
    V{"stick.mic.kick_sub.shaper", on},
    V{"stick.mic.kick_sub.sustain", -6.0},
    V{"stick.mic.snare_top.shaper", on},
    V{"stick.mic.snare_top.attack", 4.0},
    V{"stick.mic.snare_top.sustain", -5.0},
    V{"stick.mic.hihat.level", -10.0},
    V{"stick.mic.overheads.level", -3.0},
    V{"stick.mic.overheads.eq", on},
    V{"stick.mic.overheads.low_cut", 250.0},
    V{"stick.mic.room.level", off_db},
    V{"stick.mic.far_room.level", off_db},
    V{"stick.mic.mid_side.level", -12.0},
    V{"stick.bus.comp", on},
    V{"stick.bus.comp_threshold", -16.0},
    V{"stick.bus.comp_ratio", 2.0},
    V{"stick.bus.comp_attack", 30.0},
    V{"stick.bus.comp_release", 100.0},
    V{"stick.bus.comp_makeup", 2.0},
    V{"stick.bus.eq", on},
    V{"stick.bus.low_cut", 28.0},
};

// Funk: a snappy snare that keeps its ghost notes, a round kick, a little
// room.
inline constexpr std::array funk_pocket{
    V{"stick.humanise", 0.12},
    V{"stick.level", -9.5},
    V{"stick.kick.decay", 0.55},
    V{"stick.snare.decay", 0.7},
    V{"stick.snare.curve", -0.3},
    V{"stick.hihat.curve", -0.2},
    V{"stick.mic.kick_in.eq", on},
    V{"stick.mic.kick_in.mid_freq", 350.0},
    V{"stick.mic.kick_in.mid", -3.0},
    V{"stick.mic.snare_top.comp", on},
    V{"stick.mic.snare_top.comp_threshold", -26.0},
    V{"stick.mic.snare_top.comp_ratio", 3.0},
    V{"stick.mic.snare_top.comp_attack", 6.0},
    V{"stick.mic.snare_top.comp_release", 60.0},
    V{"stick.mic.snare_top.comp_makeup", 4.0},
    V{"stick.mic.snare_top.shaper", on},
    V{"stick.mic.snare_top.attack", 5.0},
    V{"stick.mic.snare_top.eq", on},
    V{"stick.mic.snare_top.mid_freq", 220.0},
    V{"stick.mic.snare_top.mid", 2.0},
    V{"stick.mic.snare_top.high", 2.0},
    V{"stick.mic.snare_bottom.level", 2.0},
    V{"stick.mic.room.level", -8.0},
    V{"stick.mic.far_room.level", off_db},
    V{"stick.mic.mid_side.level", -10.0},
    V{"stick.bus.comp", on},
    V{"stick.bus.comp_threshold", -18.0},
    V{"stick.bus.comp_ratio", 1.8},
    V{"stick.bus.comp_attack", 40.0},
    V{"stick.bus.comp_release", 150.0},
    V{"stick.bus.comp_makeup", 1.5},
};

// Breaks: the kit smashed in parallel under itself, its distant mics
// driven, peaks clipped.
inline constexpr std::array break_crunch{
    V{"stick.humanise", 0.15},
    V{"stick.kick.decay", 0.6},
    V{"stick.snare.decay", 0.75},
    V{"stick.mic.kick_in.parallel", 0.0},
    V{"stick.mic.snare_top.parallel", 0.0},
    V{"stick.mic.snare_bottom.parallel", -6.0},
    V{"stick.mic.overheads.parallel", -3.0},
    V{"stick.mic.room.parallel", 0.0},
    V{"stick.parallel.threshold", -38.0},
    V{"stick.parallel.ratio", 12.0},
    V{"stick.parallel.attack", 0.5},
    V{"stick.parallel.release", 120.0},
    V{"stick.parallel.makeup", 16.0},
    V{"stick.parallel.level", -3.0},
    V{"stick.mic.snare_top.sat", on},
    V{"stick.mic.snare_top.sat_model", model(stick::SatModel::tube)},
    V{"stick.mic.snare_top.sat_drive", 9.0},
    V{"stick.mic.overheads.sat", on},
    V{"stick.mic.overheads.sat_model", model(stick::SatModel::tape)},
    V{"stick.mic.overheads.sat_drive", 10.0},
    V{"stick.mic.room.sat", on},
    V{"stick.mic.room.sat_model", model(stick::SatModel::transistor)},
    V{"stick.mic.room.sat_drive", 14.0},
    V{"stick.mic.room.sat_mix", 0.6},
    V{"stick.mic.far_room.level", -6.0},
    V{"stick.bus.eq", on},
    V{"stick.bus.low_cut", 35.0},
    V{"stick.bus.mid_freq", 900.0},
    V{"stick.bus.mid", 1.5},
    V{"stick.bus.clip", on},
    V{"stick.bus.clip_drive", 4.0},
};

// Lo-fi: band-limited, tape-saturated, near mono (the widest pairs out,
// the close mics centred), loosely played.
inline constexpr std::array lofi_dust{
    V{"stick.humanise", 0.3},
    V{"stick.level", -3.0},
    V{"stick.kick.decay", 0.6},
    V{"stick.snare.decay", 0.65},
    V{"stick.hihat.pan", 0.0},
    V{"stick.ride.pan", 0.0},
    V{"stick.tom1.pan", -0.15},
    V{"stick.tom2.pan", -0.08},
    V{"stick.tom4.pan", 0.08},
    V{"stick.tom5.pan", 0.15},
    V{"stick.mic.overheads.level", -6.0},
    V{"stick.mic.room.level", 3.0},
    V{"stick.mic.far_room.level", off_db},
    V{"stick.mic.mid_side.level", off_db},
    V{"stick.mic.china_oh.level", off_db},
    V{"stick.mic.kick_in.sat", on},
    V{"stick.mic.kick_in.sat_model", model(stick::SatModel::tape)},
    V{"stick.mic.kick_in.sat_drive", 10.0},
    V{"stick.mic.snare_top.comp", on},
    V{"stick.mic.snare_top.comp_threshold", -24.0},
    V{"stick.mic.snare_top.comp_ratio", 4.0},
    V{"stick.mic.snare_top.comp_makeup", 4.0},
    V{"stick.mic.room.sat", on},
    V{"stick.mic.room.sat_model", model(stick::SatModel::tape)},
    V{"stick.mic.room.sat_drive", 16.0},
    V{"stick.bus.comp", on},
    V{"stick.bus.comp_threshold", -20.0},
    V{"stick.bus.comp_ratio", 3.0},
    V{"stick.bus.comp_attack", 20.0},
    V{"stick.bus.comp_release", 200.0},
    V{"stick.bus.comp_makeup", 3.0},
    V{"stick.bus.eq", on},
    V{"stick.bus.low_cut", 90.0},
    V{"stick.bus.mid_freq", 1'400.0},
    V{"stick.bus.mid", 3.0},
    V{"stick.bus.high_cut", 5'500.0},
    V{"stick.bus.clip", on},
    V{"stick.bus.clip_drive", 6.0},
};

// The big room: the rooms open and compressed, their tails gated short.
inline constexpr std::array big_room{
    V{"stick.humanise", 0.1},
    V{"stick.level", -8.5},
    V{"stick.mic.room.level", 4.0},
    V{"stick.mic.far_room.level", 4.0},
    V{"stick.mic.mid_side.level", 2.0},
    V{"stick.mic.far_room.comp", on},
    V{"stick.mic.far_room.comp_threshold", -32.0},
    V{"stick.mic.far_room.comp_ratio", 6.0},
    V{"stick.mic.far_room.comp_attack", 8.0},
    V{"stick.mic.far_room.comp_release", 200.0},
    V{"stick.mic.far_room.comp_makeup", 8.0},
    V{"stick.mic.far_room.gate", on},
    V{"stick.mic.far_room.gate_threshold", -34.0},
    V{"stick.mic.far_room.gate_release", 180.0},
    V{"stick.mic.room.gate", on},
    V{"stick.mic.room.gate_threshold", -36.0},
    V{"stick.mic.room.gate_release", 160.0},
    V{"stick.mic.kick_in.eq", on},
    V{"stick.mic.kick_in.mid_freq", 400.0},
    V{"stick.mic.kick_in.mid", -4.0},
    V{"stick.snare.room", 3.0},
    V{"stick.bus.comp", on},
    V{"stick.bus.comp_threshold", -14.0},
    V{"stick.bus.comp_ratio", 2.0},
    V{"stick.bus.comp_attack", 30.0},
    V{"stick.bus.comp_release", 120.0},
    V{"stick.bus.comp_makeup", 2.0},
};

// DRS Studio, mixed as DrumGizmo's kit sits best: its kick's back mic,
// snare bottom, toms, hi-hat, ride and room down, its cymbals spread.
inline constexpr std::array drs_studio{
    V{"stick.kit", kit(stick::Kit::drs)},
    V{"stick.humanise", 0.15},
    V{"stick.mic.kick_out.level", -3.0},
    V{"stick.mic.snare_bottom.level", -6.0},
    V{"stick.mic.toms.level", -2.0},
    V{"stick.mic.hihat.level", -6.0},
    V{"stick.mic.ride.level", -6.0},
    V{"stick.mic.room.level", -6.0},
    V{"stick.hihat.pan", -0.45},
    V{"stick.tom1.pan", -0.2},
    V{"stick.tom2.pan", 0.1},
    V{"stick.tom3.pan", 0.35},
    V{"stick.crash1.pan", -0.5},
    V{"stick.crash2.pan", 0.45},
    V{"stick.ride.pan", 0.55},
};

// DRS Studio played with brushes (its whisker recordings), light touches
// lifted.
inline constexpr std::array brushes{
    V{"stick.kit", kit(stick::Kit::drs)},
    V{"stick.striker", static_cast<double>(stick::Striker::brush)},
    V{"stick.humanise", 0.25},
    V{"stick.mic.kick_out.level", -3.0},
    V{"stick.mic.snare_bottom.level", -6.0},
    V{"stick.mic.hihat.level", -6.0},
    V{"stick.mic.ride.level", -6.0},
    V{"stick.mic.room.level", -6.0},
    V{"stick.hihat.pan", -0.45},
    V{"stick.ride.pan", 0.55},
    V{"stick.snare.curve", -0.4},
    V{"stick.hihat.curve", -0.4},
    V{"stick.ride.curve", -0.4},
    V{"stick.kick.level", -4.0},
    V{"stick.level", -1.0},
};

inline constexpr std::array natural_tags{"drums"sv, "acoustic"sv, "natural"sv};
inline constexpr std::array house_tags{"drums"sv, "acoustic"sv, "house"sv, "tight"sv};
inline constexpr std::array funk_tags{"drums"sv, "acoustic"sv, "funk"sv};
inline constexpr std::array break_tags{"drums"sv, "acoustic"sv, "breaks"sv, "parallel"sv};
inline constexpr std::array lofi_tags{"drums"sv, "acoustic"sv, "lo-fi"sv};
inline constexpr std::array room_tags{"drums"sv, "acoustic"sv, "room"sv};
inline constexpr std::array drs_tags{"drums"sv, "acoustic"sv, "maple"sv};
inline constexpr std::array brush_tags{"drums"sv, "acoustic"sv, "brushes"sv};

inline constexpr std::array<Preset, 8> presets{{
    {"Natural", "Drums", {}, natural_tags},
    {"House Tight", "Drums", house_tight, house_tags},
    {"Funk Pocket", "Drums", funk_pocket, funk_tags},
    {"Break Crunch", "Drums", break_crunch, break_tags},
    {"Lo-Fi Dust", "Drums", lofi_dust, lofi_tags},
    {"Big Room", "Drums", big_room, room_tags},
    {"DRS Studio", "Drums", drs_studio, drs_tags},
    {"Brushes", "Drums", brushes, brush_tags},
}};

} // namespace oscillate::native_dsp::synth::stick_presets
