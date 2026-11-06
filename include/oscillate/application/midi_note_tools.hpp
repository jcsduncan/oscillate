#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/persistence/groove_file.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/timeline/timing.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The piano roll's musical tools (ADR-0059): scale snapping, chord stamps,
// strum, timing humanise, quantise with strength and swing, and grooves.
// Pure functions over a clip's notes: each returns the notes it changes
// (same ids, new timing or velocity) or new notes to add, and the session
// applies the result as one undo step (ProjectSession::apply_note_edit).
// Positions are clip-local ticks; `clip_start` is the clip's first tick on
// the song's timeline, so swing and grooves line up with the song's grid.
namespace oscillate::application {

// --- Grid ----------------------------------------------------------------

// The piano roll's grids, as steps per 4/4 bar (1/4 .. 1/128 notes). The
// one list the session, the Quick adapters and the roll's GRID buttons use.
inline constexpr std::array<int, 6> midi_grid_steps_per_bar{4, 8, 16, 32, 64, 128};
inline constexpr std::int64_t midi_grid_bar_ticks = timeline::ticks_per_quarter * 4;

[[nodiscard]] constexpr bool supported_midi_grid(int steps_per_bar) noexcept {
  return std::ranges::find(midi_grid_steps_per_bar, steps_per_bar) != midi_grid_steps_per_bar.end();
}
// One grid step in ticks; only meaningful for a supported grid.
[[nodiscard]] constexpr std::int64_t midi_grid_ticks(int steps_per_bar) noexcept {
  return midi_grid_bar_ticks / steps_per_bar;
}
static_assert(std::ranges::all_of(midi_grid_steps_per_bar,
                                  [](int steps) { return midi_grid_bar_ticks % steps == 0; }),
              "every MIDI grid step is a whole number of ticks");

// --- Scale ---------------------------------------------------------------

// Pitch classes (C = 0) of the key's major or natural minor scale.
[[nodiscard]] std::array<bool, 12> scale_pitch_classes(project::MusicalKey key) noexcept;
// The in-scale key nearest `key` (a tie goes up), kept within 0..127.
[[nodiscard]] std::uint8_t snap_to_scale(int key, project::MusicalKey scale) noexcept;
// Moves `key` (snapped into the scale first) by `degrees` scale steps, as
// Scale lock transposes a set of notes; kept within 0..127.
[[nodiscard]] std::uint8_t transpose_in_scale(int key, int degrees,
                                              project::MusicalKey scale) noexcept;
// Scale steps from `from` up to `to` (negative when lower), both snapped.
[[nodiscard]] int scale_degrees_between(int from, int to, project::MusicalKey scale) noexcept;

// --- Chords --------------------------------------------------------------

enum class ChordShape : std::uint8_t { triad, seventh, sus2, sus4, add9 };
// The keys of a chord built up from `root`, lowest first. With a key the
// chord is diatonic: the root snaps into the scale and every tone is a
// scale degree (so C major's D triad is D minor). Without one it is major
// (triad, major 7th, sus2, sus4, add9). An inversion of n lifts the n
// lowest tones an octave. Tones beyond 0..127 are left out.
[[nodiscard]] std::vector<std::uint8_t> chord_keys(int root, ChordShape shape, int inversion,
                                                   std::optional<project::MusicalKey> key);

// --- Strum, humanise, quantise --------------------------------------------

// Notes of the scope starting together form a chord; each chord's tones
// start `step_ticks` apart, lowest first (upward) or highest first, the
// first in place. Ends stay, so a strummed tone is shorter; a tone never
// starts at or after its end or the pattern's end.
[[nodiscard]] std::vector<project::MidiNote> strum_notes(std::span<const project::MidiNote> notes,
                                                         std::span<const project::NoteId> scope,
                                                         std::int64_t step_ticks, bool upward,
                                                         std::int64_t pattern_length);

// Timing humanise: each note moves by a centre-weighted random amount
// within +/-`max_ticks`, keeping its length, inside the pattern. A move that
// would land on another note's start and key is skipped. Reproducible from
// `seed`.
[[nodiscard]] std::vector<project::MidiNote>
humanize_timing(std::span<const project::MidiNote> notes, std::span<const project::NoteId> scope,
                std::int64_t max_ticks, std::uint64_t seed, std::int64_t pattern_length);

struct QuantizeSettings {
  std::int64_t grid_ticks = timeline::ticks_per_quarter / 4;
  double strength = 1.0; // 0..1: how far toward the grid line.
  double swing = 0.5;    // 0.5..0.75: where every second grid line sits.
  bool ends = false;     // Also quantise note ends (keeping at least one grid).
};
// Starts move toward the nearest (swung) grid line on the song's grid.
// Notes after the pattern's end stay. Fails, changing nothing, when two
// notes would land on one start and key.
[[nodiscard]] foundation::Result<std::vector<project::MidiNote>>
quantize_notes(std::span<const project::MidiNote> notes, std::span<const project::NoteId> scope,
               const QuantizeSettings& settings, std::int64_t clip_start,
               std::int64_t pattern_length);

// --- Grooves -------------------------------------------------------------

struct GrooveAmounts {
  double quantize = 1.0; // 0..1 toward the groove's grid.
  double timing = 1.0;   // 0..1 of the groove's offsets.
  double velocity = 1.0; // 0..1 of the groove's accents.
};
// Each note moves toward its nearest groove step (quantize), then by that
// step's offset (timing), and takes its accent (velocity). Notes after the
// pattern's end stay. Fails, changing nothing, when two notes would land on
// one start and key.
[[nodiscard]] foundation::Result<std::vector<project::MidiNote>>
apply_groove(std::span<const project::MidiNote> notes, std::span<const project::NoteId> scope,
             const persistence::GrooveTemplate& groove, const GrooveAmounts& amounts,
             std::int64_t clip_start, std::int64_t pattern_length);

// The built-in groove pool: straight swing on 8ths and 16ths (54% to 75%),
// MPC-style 16th swing with accents, and laid-back and pushed feels.
[[nodiscard]] std::vector<persistence::GrooveTemplate> builtin_grooves();

// A hit on the song's timeline (song ticks, for example an audio clip's
// onsets) and its strength, for measuring a groove from a record.
struct GrooveHit {
  std::int64_t tick = 0;
  double strength = 1.0;
};

// The first song tick of a MIDI clip (0 for an elapsed-time clip, whose
// notes are still counted in ticks from its start).
[[nodiscard]] std::int64_t midi_clip_start_tick(const project::MidiClip& clip) noexcept;

} // namespace oscillate::application
