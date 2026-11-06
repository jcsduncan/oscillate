#pragma once

#include "oscillate/application/midi_recording.hpp"
#include "oscillate/application/session/session_service.hpp"
#include "oscillate/foundation/midi.hpp"

#include <algorithm>

namespace oscillate::application {

// The velocity new notes take until the lane sets another.
inline constexpr std::uint8_t default_new_note_velocity = 100;

class NoteService : public SessionService {
public:
  explicit NoteService(SessionState& state) noexcept : SessionService(state) {}

  // Copies of notes moved by ticks and semitones (a Ctrl-drag), as one undo
  // step; the originals stay. The same bounds as move_notes, and a copy may
  // not land on an existing note. Returns the copies' ids.
  [[nodiscard]] foundation::Result<std::vector<project::NoteId>>
  copy_notes_by(project::ClipId clip, std::span<const project::NoteId> notes,
                std::int64_t delta_ticks, int delta_keys);
  // Rhythm applied to notes (ADR-0084): what Comp with `parameters` (sparse,
  // as saved) plays over `held` (clip-local notes played as held input,
  // normally the clip's chords) from `start` to `end` (clip-local ticks),
  // played from the clip's place in the song: clip-local, sorted, ids
  // numbered from 1 (placeholders), cut at `end`. The same kernel, clock
  // and lookahead as playback, so Apply plays what a Comp on the track
  // would. Nothing changes (previews). `end` may pass the pattern's end, for
  // a caller lengthening the clip to hold a whole phrase.
  [[nodiscard]] foundation::Result<std::vector<project::MidiNote>>
  comp_rhythm_notes(project::ClipId clip,
                    std::span<const project::NativeInstrumentParameter> parameters,
                    std::span<const project::MidiNote> held, timeline::TickPosition start,
                    timeline::TickPosition end) const;
  // One undo step: the notes of `clip` starting in [start, end) are replaced
  // by comp_rhythm_notes; notes starting outside stay, and notes held across
  // `start` are cut there.
  [[nodiscard]] foundation::Result<void>
  apply_comp_rhythm(project::ClipId clip,
                    std::span<const project::NativeInstrumentParameter> parameters,
                    std::span<const project::MidiNote> held, timeline::TickPosition start,
                    timeline::TickPosition end);
  // MIDI recording (ADR-0086): a take's
  // finished `notes` (song ticks) onto `track`. Each goes into the MIDI clip
  // whose span holds its start (wrapped into a looping clip's pattern, cut
  // at the pattern's end), or else into the take's own clip, `take_clip`:
  // made at the first such note's bar and grown bar by bar, never into the
  // next clip. A note on an existing note's start and key is dropped. Calls
  // with the same `gesture` coalesce into one undo step. Returns the take's
  // clip, if it has one.
  [[nodiscard]] foundation::Result<std::optional<project::ClipId>>
  record_notes(project::TrackId track, std::span<const RecordedNote> notes,
               std::optional<project::ClipId> take_clip, std::uint64_t gesture);
  [[nodiscard]] foundation::Result<void> add_note(project::ClipId clip,
                                                  timeline::TickPosition start, std::uint8_t key);
  [[nodiscard]] foundation::Result<void> add_note_with_duration(project::ClipId clip,
                                                                timeline::TickPosition start,
                                                                timeline::TickPosition duration,
                                                                std::uint8_t key);
  [[nodiscard]] foundation::Result<void> remove_note(project::ClipId clip, project::NoteId note);
  // Velocity lane: set several notes' velocities (1..127) as one undo step.
  // Timing and identity are untouched; unknown notes reject the edit.
  [[nodiscard]] foundation::Result<void>
  set_note_velocities(project::ClipId clip,
                      std::span<const std::pair<project::NoteId, std::uint8_t>> velocities);
  // The piano roll's tools (midi_note_tools.hpp):
  // replace existing notes by id (new timing, key or velocity) and add new
  // notes, as one undo step. Added notes get fresh ids, returned in order.
  // Unknown ids, keys beyond 0..127, velocities beyond 1..127, empty notes
  // or two notes on one start and key reject the edit without change.
  [[nodiscard]] foundation::Result<std::vector<project::NoteId>>
  apply_note_edit(project::ClipId clip, std::span<const project::MidiNote> changed,
                  std::span<const project::MidiNote> added = {});
  // New notes take the velocity last set in the lane (100 to start).
  void set_new_note_velocity(std::uint8_t velocity) noexcept {
    new_note_velocity_ = static_cast<std::uint8_t>(
        std::clamp<int>(velocity, foundation::min_note_velocity, foundation::max_note_velocity));
  }
  [[nodiscard]] std::uint8_t new_note_velocity() const noexcept { return new_note_velocity_; }
  [[nodiscard]] foundation::Result<void> move_note(project::ClipId clip, project::NoteId note,
                                                   timeline::TickPosition start, std::uint8_t key);
  [[nodiscard]] foundation::Result<void> resize_note(project::ClipId clip, project::NoteId note,
                                                     timeline::TickPosition end);
  // Several notes at once, each as one undo step. Removing fails without
  // change if any id is unknown. Moving shifts every note by the same ticks
  // and semitones; each must stay inside the pattern (as move_note), and
  // none may land on the start and key of a note that is not moving.
  [[nodiscard]] foundation::Result<void> remove_notes(project::ClipId clip,
                                                      std::span<const project::NoteId> notes);
  [[nodiscard]] foundation::Result<void> move_notes(project::ClipId clip,
                                                    std::span<const project::NoteId> notes,
                                                    std::int64_t delta_ticks, int delta_keys);

private:
  std::uint8_t new_note_velocity_ = default_new_note_velocity;
};

} // namespace oscillate::application
