#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <oscillate/foundation/error.hpp>
#include <oscillate/project/model.hpp>

namespace oscillate::application {

enum class PlaybackApplication : std::uint8_t { pending, active, failed };

// Control-side description of the accepted edit. IDs remain typed and stable;
// presentation code chooses how many to display. The project snapshot does
// not persist this transient graph-application status.
struct ChangeDescriptor {
  std::string label;
  project::ChangeSet affected;
};

struct RevisionStatus {
  std::uint64_t model_revision = 0;
  std::uint64_t active_graph_revision = 0;
  PlaybackApplication application = PlaybackApplication::pending;
  std::optional<ChangeDescriptor> change;
  std::optional<foundation::Error> failure;
};

// Control-thread read model. Project intent remains committed when a graph
// build or publication fails; the active graph revision stays separately visible.
// UI and export must inspect this state rather than assuming saved == audible.
class RevisionTracker {
public:
  explicit RevisionTracker(std::uint64_t model_revision, std::uint64_t active_graph_revision = 0,
                           std::optional<ChangeDescriptor> initial_change = {}) noexcept;

  [[nodiscard]] bool accept_model_revision(std::uint64_t revision,
                                           ChangeDescriptor change = {"project edit", {}});
  // A retry keeps the same accepted edit and affected IDs visible.
  [[nodiscard]] bool retry_current_revision() noexcept;
  [[nodiscard]] bool observe_active_graph(std::uint64_t revision) noexcept;
  [[nodiscard]] bool fail_revision(std::uint64_t revision, foundation::Error error);
  [[nodiscard]] RevisionStatus snapshot() const { return status_; }

private:
  RevisionStatus status_;
};

} // namespace oscillate::application
