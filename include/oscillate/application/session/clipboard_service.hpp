#pragma once

#include "oscillate/application/session/session_service.hpp"

namespace oscillate::application {

class ClipboardService : public SessionService {
public:
  explicit ClipboardService(SessionState& state) noexcept : SessionService(state) {}

  // Clipboard. Copies are deep (notes, pedals, automation, fades,
  // processing) and every paste allocates fresh identities; each paste,
  // cut or duplicate is one undo step.
  //
  // Clips keep their relative timing and their track offsets: pasting onto
  // track T puts the first copied track's clips on T and the others on the
  // tracks below it, in arrangement order. Audio clips need an audio track,
  // MIDI clips an instrument track.
  [[nodiscard]] foundation::Result<void> copy_clips(std::span<const project::ClipId> clips);
  [[nodiscard]] foundation::Result<void> cut_clips(std::span<const project::ClipId> clips);
  [[nodiscard]] foundation::Result<std::vector<project::ClipId>>
  paste_clips(project::TrackId track, timeline::TickPosition position);
  // Copies placed immediately after the selection's end, on the same tracks
  // (Ableton's Ctrl+D). The clipboard is left untouched.
  [[nodiscard]] foundation::Result<std::vector<project::ClipId>>
  duplicate_clips(std::span<const project::ClipId> clips);
  [[nodiscard]] bool clipboard_has_clips() const noexcept { return !clipboard_.clips.empty(); }
  // Moves the clips together by `delta_ticks` and `track_delta` rows,
  // keeping their spacing, as one undo step. Refused as a whole when any
  // clip would start before bar 1, leave the track list or land on the
  // wrong kind of track.
  // Removes the clips, as one undo step; the clipboard is left untouched.
  [[nodiscard]] foundation::Result<void> remove_clips(std::span<const project::ClipId> clips);
  [[nodiscard]] foundation::Result<void> move_clips(std::span<const project::ClipId> clips,
                                                    std::int64_t delta_ticks, int track_delta);

  // Notes within one MIDI clip, in clip-local ticks.
  [[nodiscard]] foundation::Result<void> copy_notes(project::ClipId clip,
                                                    std::span<const project::NoteId> notes);
  [[nodiscard]] foundation::Result<std::vector<project::NoteId>>
  paste_notes(project::ClipId clip, timeline::TickPosition position);
  [[nodiscard]] foundation::Result<std::vector<project::NoteId>>
  duplicate_notes(project::ClipId clip, std::span<const project::NoteId> notes);
  [[nodiscard]] bool clipboard_has_notes() const noexcept { return !clipboard_.notes.empty(); }

private:
  // Session-local, never saved or undone.
  struct Clipboard {
    struct ClipEntry {
      std::size_t track_offset; // Arrangement rows below the first copied track.
      std::variant<project::Clip, project::MidiClip> clip;
      timeline::TickPosition start;  // The copy's start, for relative placement.
      project::TrackId source_track; // Where its track-bound lanes pointed.
    };
    std::vector<ClipEntry> clips;
    timeline::TickPosition clips_origin{};
    std::vector<project::MidiNote> notes; // Relative to the earliest note.
  };
  Clipboard clipboard_;
};

} // namespace oscillate::application
