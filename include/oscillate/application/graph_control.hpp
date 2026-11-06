#pragma once

#include <oscillate/application/clip_playback.hpp>
#include <oscillate/application/revision_status.hpp>
#include <oscillate/engine/graph.hpp>
#include <oscillate/project/model.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oscillate::application {

// One prepared WAV feed per clip. A later graph builder will accept a complete
// routing plan; this control contract never silently omits a clip.
struct ClipFeed {
  project::ClipId clip;
  std::shared_ptr<media::PrefetchRing> ring;
  std::shared_ptr<PlaybackReadiness> readiness;
  std::uint64_t instance_id;
};

// All methods run on the serialized, non-real-time control sequencer. The
// ProjectModel, GraphPublisher, feed and callback must outlive this object.
// The callback alone calls GraphPublisher::render_block. A committed project
// edit remains committed if graph preparation fails.
class GraphControl {
public:
  GraphControl(project::ProjectModel& model, engine::GraphPublisher& graph, std::size_t max_frames);

  // Use after a successful ProjectModel::apply/undo/redo. A stale result is
  // ignored. The error describes graph application, never command acceptance.
  [[nodiscard]] foundation::Result<void> submit(const project::AppliedRevision& applied,
                                                const std::optional<ClipFeed>& feed,
                                                std::string_view command_label);
  [[nodiscard]] foundation::Result<void> submit(const project::AppliedRevision& applied,
                                                std::span<const ClipFeed> feeds,
                                                std::string_view command_label);
  // Initial graph publication for a newly opened model.
  [[nodiscard]] foundation::Result<void> submit_current(const std::optional<ClipFeed>& feed);
  [[nodiscard]] foundation::Result<void> submit_current(std::span<const ClipFeed> feeds);
  [[nodiscard]] foundation::Result<void> retry(const std::optional<ClipFeed>& feed);
  [[nodiscard]] foundation::Result<void> retry(std::span<const ClipFeed> feeds);
  // Undo is another committed revision even if its graph preparation fails.
  [[nodiscard]] foundation::Result<project::AppliedRevision>
  undo_and_submit(const std::optional<ClipFeed>& feed);
  [[nodiscard]] foundation::Result<project::AppliedRevision>
  undo_and_submit(std::span<const ClipFeed> feeds);

  // Reclaim and observe only off the callback. Actual activation occurs at a
  // callback block boundary, so published is not synonymous with active.
  void observe();
  [[nodiscard]] RevisionStatus status() const { return revisions_.snapshot(); }
  [[nodiscard]] foundation::Result<void>
  require_export_ready(std::uint64_t intended_revision) const;

private:
  [[nodiscard]] foundation::Result<void>
  build_and_publish(const std::shared_ptr<const project::Snapshot>& built_snapshot,
                    std::span<const ClipFeed> feeds);
  [[nodiscard]] foundation::Result<void> fail(std::uint64_t revision, foundation::Error error);

  project::ProjectModel& model_;
  engine::GraphPublisher& graph_;
  std::size_t max_frames_;
  RevisionTracker revisions_;
};

} // namespace oscillate::application
