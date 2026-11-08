#pragma once

#include "oscillate/application/session/session_service.hpp"

namespace oscillate::application {

class MidiClipService : public SessionService {
public:
  explicit MidiClipService(SessionState& state) noexcept : SessionService(state) {}

  // The track's MIDI clips overlapping [start, end) become one clip spanning
  // them all, loops written out, as one undo step. Returns the new clip.
  [[nodiscard]] foundation::Result<project::ClipId>
  consolidate_midi_clips(project::TrackId track, timeline::TickPosition start,
                         timeline::TickPosition end);
  // The chosen MIDI clips, all on one track, become one clip from the
  // first's start to the last's end, as consolidate does; gaps stay silent.
  [[nodiscard]] foundation::Result<project::ClipId>
  merge_midi_clips(std::span<const project::ClipId> clips);
  [[nodiscard]] foundation::Result<void> add_midi_clip(project::TrackId track,
                                                       timeline::TickPosition start);
  // One undo step: a MIDI clip from `start` lasting `length`, with `notes`
  // (clip-local ticks; their ids are replaced by fresh ones), as Chop to MIDI
  // writes it (ADR-0063). Returns the new clip's id.
  [[nodiscard]] foundation::Result<project::ClipId>
  add_midi_clip_with_notes(project::TrackId track, timeline::TickPosition start,
                           timeline::TickPosition length, std::vector<project::MidiNote> notes);
  // One undo step: the MIDI clip's notes become `notes` (clip-local ticks,
  // each inside [0, length); their ids are replaced by fresh ones) and its
  // pattern `length`, as a generated pattern writes it (Knock, ADR-0068).
  // A clip that does not loop ends with the pattern; a looping clip keeps
  // its placement and repeats the new pattern. Pedals and automation stay.
  // Steps with the same gesture coalesce into one undo step (a mood-pad
  // drag rewriting a clip's chords).
  [[nodiscard]] foundation::Result<void>
  replace_midi_clip_notes(project::ClipId clip, timeline::TickPosition length,
                          std::vector<project::MidiNote> notes,
                          std::optional<std::uint64_t> gesture = {});
  // Quantise starts to the nearest grid boundary as one atomic undo step.
  // Durations and velocities are preserved; conflicting destinations reject
  // the operation without moving any notes.
  [[nodiscard]] foundation::Result<void> quantize_midi_clip(project::ClipId clip,
                                                            int steps_per_bar);
  [[nodiscard]] foundation::Result<void> move_midi_clip(project::ClipId clip,
                                                        timeline::TickPosition new_start);
  // Onto another instrument track at `new_start`, keeping its notes,
  // settings and automation: one undo step.
  [[nodiscard]] foundation::Result<void> move_midi_clip_to_track(project::ClipId clip,
                                                                 project::TrackId track,
                                                                 timeline::TickPosition new_start);
  // Moves the clip's end (any tick after its start), one undo step. A looping clip
  // repeats its pattern to the new end. Otherwise the pattern follows the
  // clip's length: lengthening makes room for notes, and shortening keeps
  // the notes after the new end, silent, until the clip is lengthened again.
  [[nodiscard]] foundation::Result<void> resize_midi_clip(project::ClipId clip,
                                                          timeline::TickPosition new_end);
  // Switches the clip's loop as one undo step. The pattern becomes the
  // clip's current length either way, so the whole clip repeats, or its
  // repeats stop with the first pass's notes left where they are.
  [[nodiscard]] foundation::Result<void> set_midi_clip_loop(project::ClipId clip, bool loop);
  // Copy one MIDI clip immediately after itself; notes and clip automation get
  // fresh IDs while keeping their clip-local timing and saved settings.
  [[nodiscard]] foundation::Result<project::ClipId> duplicate_midi_clip_after(project::ClipId clip);
  // Split a musical, non-looping MIDI clip at a project tick, one undo step.
  // Notes crossing the cut end there; pedals held across it go down again at
  // the right half's start; clip automation is subdivided exactly.
  [[nodiscard]] foundation::Result<project::ClipId>
  split_midi_clip_at_tick(project::ClipId clip, timeline::TickPosition boundary);
  [[nodiscard]] foundation::Result<void> remove_midi_clip(project::ClipId clip);
  [[nodiscard]] foundation::Result<void> toggle_midi_clip_timebase(project::ClipId clip);

private:
  [[nodiscard]] foundation::Result<project::ClipId>
  merge_chosen_midi_clips(project::TrackId track,
                          const std::vector<const project::MidiClip*>& chosen);
};

} // namespace oscillate::application
