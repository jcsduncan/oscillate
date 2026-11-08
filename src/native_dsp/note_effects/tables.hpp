#pragma once

#include "oscillate/native_dsp/note_effects/note_effect.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/timeline/timing.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Parameter tables of the note effects (ADR-0059), version 1. Ids are
// persisted: never rename or reuse one.
namespace oscillate::native_dsp::note_effects::tables {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }

// Note lengths, shortest first, in exact project ticks.
inline constexpr std::array divisions{"1/32T"sv, "1/32"sv, "1/16T"sv, "1/16"sv, "1/8T"sv,
                                      "1/16D"sv, "1/8"sv,  "1/4T"sv,  "1/8D"sv, "1/4"sv,
                                      "1/2T"sv,  "1/4D"sv, "1/2"sv,   "1/2D"sv, "1/1"sv};
inline constexpr std::int64_t quarter = timeline::ticks_per_quarter;
inline constexpr std::array<std::int64_t, divisions.size()> division_ticks{
    quarter / 12,    quarter / 8, quarter / 6,     quarter / 4, quarter / 3,
    quarter * 3 / 8, quarter / 2, quarter * 2 / 3, quarter * 3 / 4, quarter,
    quarter * 4 / 3, quarter * 3 / 2, quarter * 2, quarter * 3, quarter * 4};
inline constexpr int sixteenth = 3;
inline constexpr int eighth = 6;
inline constexpr int dotted_eighth = 8;

inline constexpr std::array roots{"C"sv,  "C#"sv, "D"sv,  "Eb"sv, "E"sv,  "F"sv,
                                  "F#"sv, "G"sv,  "Ab"sv, "A"sv,  "Bb"sv, "B"sv};
inline constexpr std::array scales{"Major"sv,          "Minor"sv,          "Dorian"sv,
                                   "Phrygian"sv,       "Lydian"sv,         "Mixolydian"sv,
                                   "Locrian"sv,        "Harmonic Minor"sv, "Melodic Minor"sv,
                                   "Major Pentatonic"sv, "Minor Pentatonic"sv, "Blues"sv,
                                   "Whole Tone"sv,     "Chromatic"sv};
// Pitch classes of each scale above its root, as a 12-bit mask.
inline constexpr std::array<std::uint16_t, scales.size()> scale_masks{
    0b1010'1011'0101, // Major: 0 2 4 5 7 9 11
    0b0101'1010'1101, // Minor: 0 2 3 5 7 8 10
    0b0110'1010'1101, // Dorian: 0 2 3 5 7 9 10
    0b0101'1010'1011, // Phrygian: 0 1 3 5 7 8 10
    0b1010'1101'0101, // Lydian: 0 2 4 6 7 9 11
    0b0110'1011'0101, // Mixolydian: 0 2 4 5 7 9 10
    0b0101'0110'1011, // Locrian: 0 1 3 5 6 8 10
    0b1001'1010'1101, // Harmonic minor: 0 2 3 5 7 8 11
    0b1010'1010'1101, // Melodic minor: 0 2 3 5 7 9 11
    0b0010'1001'0101, // Major pentatonic: 0 2 4 7 9
    0b0100'1010'1001, // Minor pentatonic: 0 3 5 7 10
    0b0100'1110'1001, // Blues: 0 3 5 6 7 10
    0b0101'0101'0101, // Whole tone: 0 2 4 6 8 10
    0b1111'1111'1111, // Chromatic
};
inline constexpr int major_scale = 0;
inline constexpr int minor_scale = 1;
inline constexpr int chromatic = static_cast<int>(scales.size()) - 1;
inline constexpr std::array off_on{"Off"sv, "On"sv};

// Arp.
namespace arp {
inline constexpr std::array modes{"Up"sv,        "Down"sv,   "Up-Down"sv, "Down-Up"sv,
                                  "As Played"sv, "Random"sv, "Chord"sv};
enum Mode : std::uint8_t { up, down, up_down, down_up, as_played, random, chord };
inline constexpr std::array velocities{"As Played"sv, "Fixed"sv};
inline constexpr std::array<ParameterDescriptor, 11> parameters{{
    {"arp.mode", "Mode", "Pattern", "", S::choice, 0, last(modes.size()), up, modes, T::live, 0},
    {"arp.rate", "Rate", "Pattern", "", S::choice, 0, last(divisions.size()), sixteenth, divisions,
     T::live, 0},
    {"arp.gate", "Gate", "Pattern", "%", S::linear, 0.05, 2.0, 0.5, {}, T::live, 0},
    {"arp.octaves", "Octaves", "Pattern", "", S::stepped, 1, 4, 1, {}, T::live, 0},
    {"arp.swing", "Swing", "Groove", "%", S::linear, 0.5, 0.75, 0.5, {}, T::live, 0},
    {"arp.steps", "Steps", "Groove", "", S::stepped, 0, 16, 0, {}, T::live, 0},
    {"arp.accent", "Accent Every", "Groove", "", S::stepped, 0, 16, 0, {}, T::live, 0},
    {"arp.velocity", "Velocity", "Velocity", "", S::choice, 0, last(velocities.size()), 0,
     velocities, T::live, 0},
    {"arp.level", "Fixed Velocity", "Velocity", "", S::stepped, 1, 127, 100, {}, T::live, 0},
    {"arp.latch", "Latch", "Hold", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"arp.seed", "Seed", "Hold", "", S::stepped, 1, 9999, 1, {}, T::live, 0},
}};
} // namespace arp

// Chord.
namespace chord {
inline constexpr std::array modes{"Intervals"sv, "In Key"sv};
inline constexpr std::array shapes{"Triad"sv, "Seventh"sv, "Sus2"sv, "Sus4"sv,
                                   "Sixth"sv, "Add9"sv,    "Ninth"sv};
inline constexpr std::array<ParameterDescriptor, 12> parameters{{
    {"chord.mode", "Mode", "Chord", "", S::choice, 0, last(modes.size()), 0, modes, T::live, 0},
    {"chord.shift1", "Shift 1", "Intervals", "st", S::stepped, -24, 24, 4, {}, T::live, 0},
    {"chord.shift2", "Shift 2", "Intervals", "st", S::stepped, -24, 24, 7, {}, T::live, 0},
    {"chord.shift3", "Shift 3", "Intervals", "st", S::stepped, -24, 24, 0, {}, T::live, 0},
    {"chord.shift4", "Shift 4", "Intervals", "st", S::stepped, -24, 24, 0, {}, T::live, 0},
    {"chord.shift5", "Shift 5", "Intervals", "st", S::stepped, -24, 24, 0, {}, T::live, 0},
    {"chord.shift6", "Shift 6", "Intervals", "st", S::stepped, -24, 24, 0, {}, T::live, 0},
    {"chord.shape", "Shape", "In Key", "", S::choice, 0, last(shapes.size()), 0, shapes, T::live,
     0},
    {"chord.inversion", "Inversion", "In Key", "", S::stepped, 0, 3, 0, {}, T::live, 0},
    {"chord.root", "Root", "In Key", "", S::choice, 0, last(roots.size()), 0, roots, T::live, 0},
    {"chord.scale", "Scale", "In Key", "", S::choice, 0, last(scales.size()), major_scale, scales,
     T::live, 0},
    {"chord.velocity", "Added Velocity", "Chord", "%", S::linear, 0, 1, 1, {}, T::live, 0},
}};
} // namespace chord

// Echo.
namespace echo {
inline constexpr std::array<ParameterDescriptor, 4> parameters{{
    {"echo.time", "Time", "Echo", "", S::choice, 0, last(divisions.size()), eighth, divisions,
     T::live, 0},
    {"echo.repeats", "Repeats", "Echo", "", S::stepped, 1, 16, 3, {}, T::live, 0},
    {"echo.decay", "Decay", "Echo", "%", S::linear, 0, 1, 0.7, {}, T::live, 0},
    {"echo.pitch", "Pitch", "Echo", "st", S::stepped, -12, 12, 0, {}, T::live, 0},
}};
} // namespace echo

// Random.
namespace random {
inline constexpr std::array<ParameterDescriptor, 7> parameters{{
    {"random.chance", "Chance", "Pitch", "%", S::linear, 0, 1, 0.5, {}, T::live, 0},
    {"random.range", "Range", "Pitch", "st", S::stepped, 1, 24, 12, {}, T::live, 0},
    {"random.root", "Root", "Pitch", "", S::choice, 0, last(roots.size()), 0, roots, T::live, 0},
    {"random.scale", "Scale", "Pitch", "", S::choice, 0, last(scales.size()), chromatic, scales,
     T::live, 0},
    {"random.velocity", "Velocity", "Feel", "%", S::linear, 0, 1, 0, {}, T::live, 0},
    {"random.timing", "Timing", "Feel", "%", S::linear, 0, 1, 0, {}, T::live, 0},
    {"random.seed", "Seed", "Feel", "", S::stepped, 1, 9999, 1, {}, T::live, 0},
}};
} // namespace random

// Scale.
namespace scale {
inline constexpr std::array snaps{"Nearest"sv, "Up"sv, "Down"sv};
enum Snap : std::uint8_t { nearest, up, down };
inline constexpr std::array<ParameterDescriptor, 5> parameters{{
    {"scale.root", "Root", "Scale", "", S::choice, 0, last(roots.size()), 0, roots, T::live, 0},
    {"scale.scale", "Scale", "Scale", "", S::choice, 0, last(scales.size()), major_scale, scales,
     T::live, 0},
    {"scale.snap", "Snap", "Scale", "", S::choice, 0, last(snaps.size()), nearest, snaps, T::live,
     0},
    {"scale.transpose", "Transpose", "Move", "steps", S::stepped, -14, 14, 0, {}, T::live, 0},
    {"scale.octave", "Octave", "Move", "oct", S::stepped, -3, 3, 0, {}, T::live, 0},
}};
} // namespace scale

// Comp (ADR-0083, ADR-0084): plays the held notes in a rhythm. Three hit
// lanes (the lowest held note, the inner ones and the highest) and a length
// lane, one step per 16th of the bar; the top lane can run shorter than the
// bar.
namespace comp {
inline constexpr int steps = note_effects::comp_steps_per_bar;
inline constexpr int max_length = 32;
inline constexpr std::array hits{"Off"sv, "Ghost"sv, "On"sv};
enum Hit : std::uint8_t { off, ghost, on };
inline constexpr std::array pushes{"1/16"sv, "1/8"sv, "1/8D"sv};
enum Push : std::uint8_t { push_sixteenth, push_eighth, push_dotted_eighth };
// The push amounts, in 16ths.
inline constexpr std::array<int, pushes.size()> push_sixteenths{1, 2, 3};
// The longest push is as far as note effects see ahead.
static_assert(push_sixteenths.back() * division_ticks[sixteenth] ==
              note_effects::note_lookahead_ticks);
inline constexpr std::array grooves{"Straight"sv, "56%"sv, "58%"sv, "62%"sv, "66%"sv};
enum Groove : std::uint8_t { straight, groove_56, groove_58, groove_62, groove_66 };
// Where the second 16th of each pair falls, as a share of the 8th.
inline constexpr std::array<double, grooves.size()> groove_swing{0.5, 0.56, 0.58, 0.62, 0.66};
// Motion (ADR-0084): small changes that keep a looped rhythm moving, each
// tied to the song position so live playback and an applied rhythm agree.
// The phrase the changes are measured in.
inline constexpr std::array phrases{"4 bars"sv, "8 bars"sv, "16 bars"sv};
enum Phrase : std::uint8_t { phrase_4, phrase_8, phrase_16 };
inline constexpr auto phrase_bars = note_effects::comp_phrase_bars;
static_assert(phrase_bars.size() == phrases.size());
inline constexpr std::array switches{"Off"sv, "On"sv};
// The default pattern is Rhodes comp: the low note on 1 and 3, the chord
// answering off the beat.
inline constexpr std::array<double, steps> low_pattern{on, off, off, off, off, off, off, off,
                                                       on, off, off, off, off, off, off, off};
inline constexpr std::array<double, steps> mid_pattern{off, off, off, on, off, off, ghost, off,
                                                       off, off, on, off, off, ghost, off, off};
inline constexpr std::array<double, steps> top_pattern{off, off, off, on, off, off, off, off,
                                                       off, off, on, off, off, off, off, off};
inline constexpr std::array<double, steps> length_pattern{3, 3, 3, 3, 3, 3, 3, 3,
                                                          3, 3, 3, 3, 3, 3, 3, 3};
inline constexpr std::array<ParameterDescriptor, 81> parameters{{
    {"comp.low.1", "Low 1", "Low", "", S::choice, 0, last(hits.size()), low_pattern[0], hits, T::live, 0},
    {"comp.low.2", "Low 2", "Low", "", S::choice, 0, last(hits.size()), low_pattern[1], hits, T::live, 0},
    {"comp.low.3", "Low 3", "Low", "", S::choice, 0, last(hits.size()), low_pattern[2], hits, T::live, 0},
    {"comp.low.4", "Low 4", "Low", "", S::choice, 0, last(hits.size()), low_pattern[3], hits, T::live, 0},
    {"comp.low.5", "Low 5", "Low", "", S::choice, 0, last(hits.size()), low_pattern[4], hits, T::live, 0},
    {"comp.low.6", "Low 6", "Low", "", S::choice, 0, last(hits.size()), low_pattern[5], hits, T::live, 0},
    {"comp.low.7", "Low 7", "Low", "", S::choice, 0, last(hits.size()), low_pattern[6], hits, T::live, 0},
    {"comp.low.8", "Low 8", "Low", "", S::choice, 0, last(hits.size()), low_pattern[7], hits, T::live, 0},
    {"comp.low.9", "Low 9", "Low", "", S::choice, 0, last(hits.size()), low_pattern[8], hits, T::live, 0},
    {"comp.low.10", "Low 10", "Low", "", S::choice, 0, last(hits.size()), low_pattern[9], hits, T::live, 0},
    {"comp.low.11", "Low 11", "Low", "", S::choice, 0, last(hits.size()), low_pattern[10], hits, T::live, 0},
    {"comp.low.12", "Low 12", "Low", "", S::choice, 0, last(hits.size()), low_pattern[11], hits, T::live, 0},
    {"comp.low.13", "Low 13", "Low", "", S::choice, 0, last(hits.size()), low_pattern[12], hits, T::live, 0},
    {"comp.low.14", "Low 14", "Low", "", S::choice, 0, last(hits.size()), low_pattern[13], hits, T::live, 0},
    {"comp.low.15", "Low 15", "Low", "", S::choice, 0, last(hits.size()), low_pattern[14], hits, T::live, 0},
    {"comp.low.16", "Low 16", "Low", "", S::choice, 0, last(hits.size()), low_pattern[15], hits, T::live, 0},
    {"comp.mid.1", "Middle 1", "Middle", "", S::choice, 0, last(hits.size()), mid_pattern[0], hits, T::live, 0},
    {"comp.mid.2", "Middle 2", "Middle", "", S::choice, 0, last(hits.size()), mid_pattern[1], hits, T::live, 0},
    {"comp.mid.3", "Middle 3", "Middle", "", S::choice, 0, last(hits.size()), mid_pattern[2], hits, T::live, 0},
    {"comp.mid.4", "Middle 4", "Middle", "", S::choice, 0, last(hits.size()), mid_pattern[3], hits, T::live, 0},
    {"comp.mid.5", "Middle 5", "Middle", "", S::choice, 0, last(hits.size()), mid_pattern[4], hits, T::live, 0},
    {"comp.mid.6", "Middle 6", "Middle", "", S::choice, 0, last(hits.size()), mid_pattern[5], hits, T::live, 0},
    {"comp.mid.7", "Middle 7", "Middle", "", S::choice, 0, last(hits.size()), mid_pattern[6], hits, T::live, 0},
    {"comp.mid.8", "Middle 8", "Middle", "", S::choice, 0, last(hits.size()), mid_pattern[7], hits, T::live, 0},
    {"comp.mid.9", "Middle 9", "Middle", "", S::choice, 0, last(hits.size()), mid_pattern[8], hits, T::live, 0},
    {"comp.mid.10", "Middle 10", "Middle", "", S::choice, 0, last(hits.size()), mid_pattern[9], hits, T::live, 0},
    {"comp.mid.11", "Middle 11", "Middle", "", S::choice, 0, last(hits.size()), mid_pattern[10], hits, T::live, 0},
    {"comp.mid.12", "Middle 12", "Middle", "", S::choice, 0, last(hits.size()), mid_pattern[11], hits, T::live, 0},
    {"comp.mid.13", "Middle 13", "Middle", "", S::choice, 0, last(hits.size()), mid_pattern[12], hits, T::live, 0},
    {"comp.mid.14", "Middle 14", "Middle", "", S::choice, 0, last(hits.size()), mid_pattern[13], hits, T::live, 0},
    {"comp.mid.15", "Middle 15", "Middle", "", S::choice, 0, last(hits.size()), mid_pattern[14], hits, T::live, 0},
    {"comp.mid.16", "Middle 16", "Middle", "", S::choice, 0, last(hits.size()), mid_pattern[15], hits, T::live, 0},
    {"comp.top.1", "Top 1", "Top", "", S::choice, 0, last(hits.size()), top_pattern[0], hits, T::live, 0},
    {"comp.top.2", "Top 2", "Top", "", S::choice, 0, last(hits.size()), top_pattern[1], hits, T::live, 0},
    {"comp.top.3", "Top 3", "Top", "", S::choice, 0, last(hits.size()), top_pattern[2], hits, T::live, 0},
    {"comp.top.4", "Top 4", "Top", "", S::choice, 0, last(hits.size()), top_pattern[3], hits, T::live, 0},
    {"comp.top.5", "Top 5", "Top", "", S::choice, 0, last(hits.size()), top_pattern[4], hits, T::live, 0},
    {"comp.top.6", "Top 6", "Top", "", S::choice, 0, last(hits.size()), top_pattern[5], hits, T::live, 0},
    {"comp.top.7", "Top 7", "Top", "", S::choice, 0, last(hits.size()), top_pattern[6], hits, T::live, 0},
    {"comp.top.8", "Top 8", "Top", "", S::choice, 0, last(hits.size()), top_pattern[7], hits, T::live, 0},
    {"comp.top.9", "Top 9", "Top", "", S::choice, 0, last(hits.size()), top_pattern[8], hits, T::live, 0},
    {"comp.top.10", "Top 10", "Top", "", S::choice, 0, last(hits.size()), top_pattern[9], hits, T::live, 0},
    {"comp.top.11", "Top 11", "Top", "", S::choice, 0, last(hits.size()), top_pattern[10], hits, T::live, 0},
    {"comp.top.12", "Top 12", "Top", "", S::choice, 0, last(hits.size()), top_pattern[11], hits, T::live, 0},
    {"comp.top.13", "Top 13", "Top", "", S::choice, 0, last(hits.size()), top_pattern[12], hits, T::live, 0},
    {"comp.top.14", "Top 14", "Top", "", S::choice, 0, last(hits.size()), top_pattern[13], hits, T::live, 0},
    {"comp.top.15", "Top 15", "Top", "", S::choice, 0, last(hits.size()), top_pattern[14], hits, T::live, 0},
    {"comp.top.16", "Top 16", "Top", "", S::choice, 0, last(hits.size()), top_pattern[15], hits, T::live, 0},
    {"comp.length.1", "Length 1", "Length", "16ths", S::stepped, 1, max_length, length_pattern[0], {}, T::live, 0},
    {"comp.length.2", "Length 2", "Length", "16ths", S::stepped, 1, max_length, length_pattern[1], {}, T::live, 0},
    {"comp.length.3", "Length 3", "Length", "16ths", S::stepped, 1, max_length, length_pattern[2], {}, T::live, 0},
    {"comp.length.4", "Length 4", "Length", "16ths", S::stepped, 1, max_length, length_pattern[3], {}, T::live, 0},
    {"comp.length.5", "Length 5", "Length", "16ths", S::stepped, 1, max_length, length_pattern[4], {}, T::live, 0},
    {"comp.length.6", "Length 6", "Length", "16ths", S::stepped, 1, max_length, length_pattern[5], {}, T::live, 0},
    {"comp.length.7", "Length 7", "Length", "16ths", S::stepped, 1, max_length, length_pattern[6], {}, T::live, 0},
    {"comp.length.8", "Length 8", "Length", "16ths", S::stepped, 1, max_length, length_pattern[7], {}, T::live, 0},
    {"comp.length.9", "Length 9", "Length", "16ths", S::stepped, 1, max_length, length_pattern[8], {}, T::live, 0},
    {"comp.length.10", "Length 10", "Length", "16ths", S::stepped, 1, max_length, length_pattern[9], {}, T::live, 0},
    {"comp.length.11", "Length 11", "Length", "16ths", S::stepped, 1, max_length, length_pattern[10], {}, T::live, 0},
    {"comp.length.12", "Length 12", "Length", "16ths", S::stepped, 1, max_length, length_pattern[11], {}, T::live, 0},
    {"comp.length.13", "Length 13", "Length", "16ths", S::stepped, 1, max_length, length_pattern[12], {}, T::live, 0},
    {"comp.length.14", "Length 14", "Length", "16ths", S::stepped, 1, max_length, length_pattern[13], {}, T::live, 0},
    {"comp.length.15", "Length 15", "Length", "16ths", S::stepped, 1, max_length, length_pattern[14], {}, T::live, 0},
    {"comp.length.16", "Length 16", "Length", "16ths", S::stepped, 1, max_length, length_pattern[15], {}, T::live, 0},
    {"comp.top_length", "Top Length", "Top", "steps", S::stepped, 1, steps, steps, {}, T::live, 0},
    {"comp.push_chance", "Push Chance", "Push", "%", S::linear, 0, 1, 0.5, {}, T::live, 0},
    {"comp.push_amount", "Push By", "Push", "", S::choice, 0, last(pushes.size()), push_eighth, pushes, T::live, 0},
    {"comp.groove", "Groove", "Feel", "", S::choice, 0, last(grooves.size()), groove_58, grooves, T::live, 0},
    {"comp.humanise", "Humanise", "Feel", "%", S::linear, 0, 1, 0.2, {}, T::live, 0},
    {"comp.seed", "Seed", "Feel", "", S::stepped, 1, 9999, 7, {}, T::live, 0},
    {"comp.velocity", "Velocity", "Feel", "", S::stepped, 1, 127, 100, {}, T::live, 0},
    {"comp.ghost_velocity", "Ghost Velocity", "Feel", "", S::stepped, 1, 127, 60, {}, T::live, 0},
    {"comp.octave", "Octave", "Feel", "oct", S::stepped, -2, 2, 0, {}, T::live, 0},
    // Motion: how often a bar varies (a dropped hit, a ghost 16th after a
    // hit); a turnaround in each phrase's last bar; the top voice moved to
    // another inversion for each phrase's second half; a velocity swell
    // across the phrase.
    {"comp.variation", "Variation", "Motion", "%", S::linear, 0, 1, 0, {}, T::live, 0},
    {"comp.phrase", "Phrase", "Motion", "", S::choice, 0, last(phrases.size()), phrase_8, phrases, T::live, 0},
    {"comp.turnaround", "Turnaround", "Motion", "", S::choice, 0, last(switches.size()), 0, switches, T::live, 0},
    {"comp.drift", "Top Drift", "Motion", "", S::choice, 0, last(switches.size()), 0, switches, T::live, 0},
    {"comp.swell", "Swell", "Motion", "%", S::linear, 0, 1, 0, {}, T::live, 0},
    // Density: below the middle the chord's lanes thin towards the 8ths,
    // above it ghost 16ths fill after hits; the same in every bar.
    {"comp.density", "Density", "Motion", "%", S::linear, 0, 1, 0.5, {}, T::live, 0},
    // Lift: for each phrase's second half the top voice is doubled an octave
    // up, the brighter second half melodic house writes.
    {"comp.lift", "Lift", "Motion", "", S::choice, 0, last(switches.size()), 0, switches, T::live, 0},
    // Articulation: every hit's length scaled, from staccato to as written.
    {"comp.articulation", "Articulation", "Feel", "%", S::linear, 0.1, 1, 1, {}, T::live, 0},
}};
} // namespace comp

using native_dsp::parameter_index;

} // namespace oscillate::native_dsp::note_effects::tables
// clang-format on
