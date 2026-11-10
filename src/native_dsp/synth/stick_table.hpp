#pragma once

#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/stick.hpp"

#include <array>
#include <cstddef>
#include <string_view>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Stick's parameter table (ADR-0114, ADR-0121): the kit-wide controls, then
// each mic group's level, mute, solo and insert chain (gate, EQ, compressor,
// transient shaper, saturation, parallel send), then each piece's seven
// controls, then the parallel compression bus and the kit bus.
// The kit, round robins, Humanise, Tune and the velocity curves are read
// when a hit starts; everything else plays live. Defaults are the "Natural"
// preset: Naked Drums as its SFZ mixes it.
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
inline constexpr std::array kits{"Naked Drums"sv, "DRS Studio"sv};
// In stick::Mix order.
inline constexpr std::array mixes{"Dry"sv, "Studio"sv, "Room"sv};
inline constexpr std::array switches{"Off"sv, "On"sv};
// In stick::Striker order.
inline constexpr std::array strikers{"Sticks"sv, "Brushes"sv};
// In stick::SatModel order.
inline constexpr std::array sat_models{"Tape"sv, "Tube"sv, "Transistor"sv, "Clip"sv};

inline constexpr double min_level_db = -60.0;
inline constexpr double max_level_db = 12.0;
inline constexpr double max_send_db = 6.0;
inline constexpr double default_level_db = -6.0;
// Naked's hi-hat, ride and china mics sit this far down in its SFZ's mix.
inline constexpr double cymbal_mic_db = -14.0;
inline constexpr double natural_decay = 1.0;
// Levels at the bottom of their range are off.
inline constexpr double off_db = min_level_db;

// ---- The production chain (ADR-0121) ----
// The gate's threshold and release.
inline constexpr double gate_min_db = -80.0;
inline constexpr double gate_default_db = -40.0;
inline constexpr double gate_release_min_ms = 5.0;
inline constexpr double gate_release_max_ms = 1'000.0;
inline constexpr double gate_release_default_ms = 80.0;
// The EQ: a low cut (off at its lowest), a low shelf, a bell, a high shelf
// and a high cut (off at its highest).
inline constexpr double low_cut_min_hz = 20.0;
inline constexpr double low_cut_max_hz = 1'000.0;
inline constexpr double high_cut_min_hz = 1'000.0;
inline constexpr double high_cut_max_hz = 20'000.0;
inline constexpr double mid_min_hz = 100.0;
inline constexpr double mid_max_hz = 10'000.0;
inline constexpr double mid_default_hz = 1'000.0;
inline constexpr double eq_max_db = 15.0;
// The compressor.
inline constexpr double comp_min_db = -60.0;
inline constexpr double comp_default_db = -18.0;
inline constexpr double ratio_min = 1.0;
inline constexpr double ratio_max = 20.0;
inline constexpr double ratio_default = 4.0;
inline constexpr double comp_attack_min_ms = 0.1;
inline constexpr double comp_attack_max_ms = 100.0;
inline constexpr double comp_attack_default_ms = 10.0;
inline constexpr double comp_release_min_ms = 10.0;
inline constexpr double comp_release_max_ms = 1'000.0;
inline constexpr double comp_release_default_ms = 120.0;
inline constexpr double makeup_max_db = 24.0;
// The transient shaper's attack and sustain (Strike's range).
inline constexpr double shaper_max_db = 18.0;
// Saturation.
inline constexpr double drive_max_db = 36.0;
inline constexpr double drive_default_db = 6.0;
// The parallel bus: a hard, fast compressor, its return under the kit.
inline constexpr double parallel_default_db = -30.0;
inline constexpr double parallel_ratio_default = 10.0;
inline constexpr double parallel_attack_default_ms = 1.0;
inline constexpr double parallel_release_default_ms = 100.0;
inline constexpr double parallel_makeup_default_db = 12.0;
inline constexpr double parallel_return_default_db = -6.0;
// The kit bus: glue compression, EQ and a clipper.
inline constexpr double glue_default_db = -12.0;
inline constexpr double glue_ratio_default = 2.0;
inline constexpr double glue_attack_default_ms = 30.0;
inline constexpr double glue_release_default_ms = 150.0;
inline constexpr double clip_max_db = 18.0;
inline constexpr double clip_default_db = 3.0;

inline constexpr std::size_t global_count = 8;
inline constexpr std::size_t mic_parameters = 27;
inline constexpr std::size_t piece_parameters = 7;
inline constexpr std::size_t eq_parameters = 7;
inline constexpr std::size_t bus_parameters = 6 + 6 + eq_parameters + 2;
inline constexpr std::size_t parameter_count =
    global_count + stick::mic_group_count * mic_parameters +
    stick::piece_count * piece_parameters + bus_parameters;

// Where the mic and piece controls start, and each one's place in its run.
inline constexpr std::size_t first_mic = global_count;
inline constexpr std::size_t first_piece = first_mic + stick::mic_group_count * mic_parameters;
inline constexpr std::size_t first_bus = first_piece + stick::piece_count * piece_parameters;
namespace mic {
inline constexpr std::size_t level = 0;
inline constexpr std::size_t mute = 1;
inline constexpr std::size_t solo = 2;
inline constexpr std::size_t gate = 3;
inline constexpr std::size_t gate_threshold = 4;
inline constexpr std::size_t gate_release = 5;
inline constexpr std::size_t eq = 6; // Then the EQ's controls in eq:: order.
inline constexpr std::size_t comp = 13;
inline constexpr std::size_t comp_threshold = 14;
inline constexpr std::size_t comp_ratio = 15;
inline constexpr std::size_t comp_attack = 16;
inline constexpr std::size_t comp_release = 17;
inline constexpr std::size_t comp_makeup = 18;
inline constexpr std::size_t shaper = 19;
inline constexpr std::size_t attack = 20;
inline constexpr std::size_t sustain = 21;
inline constexpr std::size_t sat = 22;
inline constexpr std::size_t sat_model = 23;
inline constexpr std::size_t sat_drive = 24;
inline constexpr std::size_t sat_mix = 25;
inline constexpr std::size_t parallel = 26;
} // namespace mic
// An EQ's controls, from its switch: the group's and the kit bus's.
namespace eq {
inline constexpr std::size_t on = 0;
inline constexpr std::size_t low_cut = 1;
inline constexpr std::size_t low = 2;
inline constexpr std::size_t mid_freq = 3;
inline constexpr std::size_t mid = 4;
inline constexpr std::size_t high = 5;
inline constexpr std::size_t high_cut = 6;
} // namespace eq
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

// An on/off switch, and an EQ's controls (PREFIX "stick.mic.kick_in." or "stick.bus.").
#define STICK_SWITCH(ID, NAME, GROUP)                                                                  \
  {ID, NAME, GROUP, "", S::choice, 0, last(switches.size()), 0, switches, T::live, 0, false}
#define STICK_EQ(PREFIX, GROUP)                                                                        \
  STICK_SWITCH(PREFIX "eq", "EQ", GROUP),                                                              \
  {PREFIX "low_cut", "Low cut", GROUP, "Hz", S::logarithmic, low_cut_min_hz, low_cut_max_hz,           \
   low_cut_min_hz, {}, T::live, g},                                                                    \
  {PREFIX "low", "Low", GROUP, "dB", S::linear, -eq_max_db, eq_max_db, 0, {}, T::live, g},              \
  {PREFIX "mid_freq", "Mid freq", GROUP, "Hz", S::logarithmic, mid_min_hz, mid_max_hz, mid_default_hz,  \
   {}, T::live, g},                                                                                    \
  {PREFIX "mid", "Mid", GROUP, "dB", S::linear, -eq_max_db, eq_max_db, 0, {}, T::live, g},              \
  {PREFIX "high", "High", GROUP, "dB", S::linear, -eq_max_db, eq_max_db, 0, {}, T::live, g},            \
  {PREFIX "high_cut", "High cut", GROUP, "Hz", S::logarithmic, high_cut_min_hz, high_cut_max_hz,        \
   high_cut_max_hz, {}, T::live, g}
// A compressor's controls after its switch.
#define STICK_COMP(PREFIX, GROUP, THRESHOLD, RATIO, ATTACK, RELEASE, MAKEUP)                            \
  {PREFIX "threshold", "Threshold", GROUP, "dB", S::linear, comp_min_db, 0, THRESHOLD, {}, T::live, g}, \
  {PREFIX "ratio", "Ratio", GROUP, "", S::logarithmic, ratio_min, ratio_max, RATIO, {}, T::live, g},    \
  {PREFIX "attack", "Attack", GROUP, "ms", S::logarithmic, comp_attack_min_ms, comp_attack_max_ms,      \
   ATTACK, {}, T::live, g},                                                                            \
  {PREFIX "release", "Release", GROUP, "ms", S::logarithmic, comp_release_min_ms,                       \
   comp_release_max_ms, RELEASE, {}, T::live, g},                                                      \
  {PREFIX "makeup", "Makeup", GROUP, "dB", S::linear, 0, makeup_max_db, MAKEUP, {}, T::live, g}

// A mic group's controls: its level (default `level`), mute, solo and
// insert chain, every stage off.
#define STICK_MIC(ID, NAME, LEVEL)                                                                  \
  {"stick.mic." ID ".level", "Level", NAME, "dB", S::linear, min_level_db, max_level_db, LEVEL, {}, \
   T::live, g},                                                                                     \
  {"stick.mic." ID ".mute", "Mute", NAME, "", S::choice, 0, last(switches.size()), 0, switches,     \
   T::live, 0, false},                                                                              \
  {"stick.mic." ID ".solo", "Solo", NAME, "", S::choice, 0, last(switches.size()), 0, switches,     \
   T::live, 0, false},                                                                              \
  STICK_SWITCH("stick.mic." ID ".gate", "Gate", NAME),                                              \
  {"stick.mic." ID ".gate_threshold", "Gate threshold", NAME, "dB", S::linear, gate_min_db, 0,      \
   gate_default_db, {}, T::live, g},                                                                \
  {"stick.mic." ID ".gate_release", "Gate release", NAME, "ms", S::logarithmic,                     \
   gate_release_min_ms, gate_release_max_ms, gate_release_default_ms, {}, T::live, g},              \
  STICK_EQ("stick.mic." ID ".", NAME),                                                              \
  STICK_SWITCH("stick.mic." ID ".comp", "Compressor", NAME),                                        \
  STICK_COMP("stick.mic." ID ".comp_", NAME, comp_default_db, ratio_default,                        \
             comp_attack_default_ms, comp_release_default_ms, 0),                                   \
  STICK_SWITCH("stick.mic." ID ".shaper", "Transients", NAME),                                      \
  {"stick.mic." ID ".attack", "Attack", NAME, "dB", S::linear, -shaper_max_db, shaper_max_db, 0,    \
   {}, T::live, g},                                                                                 \
  {"stick.mic." ID ".sustain", "Sustain", NAME, "dB", S::linear, -shaper_max_db, shaper_max_db, 0,  \
   {}, T::live, g},                                                                                 \
  STICK_SWITCH("stick.mic." ID ".sat", "Saturation", NAME),                                         \
  {"stick.mic." ID ".sat_model", "Model", NAME, "", S::choice, 0, last(sat_models.size()), 0,       \
   sat_models, T::live, 0, false},                                                                  \
  {"stick.mic." ID ".sat_drive", "Drive", NAME, "dB", S::linear, 0, drive_max_db,                   \
   drive_default_db, {}, T::live, g},                                                               \
  {"stick.mic." ID ".sat_mix", "Mix", NAME, "%", S::linear, 0, 1, 1, {}, T::live, g},               \
  {"stick.mic." ID ".parallel", "Parallel", NAME, "dB", S::linear, off_db, max_send_db, off_db, {}, \
   T::live, g}

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
     static_cast<double>(stick::Kit::naked), kits, T::next_note, 0, false},
    {"stick.mix", "Mix", "Kit", "", S::choice, 0, last(mixes.size()),
     static_cast<double>(stick::Mix::studio), mixes, T::live, 0, false},
    {"stick.bleed", "Bleed", "Mics", "", S::choice, 0, last(switches.size()), 1, switches, T::live,
     0, false},
    // The hi-hat pedal, a performance lane: 0 closed, 1 open.
    {"stick.perf.pedal", "Hi-hat pedal", "Kit", "%", S::linear, 0, 1, 0, {}, T::live,
     pedal_glide_seconds},
    {"stick.humanise", "Humanise", "Kit", "%", S::linear, 0, 1, 0, {}, T::next_note, 0, false},
    {"stick.round_robin", "Round robins", "Kit", "", S::choice, 0, last(switches.size()), 1,
     switches, T::next_note, 0, false},
    {"stick.striker", "Strikers", "Kit", "", S::choice, 0, last(strikers.size()),
     static_cast<double>(stick::Striker::stick), strikers, T::next_note, 0, false},
    {"stick.level", "Level", "Kit", "dB", S::linear, min_level_db, max_level_db, default_level_db,
     {}, T::live, g},
    // Mics, in stick::MicGroup order.
    STICK_MIC("kick_in", "Kick In", 0),
    STICK_MIC("kick_out", "Kick Out", 0),
    STICK_MIC("kick_sub", "Kick Sub", 0),
    STICK_MIC("snare_top", "Snare Top", 0),
    STICK_MIC("snare_bottom", "Snare Bottom", 0),
    STICK_MIC("toms", "Toms", 0),
    STICK_MIC("hihat", "Hi-hat", cymbal_mic_db),
    STICK_MIC("ride", "Ride", cymbal_mic_db),
    STICK_MIC("overheads", "Overheads", 0),
    STICK_MIC("room", "Room", 0),
    STICK_MIC("far_room", "Far Room", 0),
    STICK_MIC("mid_side", "Mid-Side", 0),
    STICK_MIC("china_oh", "China OH", cymbal_mic_db),
    // Pieces, in stick::Piece order, as recorded; pans from the drummer's
    // seat, as Naked's SFZ places its close mics.
    STICK_PIECE("kick", "Kick", natural_decay, 0, 0, 0),
    STICK_PIECE("snare", "Snare", natural_decay, 0, 0, 0),
    STICK_PIECE("hihat", "Hi-hat", natural_decay, -0.3, 0, 0),
    STICK_PIECE("tom1", "Tom 1", natural_decay, -0.6, 0, 0),
    STICK_PIECE("tom2", "Tom 2", natural_decay, -0.3, 0, 0),
    STICK_PIECE("tom3", "Tom 3", natural_decay, 0, 0, 0),
    STICK_PIECE("tom4", "Tom 4", natural_decay, 0.3, 0, 0),
    STICK_PIECE("tom5", "Tom 5", natural_decay, 0.6, 0, 0),
    STICK_PIECE("crash1", "Crash 1", natural_decay, 0, 0, 0),
    STICK_PIECE("crash2", "Crash 2", natural_decay, 0, 0, 0),
    STICK_PIECE("ride", "Ride", natural_decay, 0.3, 0, 0),
    STICK_PIECE("splash", "Splash", natural_decay, 0, 0, 0),
    STICK_PIECE("china", "China", natural_decay, 0, 0, 0),
    // The parallel compression bus: each group's Parallel send, compressed
    // hard and returned under the kit.
    STICK_COMP("stick.parallel.", "Parallel", parallel_default_db, parallel_ratio_default,
               parallel_attack_default_ms, parallel_release_default_ms, parallel_makeup_default_db),
    {"stick.parallel.level", "Return", "Parallel", "dB", S::linear, off_db, max_send_db,
     parallel_return_default_db, {}, T::live, g},
    // The kit bus: glue compression, EQ and a clipper, in that order.
    STICK_SWITCH("stick.bus.comp", "Glue", "Kit bus"),
    STICK_COMP("stick.bus.comp_", "Kit bus", glue_default_db, glue_ratio_default,
               glue_attack_default_ms, glue_release_default_ms, 0),
    STICK_EQ("stick.bus.", "Kit bus"),
    STICK_SWITCH("stick.bus.clip", "Clip", "Kit bus"),
    {"stick.bus.clip_drive", "Clip drive", "Kit bus", "dB", S::linear, 0, clip_max_db,
     clip_default_db, {}, T::live, g},
}};

#undef STICK_MIC
#undef STICK_PIECE
#undef STICK_SWITCH
#undef STICK_EQ
#undef STICK_COMP

consteval std::size_t index_of(std::string_view id) {
  return parameter_index(parameters, id);
}

static_assert(kits.size() == stick::kit_count, "a name per stick::Kit, in order");
static_assert(mixes.size() == stick::mix_count, "a name per stick::Mix, in order");
static_assert(strikers.size() == stick::striker_count, "a name per stick::Striker, in order");
static_assert(sat_models.size() == stick::sat_model_count, "a name per stick::SatModel, in order");
static_assert(index_of("stick.level") + 1 == first_mic, "the kit's controls come first");
static_assert(index_of("stick.mic.kick_in.level") == mic_index(stick::MicGroup::kick_in, mic::level));
static_assert(index_of("stick.mic.china_oh.solo") == mic_index(stick::MicGroup::china_oh, mic::solo));
static_assert(index_of("stick.kick.tune") == piece_index(stick::Piece::kick, piece::tune));
static_assert(index_of("stick.snare.decay") == piece_index(stick::Piece::snare, piece::decay));
static_assert(index_of("stick.tom5.pan") == piece_index(stick::Piece::tom5, piece::pan));
static_assert(index_of("stick.china.room") == piece_index(stick::Piece::china, piece::room));
static_assert(index_of("stick.china.room") + 1 == first_bus, "the buses follow the pieces");
static_assert(index_of("stick.mic.kick_in.gate") == mic_index(stick::MicGroup::kick_in, mic::gate));
static_assert(index_of("stick.mic.kick_in.eq") == mic_index(stick::MicGroup::kick_in, mic::eq));
static_assert(index_of("stick.mic.kick_in.high_cut") ==
              mic_index(stick::MicGroup::kick_in, mic::eq + eq::high_cut));
static_assert(index_of("stick.mic.kick_in.comp") == mic_index(stick::MicGroup::kick_in, mic::comp));
static_assert(index_of("stick.mic.kick_in.comp_makeup") ==
              mic_index(stick::MicGroup::kick_in, mic::comp_makeup));
static_assert(index_of("stick.mic.kick_in.shaper") ==
              mic_index(stick::MicGroup::kick_in, mic::shaper));
static_assert(index_of("stick.mic.kick_in.sat_mix") ==
              mic_index(stick::MicGroup::kick_in, mic::sat_mix));
static_assert(index_of("stick.mic.china_oh.parallel") ==
              mic_index(stick::MicGroup::china_oh, mic::parallel));
static_assert(index_of("stick.bus.clip_drive") + 1 == parameter_count, "the kit bus closes the table");

} // namespace oscillate::native_dsp::synth::stick_table
// clang-format on
