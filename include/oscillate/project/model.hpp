#pragma once

// The project model: commands validate, publish one revision and keep undo
// history. The umbrella for the project state types and commands.

#include "oscillate/foundation/error.hpp"
#include "oscillate/project/automation_types.hpp"
#include "oscillate/project/clip_types.hpp"
#include "oscillate/project/commands.hpp"
#include "oscillate/project/ids.hpp"
#include "oscillate/project/native_document.hpp"
#include "oscillate/project/plugin_types.hpp"
#include "oscillate/project/snapshot.hpp"
#include "oscillate/project/source_types.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::project {

// TransportGate must outlive ProjectModel and all outstanding revision leases.
// The application sequencer must serialize acquire_stopped() with transport starts.
// The lease is an exclusive authority, not a sampled boolean. A real adapter must
// prevent a concurrent start while it exists.
class StoppedTransportLease {
public:
  virtual ~StoppedTransportLease() = default;
};
class TransportGate {
public:
  virtual ~TransportGate() = default;
  [[nodiscard]] virtual foundation::Result<std::unique_ptr<StoppedTransportLease>>
  acquire_stopped() = 0;
};

// A rate-changing apply/undo/redo transfers the stopped lease to this result.
// The application must hold it through device renegotiation scheduling/acknowledgement
// before allowing transport to start. Other revisions carry no lease. This contract
// does not implement the device transition itself.
struct AppliedRevision {
  std::shared_ptr<const Snapshot> snapshot;
  ChangeSet changes;
  std::unique_ptr<StoppedTransportLease> stopped_transport;
};

// Undo history retention. Undo keeps at most `steps` entries, and trims
// the oldest while the history's estimated bytes (estimated_bytes of each
// snapshot it alone keeps alive) exceed `bytes`, but never below
// `kept_steps`, so even a project with large plugin states can undo its
// recent edits. Redo is never trimmed: it only holds steps just undone.
// A 16-track song with 175 clips and 1,600 notes estimates about 170 KB a
// snapshot, so the depth decides there (about 80 MiB); a project holding
// several MiB of plugin state reaches the bytes limit first.
inline constexpr std::size_t default_undo_steps = 500;
inline constexpr std::size_t default_undo_history_bytes = std::size_t{256} * 1024 * 1024;
inline constexpr std::size_t default_kept_undo_steps = 32;
struct HistoryLimits {
  std::size_t steps = default_undo_steps;
  std::size_t bytes = default_undo_history_bytes;
  std::size_t kept_steps = default_kept_undo_steps;
};

class ProjectModel {
public:
  ProjectModel(const ProjectModel&) = delete;
  ProjectModel& operator=(const ProjectModel&) = delete;
  ProjectModel(ProjectModel&&) noexcept = default;
  ProjectModel& operator=(ProjectModel&&) noexcept = default;

  [[nodiscard]] static foundation::Result<ProjectModel> create(ProjectId id, std::uint32_t rate,
                                                               timeline::TempoMap tempo,
                                                               timeline::MeterMap meter,
                                                               TransportGate& transport);
  // Rehydrate a decoded snapshot after the storage layer validates its schema.
  // Undo/redo starts empty; validation is repeated before publication.
  [[nodiscard]] static foundation::Result<ProjectModel> open(Snapshot snapshot,
                                                             TransportGate& transport);

  [[nodiscard]] std::shared_ptr<const Snapshot> snapshot() const noexcept { return current_; }
  [[nodiscard]] std::size_t undo_depth() const noexcept { return undo_.size(); }
  [[nodiscard]] std::size_t redo_depth() const noexcept { return redo_.size(); }
  // The estimated bytes of the snapshots only the history keeps alive.
  [[nodiscard]] std::size_t history_bytes() const noexcept { return history_bytes_; }
  [[nodiscard]] const HistoryLimits& history_limits() const noexcept { return limits_; }
  // Applies to the current history at once, trimming its oldest undo steps.
  void set_history_limits(HistoryLimits limits);
  // One undoable step, read-only: the snapshots either side of it.
  struct HistoryStep {
    std::shared_ptr<const Snapshot> before;
    std::shared_ptr<const Snapshot> after;
  };
  // The undo steps oldest first, and the redo steps next-to-redo first.
  [[nodiscard]] std::vector<HistoryStep> undo_steps() const;
  [[nodiscard]] std::vector<HistoryStep> redo_steps() const;

  // The control sequencer is the sole caller. One call publishes at most one revision.
  // A nonzero gesture groups consecutive accepted calls into a single undo step.
  [[nodiscard]] foundation::Result<AppliedRevision>
  apply(const std::vector<Operation>& operations, std::optional<std::uint64_t> gesture = {});
  // Return to a validated snapshot of this project as one undoable model edit.
  // Its saved revision is not reused: publication advances the current
  // revision, invalidating in-flight work from either state. The caller owns
  // storage selection and save; this method never advances CURRENT itself.
  [[nodiscard]] foundation::Result<AppliedRevision> restore_snapshot(Snapshot saved);
  // A read-only playback mirror may follow a newer authoritative project
  // revision without creating its own undo history. The sample rate must stay
  // fixed while its device is active.
  [[nodiscard]] foundation::Result<AppliedRevision> adopt_authoritative_snapshot(Snapshot saved);
  [[nodiscard]] foundation::Result<AppliedRevision> undo();
  [[nodiscard]] foundation::Result<AppliedRevision> redo();

private:
  struct HistoryEntry {
    std::shared_ptr<const Snapshot> before;
    std::shared_ptr<const Snapshot> after;
    std::optional<std::uint64_t> gesture;
    // estimated_bytes of each side, measured once when it was published.
    std::size_t before_bytes = 0;
    std::size_t after_bytes = 0;
  };

  explicit ProjectModel(std::shared_ptr<const Snapshot> initial, TransportGate& transport)
      : current_(std::move(initial)), current_bytes_(estimated_bytes(*current_)),
        transport_(&transport) {}

  [[nodiscard]] foundation::Result<AppliedRevision> publish_history(const Snapshot& target,
                                                                    bool undoing);

  // Publishes `next` as current with an undo step from the old current,
  // clears redo and trims the history to its limits.
  void push_undo(std::shared_ptr<const Snapshot> next, std::optional<std::uint64_t> gesture);
  void recount_history();
  void trim_history();

  std::shared_ptr<const Snapshot> current_;
  std::size_t current_bytes_ = 0;
  TransportGate* transport_;
  std::deque<HistoryEntry> undo_;
  std::deque<HistoryEntry> redo_;
  std::size_t history_bytes_ = 0;
  HistoryLimits limits_;
  std::optional<std::uint64_t> eligible_gesture_;
};

} // namespace oscillate::project
