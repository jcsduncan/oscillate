#pragma once

#include "oscillate/clay_dsp/take.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Pure functions over a take's notes and their edits. Each returns the full
// edit list to use next (one entry per edited note, duplicates after),
// leaving notes it was not asked to touch as they were. A change that would
// leave an edit outside validate()'s bounds (a shift past max_edit_cents, a
// stretch past max_time_scale) is not made, so the result always renders.
namespace oscillate::clay_dsp {

inline constexpr int pitch_classes = 12;

// A key: a root pitch class (0 = C) and the pitch classes in it.
struct Scale {
  int root = 0;
  std::array<bool, pitch_classes> classes{};

  [[nodiscard]] static Scale chromatic();
  [[nodiscard]] static Scale major(int root);
  [[nodiscard]] static Scale minor(int root);
  [[nodiscard]] bool contains(int pitch_class) const noexcept;
  // The degrees in order from the root, as semitones above it.
  [[nodiscard]] std::vector<int> degrees() const;
  bool operator==(const Scale&) const = default;
};

// The edit a macro starts from for a note: its existing edit, or none.
[[nodiscard]] NoteEdit edit_of(std::span<const NoteEdit> edits, std::uint32_t note);
// Replaces (or adds) the non-duplicate edit of edit.note.
void put_edit(std::vector<NoteEdit>& edits, const NoteEdit& edit);
// Where a note sounds after its edit, in cents.
[[nodiscard]] double edited_centre(const Note& note, const NoteEdit& edit) noexcept;
// The nearest pitch in the scale to `cents`, in cents.
[[nodiscard]] double nearest_in_scale(double cents, const Scale& scale) noexcept;

// An empty selection means every note.
using Selection = std::span<const std::uint32_t>;

// Moves each note's centre `centre_amount` (0..1) of the way to the nearest
// scale pitch and cuts its drift by `drift_amount` (0..1).
[[nodiscard]] std::vector<NoteEdit> correct_pitch(const Take& take, std::span<const NoteEdit> edits,
                                                  double centre_amount, double drift_amount,
                                                  const Scale& scale, Selection selection = {});

// A timing grid: steps of `step_frames` from `origin`, each step pushed by
// offsets[step % size] steps (a groove; empty means straight). A grid with a
// non-positive or non-finite step or a non-finite offset moves nothing.
struct TimeGrid {
  std::int64_t origin = 0;
  double step_frames = 0.0;
  std::vector<double> offsets;
};
// Moves each note's start `amount` (0..1) of the way to the nearest grid
// step, keeping its length.
[[nodiscard]] std::vector<NoteEdit> quantize_time(const Take& take, std::span<const NoteEdit> edits,
                                                  const TimeGrid& grid, double amount,
                                                  Selection selection = {});

// Moves each note's level `amount` (0..1) of the way to the selection's
// median level.
[[nodiscard]] std::vector<NoteEdit> level_amplitude(const Take& take,
                                                    std::span<const NoteEdit> edits, double amount,
                                                    Selection selection = {});

struct MidiNoteOut {
  std::int64_t start = 0;
  std::int64_t end = 0;
  int key = 0;
  int velocity = 0;
};
struct BendPoint {
  std::int64_t frame = 0;
  double semitones = 0.0; // relative to the sounding key
  // The voice the bend belongs to: an index into MidiOut::notes, so
  // overlapping voices can route their bends separately.
  std::size_t note = 0;
};
struct MidiOut {
  std::vector<MidiNoteOut> notes;
  std::vector<BendPoint> bends;
  double bend_range = 0.0; // semitones the bends need
};
// Notes as edited, with velocities from their levels and pitch bends that
// follow their drift and vibrato every `bend_step_seconds`. A voice whose
// placement validate() would refuse is left out.
inline constexpr double bend_step_seconds = 0.01;
[[nodiscard]] MidiOut to_midi(const Take& take, std::span<const NoteEdit> edits);

// Adds a duplicate of each note `degrees` scale steps away (negative for
// below), placed on the scale.
[[nodiscard]] std::vector<NoteEdit> harmonise(const Take& take, std::span<const NoteEdit> edits,
                                              int degrees, const Scale& scale,
                                              Selection selection = {});

// Moves notes from one key into another, degree by degree. With
// `follow_quality` a minor third becomes a major third (and so on) when the
// keys' modes differ; without, the notes keep their intervals.
[[nodiscard]] std::vector<NoteEdit> fit_to_key(const Take& take, std::span<const NoteEdit> edits,
                                               const Scale& from, const Scale& to,
                                               bool follow_quality, Selection selection = {});

// Moves each note to the target note it overlaps most: its key and, by
// `time_amount` (0..1), its start and end.
struct TargetNote {
  std::int64_t start = 0;
  std::int64_t end = 0;
  int key = 0;
};
[[nodiscard]] std::vector<NoteEdit> follow_notes(const Take& take, std::span<const NoteEdit> edits,
                                                 std::span<const TargetNote> targets,
                                                 double time_amount, Selection selection = {});

// Small seeded changes of pitch, timing and level, scaled by `amount` (0..1).
inline constexpr double variation_cents = 15.0;
inline constexpr double variation_seconds = 0.012;
inline constexpr double variation_db = 2.0;
[[nodiscard]] std::vector<NoteEdit> variation(const Take& take, std::span<const NoteEdit> edits,
                                              std::uint64_t seed, double amount,
                                              Selection selection = {});

} // namespace oscillate::clay_dsp
