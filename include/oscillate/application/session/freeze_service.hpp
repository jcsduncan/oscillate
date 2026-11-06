#pragma once

#include "oscillate/application/session/session_service.hpp"

#include "oscillate/application/session/save_service.hpp"
#include <functional>
#include <memory>

namespace oscillate::application {

// A track freeze's render being staged as a managed WAV (ADR-0059). Run the
// task on a worker, then pass its StagedWav to finish_track_freeze on the
// same control sequencer, with this origin revision and track.
struct TrackFreezeStage {
  persistence::WavStageTask task;
  std::uint64_t origin_revision;
  project::TrackId track;
};

// A track's own render (exporting::render_track) for Freeze, Bounce and
// Flatten (ADR-0059). The task pins the snapshot and a store reader; run()
// renders on a worker into a private file, already unlinked, whose
// descriptor the result owns. Stage it with begin_track_render_stage.
class TrackRender {
public:
  TrackRender(int fd, std::uint64_t first_frame, std::uint64_t origin_revision,
              project::TrackId track) noexcept
      : fd_(fd), first_frame_(first_frame), origin_revision_(origin_revision), track_(track) {}
  TrackRender(TrackRender&& other) noexcept;
  TrackRender& operator=(TrackRender&& other) noexcept;
  TrackRender(const TrackRender&) = delete;
  TrackRender& operator=(const TrackRender&) = delete;
  ~TrackRender();

  [[nodiscard]] int fd() const noexcept { return fd_; }
  // The project frame the render's frame 0 sounds at.
  [[nodiscard]] std::uint64_t first_frame() const noexcept { return first_frame_; }
  // The revision rendered: a later audible change makes the render stale.
  [[nodiscard]] std::uint64_t origin_revision() const noexcept { return origin_revision_; }
  [[nodiscard]] project::TrackId track() const noexcept { return track_; }

private:
  int fd_ = -1;
  std::uint64_t first_frame_ = 0;
  std::uint64_t origin_revision_ = 0;
  project::TrackId track_;
};

class TrackRenderTask {
public:
  TrackRenderTask(TrackRenderTask&&) noexcept = default;
  TrackRenderTask& operator=(TrackRenderTask&&) noexcept = default;
  TrackRenderTask(const TrackRenderTask&) = delete;
  TrackRenderTask& operator=(const TrackRenderTask&) = delete;

  [[nodiscard]] std::uint64_t revision() const noexcept { return snapshot_->revision; }
  [[nodiscard]] project::TrackId track() const noexcept { return track_; }
  // Worker only. progress gets (rendered, total) project frames; false cancels.
  [[nodiscard]] foundation::Result<TrackRender>
  run(std::function<bool(std::uint64_t, std::uint64_t)> progress = {}) &&;

private:
  friend class FreezeService;
  TrackRenderTask(std::shared_ptr<const project::Snapshot> snapshot,
                  persistence::ProjectStore reader, project::TrackId track,
                  std::optional<std::pair<std::uint64_t, std::uint64_t>> range)
      : snapshot_(std::move(snapshot)), reader_(std::move(reader)), track_(track), range_(range) {}

  std::shared_ptr<const project::Snapshot> snapshot_;
  persistence::ProjectStore reader_;
  project::TrackId track_;
  std::optional<std::pair<std::uint64_t, std::uint64_t>> range_;
};

// What a bounced render becomes (ADR-0059). A new audio track below the
// source plays it and the source is muted, kept for later; or, for Flatten,
// the audio track takes the source's place, channel, sends and channel
// lanes, and the source with its devices is removed.
enum class BounceMode : std::uint8_t { new_track, flatten };

class FreezeService : public SessionService {
public:
  FreezeService(SessionState& state, SaveService& saving) noexcept
      : SessionService(state), saving_(saving) {}

  // Freeze: begin stages a render of `track` (a stereo WAV at the project
  // rate, read from render_fd) captured against the current revision; finish
  // commits it in one undo step and saves, provided nothing audible changed
  // since. The frozen track plays the render from project frame first_frame.
  [[nodiscard]] foundation::Result<TrackFreezeStage> begin_track_freeze(project::TrackId track,
                                                                        int render_fd);
  [[nodiscard]] foundation::Result<void> finish_track_freeze(persistence::StagedWav&& render,
                                                             std::uint64_t origin_revision,
                                                             project::TrackId track,
                                                             std::uint64_t first_frame = 0);
  // Back to playing the instrument and effects; the render's source stays.
  [[nodiscard]] foundation::Result<void> unfreeze_track(project::TrackId track);
  // A track's own render for Freeze, Bounce and Flatten (ADR-0059): all of
  // it, or exactly `range` of the project timeline (Bounce selection).
  [[nodiscard]] foundation::Result<TrackRenderTask>
  begin_track_render(project::TrackId track,
                     std::optional<std::pair<std::uint64_t, std::uint64_t>> range = std::nullopt);
  // Stages a finished render as the next managed source. Run the stage task
  // on a worker, then pass its StagedWav to finish_track_freeze (with the
  // render's origin revision and first frame) or finish_track_bounce.
  [[nodiscard]] foundation::Result<TrackFreezeStage>
  begin_track_render_stage(const TrackRender& render);
  // A staged render becomes audio in one undo step, then saves: a new audio
  // track (the source muted) or, for Flatten, the source's replacement.
  // Refused as stale when anything audible changed since origin_revision.
  [[nodiscard]] foundation::Result<project::TrackId>
  finish_track_bounce(persistence::StagedWav&& render, std::uint64_t origin_revision,
                      project::TrackId track, std::uint64_t first_frame, BounceMode mode);

private:
  SaveService& saving_;
};

} // namespace oscillate::application
