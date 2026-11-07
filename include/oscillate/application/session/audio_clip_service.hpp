#pragma once

#include "oscillate/application/clay_edit.hpp"
#include "oscillate/application/session/session_service.hpp"

namespace oscillate::application {

enum class AudioClipEdge : std::uint8_t { left, right };

// What Convert to sampler made: the new Crate track, its MIDI clip and how
// many chops that clip plays.
struct SamplerConversion {
  project::TrackId track;
  project::ClipId clip;
  std::size_t chops = 0;
};

class AudioClipService : public SessionService {
public:
  explicit AudioClipService(SessionState& state) noexcept : SessionService(state) {}

  [[nodiscard]] foundation::Result<void> set_clip_color(project::ClipId clip,
                                                        std::optional<std::uint32_t> color);
  // Convert to sampler (ADR-0063): the audio clip's source on Crate, chopped
  // at the transients inside the clip, on a new instrument track just below
  // the clip's, with a MIDI clip at the same place and in the same timebase
  // playing those chops where the audio plays them; one undo step. The
  // source is referenced, not copied, and the audio track stays as it is.
  // Decodes the source here: refused for a source longer than
  // project::max_native_synth_media_frames, linked from outside the project
  // or unavailable, and for a MIDI clip or a frozen track.
  [[nodiscard]] foundation::Result<SamplerConversion>
  convert_audio_clip_to_sampler(project::ClipId clip);
  // Places an audio clip at the requested musical location while retaining its
  // stored timebase and exact duration. Arrangement UI supplies its snap policy.
  [[nodiscard]] foundation::Result<void> move_audio_clip(project::ClipId clip,
                                                         timeline::TickPosition new_start);
  // Moves an audio clip onto another audio track at `new_start`, keeping its
  // identity, settings, automation and stretch: one undo step.
  [[nodiscard]] foundation::Result<void> move_audio_clip_to_track(project::ClipId clip,
                                                                  project::TrackId track,
                                                                  timeline::TickPosition new_start);
  [[nodiscard]] foundation::Result<project::ClipId>
  duplicate_audio_clip(project::ClipId clip, timeline::TickPosition new_start);
  // Places one copy at the exact following boundary in the clip's own timebase.
  // A neighbouring clip can reject the atomic AddClip operation.
  [[nodiscard]] foundation::Result<project::ClipId>
  duplicate_audio_clip_after(project::ClipId clip);
  // Split at an exact project-sample boundary. The two resulting clips keep
  // their source ranges, fades and clip-local automation, with one undo step.
  // A cut inside a nonlinear automation segment rejects until exact curve
  // subdivision is available; the saved clip is never approximated silently.
  // Split, slip and trim of a processed (warped, stretched or pitched) clip
  // re-window its warp map (application::rewindow_processed_clip): reviewed
  // markers stay with their audio and per-clip warp settings are kept. A
  // split gives each marker to its half; a trim or slip that would leave a
  // marker outside the clip rejects instead of dropping it.
  [[nodiscard]] foundation::Result<project::ClipId>
  split_audio_clip_at_sample(project::ClipId clip, timeline::SamplePosition boundary);
  // Slides the selected source window under a fixed timeline interval. For a
  // processed clip the audio moves with its markers by the timeline span the
  // delta covers at the clip start (application::processed_slip_offset).
  [[nodiscard]] foundation::Result<void> slip_audio_clip(project::ClipId clip,
                                                         std::int64_t source_frame_delta);
  // Switch the placement anchor while retaining both current project-sample
  // boundaries. Future tempo edits then follow the selected anchor.
  [[nodiscard]] foundation::Result<void> toggle_audio_clip_timebase(project::ClipId clip);
  [[nodiscard]] foundation::Result<void> remove_audio_clip(project::ClipId clip);
  // new_edge is an absolute project sample boundary. The UI applies snap
  // before calling. For musical clips it must round-trip exactly through the
  // shared tempo map. Rate-converted source boundaries must round-trip through
  // the source-rate reader's frame mapping; trimming cannot hide a stretch.
  [[nodiscard]] foundation::Result<void> trim_audio_clip(project::ClipId clip, AudioClipEdge edge,
                                                         timeline::SamplePosition new_edge);
  [[nodiscard]] foundation::Result<void> set_audio_clip_fades(project::ClipId clip,
                                                              project::ClipFades fades);
  // Persist clip-local pitch and reviewed marker intent in one undoable edit.
  // Renderers report unsupported until processed audio is ready.
  [[nodiscard]] foundation::Result<void>
  set_audio_clip_processing(project::ClipId clip, project::ClipProcessing processing);
  // Presentation only; empty shows the track's name. Control characters are
  // refused and names are at most project::max_track_name_bytes.
  [[nodiscard]] foundation::Result<void> set_audio_clip_name(project::ClipId clip,
                                                             std::string name);
  // Clay (ADR-0081): an audio clip's note edits as one undo step; nullopt
  // removes them. The take's structure is validated here; its note ids are
  // checked against the analysis when it renders.
  [[nodiscard]] foundation::Result<void> set_clip_clay(project::ClipId clip,
                                                       std::optional<project::ClayTake> take);
  // Clay's To MIDI as one undo step: a MIDI clip of `midi` on `track` (a
  // built-in synth track), or on a new Nova track when absent, with the
  // bends as clip automation of the engine's pitch bend. The engine's bend
  // range is raised when the bends need more. Returns the MIDI clip.
  [[nodiscard]] foundation::Result<project::ClipId>
  add_clay_midi_clip(std::optional<project::TrackId> track, const clay::MidiClipOut& midi);
  // Several audio clips' fades as one undo step.
  [[nodiscard]] foundation::Result<void>
  set_audio_clips_fades(std::span<const std::pair<project::ClipId, project::ClipFades>> fades);

private:
  // Processed clip edits (see split_audio_clip_at_sample).
  [[nodiscard]] foundation::Result<bool> audio_clip_is_processed(project::ClipId clip) const;
  // Each side of a split: its fades and its automation.
  struct SplitHalf {
    project::ClipFades fades;
    std::vector<project::AutomationLane> automation;
  };
  struct SplitHalves {
    SplitHalf left;
    SplitHalf right;
  };
  [[nodiscard]] foundation::Result<project::ClipId>
  split_processed_audio_clip(project::ClipId clip, std::int64_t offset, project::ClipId right_id,
                             SplitHalves halves);
  [[nodiscard]] foundation::Result<void> slip_processed_audio_clip(project::ClipId clip,
                                                                   std::int64_t source_frame_delta);
  [[nodiscard]] foundation::Result<void>
  trim_processed_audio_clip(project::ClipId clip, AudioClipEdge edge,
                            timeline::SamplePosition new_edge);
  // A move whose start is before zero first adds
  // the bars before bar 1 it needs, as one edit.
  [[nodiscard]] foundation::Result<void> move_audio_clip_before_zero(project::ClipId clip,
                                                                     timeline::TickPosition start);
};

} // namespace oscillate::application
