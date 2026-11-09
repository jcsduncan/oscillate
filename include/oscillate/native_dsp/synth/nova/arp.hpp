#pragma once

#include "oscillate/foundation/id.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/project/model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

// Nova's arpeggiator and clip sequencer (spec 08 §8, contract 08a §2.11 and
// §3.6, ADR-0078): a note generator between the runtime's notes and Nova's
// voices. Its output depends only on the played notes, the song position
// and the seed, so a seek or any block split gives the same notes (export
// equals live).
namespace oscillate::native_dsp::synth::nova {

inline constexpr std::size_t sequencer_clips = 4;
inline constexpr std::size_t max_generated_notes = 256; // Per call.

// ---- The pattern engine (ADR-0078) ----------------------------------------

// Steps a lane can hold; each lane loops at its own length.
inline constexpr std::size_t arp_max_steps = 32;
inline constexpr std::uint32_t arp_default_length = 16;

// In the order of nova.arp.mode's choices, grouped as the panel shows them:
// Order, Pedal, Chance, Harmony, Drawn, Rhythm.
enum class ArpMode : std::uint8_t {
  up,
  down,
  up_down,
  down_up,
  converge,
  diverge,
  played,
  thumb,
  pinky,
  random,
  shuffle,
  walk,
  chord,
  strum,
  shape,
  euclid,
};
inline constexpr std::size_t arp_mode_count = 16;
inline constexpr std::array<std::string_view, arp_mode_count> arp_mode_names{
    "Up",    "Down",   "Up/Down", "Down/Up",     "Converge", "Diverge", "Played", "Thumb",
    "Pinky", "Random", "Shuffle", "Random walk", "Chord",    "Strum",   "Shape",  "Euclidean"};
enum class ArpVelocity : std::uint8_t { played, pattern };

// nova.arp.rate's choices, in quarter-note beats.
inline constexpr std::array<double, 15> arp_rate_beats{
    4.0,       2.0,   4.0 / 3.0, 1.5,       1.0,   2.0 / 3.0,  0.75,  0.5,
    1.0 / 3.0, 0.375, 0.25,      1.0 / 6.0, 0.125, 1.0 / 12.0, 0.0625};
inline constexpr std::size_t arp_default_rate = 10; // 1/16
inline constexpr std::uint32_t arp_max_octaves = 4;
inline constexpr double arp_max_swing = 0.75;

// The lanes, in the order of their parameters.
enum class ArpLane : std::uint8_t {
  note,
  octave,
  velocity,
  gate,
  tie,
  ratchet,
  chance,
  condition,
  accent,
  mod,
  chord,
};
inline constexpr std::size_t arp_lane_count = 11;
// What a lane's values are: a level 0..1 (drawn as a bar), a whole number,
// an on/off toggle, or one of a list of choices.
enum class ArpLaneKind : std::uint8_t { level, integer, toggle, choice };
struct ArpLaneSpec {
  std::string_view id;
  std::string_view name;
  ArpLaneKind kind;
  double minimum;
  double maximum;
  double fallback;
};

// Conditions (Elektron-style trigs): X:Y plays on the X-th of every Y passes
// of the condition lane; Fill while Fill is held or on a variation bar;
// First on the first pass after the pattern starts.
enum class ArpCondition : std::uint8_t {
  always,
  one_of_two,
  two_of_two,
  one_of_three,
  two_of_three,
  three_of_three,
  one_of_four,
  two_of_four,
  three_of_four,
  four_of_four,
  fill,
  not_fill,
  first,
  not_first,
};
inline constexpr std::size_t arp_condition_count = 14;
inline constexpr std::array<std::string_view, arp_condition_count> arp_condition_names{
    "Always", "1:2", "2:2", "1:3",  "2:3",      "3:3",   "1:4",
    "2:4",    "3:4", "4:4", "Fill", "Not fill", "First", "Not first"};

// The chord lane stacks notes over a single-note mode's note: diatonic to
// the arp scale when one is set, otherwise from the held chord.
enum class ArpChord : std::uint8_t { off, octave, fifth, triad, seventh, sus };
inline constexpr std::size_t arp_chord_count = 6;
inline constexpr std::array<std::string_view, arp_chord_count> arp_chord_names{
    "Off", "+Oct", "5th", "Triad", "7th", "Sus"};

inline constexpr int arp_note_range = 12;  // Note lane: −12..+12.
inline constexpr int arp_octave_range = 2; // Octave lane: −2..+2.
inline constexpr int arp_max_ratchet = 4;

inline constexpr std::array<ArpLaneSpec, arp_lane_count> arp_lane_specs{{
    {"note", "Note", ArpLaneKind::integer, -arp_note_range, arp_note_range, 0.0},
    {"octave", "Octave", ArpLaneKind::integer, -arp_octave_range, arp_octave_range, 0.0},
    {"velocity", "Velocity", ArpLaneKind::level, 0.0, 1.0, 1.0},
    {"gate", "Gate", ArpLaneKind::level, 0.0, 1.0, 1.0},
    {"tie", "Tie", ArpLaneKind::toggle, 0.0, 1.0, 0.0},
    {"ratchet", "Ratchet", ArpLaneKind::integer, 1.0, arp_max_ratchet, 1.0},
    {"chance", "Chance", ArpLaneKind::level, 0.0, 1.0, 1.0},
    {"condition", "Condition", ArpLaneKind::choice, 0.0, arp_condition_count - 1, 0.0},
    {"accent", "Accent", ArpLaneKind::toggle, 0.0, 1.0, 0.0},
    {"mod", "Mod", ArpLaneKind::level, 0.0, 1.0, 0.0},
    {"chord", "Chord", ArpLaneKind::choice, 0.0, arp_chord_count - 1, 0.0},
}};
[[nodiscard]] constexpr const ArpLaneSpec& arp_lane_spec(ArpLane lane) noexcept {
  return arp_lane_specs[static_cast<std::size_t>(lane)];
}

// How the Note lane moves the note: by places in the held notes (wrapping)
// or by degrees of the arp scale (the held chord's tones when the scale is
// Chord), crossing octaves.
enum class ArpNoteMode : std::uint8_t { held, scale };

// Scales as pitch-class masks over the root (bit n: n semitones up). Chord
// is no scale: degrees are the held notes and there are no passing notes.
inline constexpr std::size_t semitones = 12;
struct ArpScale {
  std::string_view name;
  std::uint16_t mask;
};
[[nodiscard]] constexpr std::uint16_t pitch_classes(std::initializer_list<int> classes) noexcept {
  std::uint16_t mask = 0;
  for (const int c : classes) {
    mask = static_cast<std::uint16_t>(mask | (1U << static_cast<unsigned>(c)));
  }
  return mask;
}
inline constexpr std::size_t arp_scale_count = 14;
inline constexpr std::array<ArpScale, arp_scale_count> arp_scales{{
    {"Chord", 0},
    {"Major", pitch_classes({0, 2, 4, 5, 7, 9, 11})},
    {"Minor", pitch_classes({0, 2, 3, 5, 7, 8, 10})},
    {"Dorian", pitch_classes({0, 2, 3, 5, 7, 9, 10})},
    {"Phrygian", pitch_classes({0, 1, 3, 5, 7, 8, 10})},
    {"Lydian", pitch_classes({0, 2, 4, 6, 7, 9, 11})},
    {"Mixolydian", pitch_classes({0, 2, 4, 5, 7, 9, 10})},
    {"Locrian", pitch_classes({0, 1, 3, 5, 6, 8, 10})},
    {"Harmonic minor", pitch_classes({0, 2, 3, 5, 7, 8, 11})},
    {"Melodic minor", pitch_classes({0, 2, 3, 5, 7, 9, 11})},
    {"Major pentatonic", pitch_classes({0, 2, 4, 7, 9})},
    {"Minor pentatonic", pitch_classes({0, 3, 5, 7, 10})},
    {"Blues", pitch_classes({0, 3, 5, 6, 7, 10})},
    {"Chromatic", pitch_classes({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11})},
}};
inline constexpr std::array<std::string_view, arp_scale_count> arp_scale_names{
    arp_scales[0].name,  arp_scales[1].name, arp_scales[2].name,  arp_scales[3].name,
    arp_scales[4].name,  arp_scales[5].name, arp_scales[6].name,  arp_scales[7].name,
    arp_scales[8].name,  arp_scales[9].name, arp_scales[10].name, arp_scales[11].name,
    arp_scales[12].name, arp_scales[13].name};
inline constexpr std::array<std::string_view, semitones> arp_root_names{
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// Variation every N bars: the last bar of every N plays as a fill, with the
// Note and Octave lanes read from a seeded offset (0 is off).
inline constexpr std::array<std::uint32_t, 5> arp_variation_bars{0, 2, 4, 8, 16};
inline constexpr std::array<std::string_view, 5> arp_variation_names{"Off", "2 bars", "4 bars",
                                                                     "8 bars", "16 bars"};

// The arp's groove: a copy of a groove-pool template (ADR-0059) held in
// parameters, so it saves with the patch and needs no document. Steps on a
// grid of `steps_per_quarter`; timing is a fraction of a groove step (the
// pool's convention), velocity a factor.
inline constexpr std::size_t arp_groove_steps = 32;
inline constexpr std::array<std::uint32_t, 6> arp_groove_grids{1, 2, 3, 4, 6, 8};
inline constexpr std::array<std::string_view, 6> arp_groove_grid_names{"1/4",  "1/8",    "1/8 T",
                                                                       "1/16", "1/16 T", "1/32"};
inline constexpr std::size_t arp_default_groove_grid = 3; // 1/16
inline constexpr double arp_max_groove_timing = 0.5;      // persistence::max_groove_timing
inline constexpr double arp_max_groove_velocity = 2.0;    // persistence::max_groove_velocity

// A step moves early or late by swing and groove together, at most this
// share of a step each way, so steps keep their order (early + late < 1).
inline constexpr double arp_early_limit = 0.24;
inline constexpr double arp_late_limit = arp_max_swing;
// An accented note's velocity rises by this (of full scale); a ratchet's
// later hits play at this share of the first's.
inline constexpr double arp_accent_velocity = 0.2;
inline constexpr double arp_ratchet_velocity = 0.8;
// Random walk restarts from the first note every phrase of this many steps,
// so any step's note is found from the phrase start.
inline constexpr std::int64_t arp_walk_phrase = 64;
// The longest seed (nova.arp.seed).
inline constexpr double arp_max_seed = 9999.0;
inline constexpr double arp_default_strum = 0.35; // Of a step.
inline constexpr std::uint32_t arp_default_euclid_hits = 5;

// Document labels (08a §3.5) naming the pattern and groove last loaded;
// the sound is in the parameters, so they only name it.
inline constexpr std::string_view arp_pattern_label = "arp.pattern";
inline constexpr std::string_view arp_groove_label = "arp.groove";

template <std::size_t N>
[[nodiscard]] constexpr std::array<double, N> filled(double value) noexcept {
  std::array<double, N> values{};
  for (auto& v : values) {
    v = value;
  }
  return values;
}

struct ArpLaneValues {
  std::uint32_t length = arp_default_length; // 1..arp_max_steps
  std::array<double, arp_max_steps> values{};
};
[[nodiscard]] constexpr std::array<ArpLaneValues, arp_lane_count> default_arp_lanes() noexcept {
  std::array<ArpLaneValues, arp_lane_count> lanes{};
  for (std::size_t l = 0; l < arp_lane_count; ++l) {
    lanes[l].values = filled<arp_max_steps>(arp_lane_specs[l].fallback);
  }
  return lanes;
}

struct ArpGroove {
  std::uint32_t steps = 0; // 0: no groove.
  std::size_t grid = arp_default_groove_grid;
  double amount = 1.0;
  std::array<double, arp_groove_steps> timing{};
  std::array<double, arp_groove_steps> velocity = filled<arp_groove_steps>(1.0);
};

struct ArpSettings {
  bool on = false;
  ArpMode mode = ArpMode::up;
  std::size_t rate = arp_default_rate; // Index into arp_rate_beats.
  double gate = 0.6;
  double swing = 0.0; // 0..0.75: every second step is late by swing × step.
  std::uint32_t octaves = 1;
  ArpVelocity velocity = ArpVelocity::played;
  bool retrigger = true; // The pattern restarts when a chord starts.
  bool latch = false;
  ArpNoteMode note_mode = ArpNoteMode::held;
  std::size_t scale = 0; // Index into arp_scales.
  std::uint32_t root = 0;
  bool passing = false; // Scale tones between the held notes join them.
  std::uint32_t seed = 0;
  bool fill = false;
  std::uint32_t variation = 0; // Bars; 0 off.
  double strum = arp_default_strum;
  std::uint32_t euclid_hits = arp_default_euclid_hits;
  std::uint32_t euclid_steps = arp_default_length;
  std::uint32_t euclid_rotate = 0;
  ArpGroove groove{};
  std::array<ArpLaneValues, arp_lane_count> lanes = default_arp_lanes();

  [[nodiscard]] ArpLaneValues& lane(ArpLane l) noexcept {
    return lanes[static_cast<std::size_t>(l)];
  }
  [[nodiscard]] const ArpLaneValues& lane(ArpLane l) const noexcept {
    return lanes[static_cast<std::size_t>(l)];
  }
};

// The pattern repeats after the least common multiple of its lane lengths
// (and the Euclidean cycle in that mode); what the panel shows as PERIOD.
[[nodiscard]] std::uint64_t arp_period(const ArpSettings& settings) noexcept;
// Whether Euclidean step k (counted from the pattern start) is a hit.
[[nodiscard]] bool euclid_hit(std::int64_t k, std::uint32_t hits, std::uint32_t steps,
                              std::uint32_t rotate) noexcept;

enum class SequencerTrigger : std::uint8_t { transport, note };
struct SequencerSettings {
  bool on = false;
  std::size_t clip = 0; // 0..3, the clip that plays with the transport.
  SequencerTrigger trigger = SequencerTrigger::transport;
  std::uint8_t root = 24;
  bool follow = true; // Transpose by the held key relative to root.
};

// The document's clip.1..clip.4 in a fixed, callback-readable form; built on
// control when the document compiles.
struct SequencerClips {
  struct Note {
    double start_beats = 0.0;
    double length_beats = 0.25;
    std::uint8_t key = 60;
    std::uint8_t velocity = 100;
  };
  std::array<double, sequencer_clips> length_beats{4.0, 4.0, 4.0, 4.0};
  std::array<std::vector<Note>, sequencer_clips> notes; // Sorted by start.
};
[[nodiscard]] SequencerClips compile_sequencer_clips(const project::NativeSynthDocument& document);

// A note going in (played) or out (generated). Generated notes carry ids
// the generator mints, unique while they sound, so note-offs pair exactly.
// An arp note also carries its step's Mod lane value and accent, which its
// voice publishes as the arp.mod and arp.accent sources.
struct GeneratedNote {
  std::uint32_t offset = 0; // Frame within the block.
  bool on = true;
  NoteEvent note{};
  float mod = 0.0F;
  bool accent = false;
};

// Callback safe after prepare(); allocation-free.
class NoteGenerator {
public:
  // Defined in arp.cpp, where State is complete.
  NoteGenerator();
  ~NoteGenerator();
  NoteGenerator(const NoteGenerator&) = delete;
  NoteGenerator& operator=(const NoteGenerator&) = delete;
  void prepare(double sample_rate);
  // Seek or loop: forget held notes and sounding generated notes (the
  // kernel has already silenced its voices).
  void reset() noexcept;
  void set(const ArpSettings& arp, const SequencerSettings& sequencer) noexcept;
  // The adopted document's clips; valid until the next call.
  void adopt(const SequencerClips& clips) noexcept;
  [[nodiscard]] bool active() const noexcept; // Arp or sequencer on.
  // One block: `played` sorted by offset, the transport at the block start
  // and its length. Writes the notes the voices should play, sorted by
  // offset (offs before ons at one frame), and returns how many. When
  // neither the arp nor the sequencer is on, played notes pass through.
  [[nodiscard]] std::size_t process(std::span<const GeneratedNote> played,
                                    const Transport& transport, std::uint32_t frames,
                                    std::span<GeneratedNote, max_generated_notes> out) noexcept;
  // 08a §6 offsets 77..79 and the arp block.
  [[nodiscard]] int playing_clip() const noexcept;
  [[nodiscard]] double clip_playhead_beats() const noexcept;
  // The pattern step last played, counted from the pattern start (each
  // lane shows step % its length), or −1.
  [[nodiscard]] int arp_step() const noexcept;
  // Whether that step was on a variation bar.
  [[nodiscard]] bool arp_variation() const noexcept;

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace oscillate::native_dsp::synth::nova
