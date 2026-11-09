#pragma once

#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/stick.hpp"

#include <array>
#include <cstddef>
#include <string_view>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Stick's parameter table (ADR-0114): the kit-wide controls, then each mic
// group's level, mute and solo, then each piece's seven controls. The kit,
// round robins, Humanise, Tune and the velocity curves are read when a hit
// starts; everything else plays live. Defaults are the "Tight Funk" preset.
// Ids are persisted and automated: never rename or reuse one.
namespace oscillate::native_dsp::synth::stick_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = synth_glide_seconds;
// The pedal glides as little as a foot moves, so a stepped lane still opens.
inline constexpr double pedal_glide_seconds = 0.01;

// In stick::Kit order.
inline constexpr std::array kits{"DRS Studio"sv, "Muldjord"sv, "Garage"sv};
// In stick::Mix order.
inline constexpr std::array mixes{"Dry"sv, "Studio"sv, "Room"sv};
inline constexpr std::array switches{"Off"sv, "On"sv};
// In stick::Striker order.
inline constexpr std::array strikers{"Sticks"sv, "Brushes"sv};

inline constexpr double min_level_db = -60.0;
inline constexpr double max_level_db = 12.0;
inline constexpr double max_send_db = 6.0;
inline constexpr double default_humanise = 0.18;
inline constexpr double default_level_db = -3.0;
inline constexpr double natural_decay = 1.0;
// Levels at the bottom of their range are off.
inline constexpr double off_db = min_level_db;

inline constexpr std::size_t global_count = 8;
inline constexpr std::size_t mic_parameters = 3;
inline constexpr std::size_t piece_parameters = 7;
inline constexpr std::size_t parameter_count =
    global_count + stick::mic_group_count * mic_parameters + stick::piece_count * piece_parameters;

// Where the mic and piece controls start, and each one's place in its run.
inline constexpr std::size_t first_mic = global_count;
inline constexpr std::size_t first_piece = first_mic + stick::mic_group_count * mic_parameters;
namespace mic {
inline constexpr std::size_t level = 0;
inline constexpr std::size_t mute = 1;
inline constexpr std::size_t solo = 2;
} // namespace mic
namespace piece {
inline constexpr std::size_t tune = 0;
inline constexpr std::size_t decay = 1;
inline constexpr std::size_t level = 2;
inline constexpr std::size_t pan = 3;
inline constexpr std::size_t curve = 4;
inline constexpr std::size_t overheads = 5;
inline constexpr std::size_t room = 6;
} // namespace piece

[[nodiscard]] constexpr std::size_t mic_index(stick::MicGroup group, std::size_t control) {
  return first_mic + static_cast<std::size_t>(group) * mic_parameters + control;
}
[[nodiscard]] constexpr std::size_t piece_index(stick::Piece p, std::size_t control) {
  return first_piece + static_cast<std::size_t>(p) * piece_parameters + control;
}

// A mic group's controls: its level (default `level`), mute and solo.
#define STICK_MIC(ID, NAME, LEVEL)                                                                  \
  {"stick.mic." ID ".level", "Level", NAME, "dB", S::linear, min_level_db, max_level_db, LEVEL, {}, \
   T::live, g},                                                                                     \
  {"stick.mic." ID ".mute", "Mute", NAME, "", S::choice, 0, last(switches.size()), 0, switches,     \
   T::live, 0, false},                                                                              \
  {"stick.mic." ID ".solo", "Solo", NAME, "", S::choice, 0, last(switches.size()), 0, switches,     \
   T::live, 0, false}

// A piece's controls, with its Decay, Pan and sends by default.
#define STICK_PIECE(ID, NAME, DECAY, PAN, OVERHEADS, ROOM)                                         \
  {"stick." ID ".tune", "Tune", NAME, "st", S::linear, -stick::max_tune_semitones,                  \
   stick::max_tune_semitones, 0, {}, T::next_note, 0, false},                                       \
  {"stick." ID ".decay", "Decay", NAME, "%", S::linear, stick::min_decay, natural_decay, DECAY, {}, \
   T::live, g},                                                                                     \
  {"stick." ID ".level", "Level", NAME, "dB", S::linear, min_level_db, max_level_db, 0, {},         \
   T::live, g},                                                                                     \
  {"stick." ID ".pan", "Pan", NAME, "", S::linear, -1, 1, PAN, {}, T::live, g},                     \
  {"stick." ID ".curve", "Velocity curve", NAME, "", S::linear, -stick::max_curve,                  \
   stick::max_curve, 0, {}, T::next_note, 0, false},                                               \
  {"stick." ID ".overheads", "Overheads", NAME, "dB", S::linear, off_db, max_send_db, OVERHEADS, {},\
   T::live, g},                                                                                     \
  {"stick." ID ".room", "Room", NAME, "dB", S::linear, off_db, max_send_db, ROOM, {}, T::live, g}

inline constexpr std::array<ParameterDescriptor, parameter_count> parameters{{
    // Kit
    {"stick.kit", "Kit", "Kit", "", S::choice, 0, last(kits.size()),
     static_cast<double>(stick::Kit::drs), kits, T::next_note, 0, false},
    {"stick.mix", "Mix", "Kit", "", S::choice, 0, last(mixes.size()),
     static_cast<double>(stick::Mix::studio), mixes, T::live, 0, false},
    {"stick.bleed", "Bleed", "Mics", "", S::choice, 0, last(switches.size()), 1, switches, T::live,
     0, false},
    // The hi-hat pedal, a performance lane: 0 closed, 1 open.
    {"stick.perf.pedal", "Hi-hat pedal", "Kit", "%", S::linear, 0, 1, 0, {}, T::live,
     pedal_glide_seconds},
    {"stick.humanise", "Humanise", "Kit", "%", S::linear, 0, 1, default_humanise, {},
     T::next_note, 0, false},
    {"stick.round_robin", "Round robins", "Kit", "", S::choice, 0, last(switches.size()), 1,
     switches, T::next_note, 0, false},
    {"stick.striker", "Strikers", "Kit", "", S::choice, 0, last(strikers.size()),
     static_cast<double>(stick::Striker::stick), strikers, T::next_note, 0, false},
    {"stick.level", "Level", "Kit", "dB", S::linear, min_level_db, max_level_db, default_level_db,
     {}, T::live, g},
    // Mics, in stick::MicGroup order.
    STICK_MIC("kick_in", "Kick In", 0),
    STICK_MIC("kick_out", "Kick Out", -3),
    STICK_MIC("snare_top", "Snare Top", 0),
    STICK_MIC("snare_bottom", "Snare Bottom", -6),
    STICK_MIC("toms", "Toms", -2),
    STICK_MIC("hihat", "Hi-hat", -6),
    STICK_MIC("ride", "Ride", -6),
    STICK_MIC("overheads", "Overheads", 0),
    STICK_MIC("room", "Room", -6),
    // Pieces, in stick::Piece order; pans from the drummer's seat.
    STICK_PIECE("kick", "Kick", 0.45, 0, -6, -6),
    STICK_PIECE("snare", "Snare", 0.62, -0.1, 0, 0),
    STICK_PIECE("hihat", "Hi-hat", 1, -0.45, 0, -6),
    STICK_PIECE("tom1", "Tom 1", 0.7, -0.2, 0, 0),
    STICK_PIECE("tom2", "Tom 2", 0.7, 0.1, 0, 0),
    STICK_PIECE("tom3", "Tom 3", 0.7, 0.35, 0, 0),
    STICK_PIECE("tom4", "Tom 4", 0.7, 0.5, 0, 0),
    STICK_PIECE("crash1", "Crash 1", 1, -0.5, 0, 0),
    STICK_PIECE("crash2", "Crash 2", 1, 0.45, 0, 0),
    STICK_PIECE("ride", "Ride", 1, 0.55, 0, -6),
    STICK_PIECE("ride2", "Ride 2", 1, -0.55, 0, -6),
    STICK_PIECE("china", "China", 1, 0.7, 0, 0),
}};

#undef STICK_MIC
#undef STICK_PIECE

consteval std::size_t index_of(std::string_view id) {
  return parameter_index(parameters, id);
}

static_assert(kits.size() == stick::kit_count, "a name per stick::Kit, in order");
static_assert(mixes.size() == stick::mix_count, "a name per stick::Mix, in order");
static_assert(strikers.size() == stick::striker_count, "a name per stick::Striker, in order");
static_assert(index_of("stick.level") + 1 == first_mic, "the kit's controls come first");
static_assert(index_of("stick.mic.kick_in.level") == mic_index(stick::MicGroup::kick_in, mic::level));
static_assert(index_of("stick.mic.room.solo") == mic_index(stick::MicGroup::room, mic::solo));
static_assert(index_of("stick.kick.tune") == piece_index(stick::Piece::kick, piece::tune));
static_assert(index_of("stick.snare.decay") == piece_index(stick::Piece::snare, piece::decay));
static_assert(index_of("stick.tom4.pan") == piece_index(stick::Piece::tom4, piece::pan));
static_assert(index_of("stick.china.room") == piece_index(stick::Piece::china, piece::room));
static_assert(index_of("stick.china.room") + 1 == parameter_count, "the pieces close the table");

} // namespace oscillate::native_dsp::synth::stick_table
// clang-format on
