#pragma once

#include "oscillate/application/session/session_service.hpp"

namespace oscillate::application {

class WarpService : public SessionService {
public:
  explicit WarpService(SessionState& state) noexcept : SessionService(state) {}

  // Applies a reviewed placement and processing (auto-warp or tempo conform)
  // as one undoable edit, only if the clip's settings generation is still
  // `settings_generation`.
  [[nodiscard]] foundation::Result<void> apply_audio_clip_warp(project::ClipId clip,
                                                               std::uint64_t settings_generation,
                                                               project::Placement placement,
                                                               project::ClipProcessing processing);
  // The clip strip (ADR-0053), each one undoable edit. Follow on plays the
  // clip on the project's bars at its source's reviewed BPM, its downbeat
  // snapped to the nearest bar; off plays it unstretched in real time.
  [[nodiscard]] foundation::Result<void> set_audio_clip_follow(project::ClipId clip, bool follow);
  // Corrects the clip's source BPM (absent: back to the detected tempo).
  // Every clip of that source that follows the project tempo re-conforms
  // with its downbeat where it plays now.
  [[nodiscard]] foundation::Result<void> set_audio_clip_source_bpm(project::ClipId clip,
                                                                   std::optional<double> bpm);
  // Marks `source_frame` as a downbeat of the clip's source; a following
  // clip moves so it lands on the nearest bar.
  [[nodiscard]] foundation::Result<void> set_audio_clip_downbeat(project::ClipId clip,
                                                                 std::uint64_t source_frame);
};

} // namespace oscillate::application
