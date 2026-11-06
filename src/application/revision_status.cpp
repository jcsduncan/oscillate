#include <oscillate/application/revision_status.hpp>

#include <utility>

namespace oscillate::application {

RevisionTracker::RevisionTracker(std::uint64_t model_revision, std::uint64_t active_graph_revision,
                                 std::optional<ChangeDescriptor> initial_change) noexcept {
  status_.model_revision = model_revision;
  status_.active_graph_revision =
      active_graph_revision <= model_revision ? active_graph_revision : 0;
  status_.application =
      status_.model_revision != 0 && status_.active_graph_revision == status_.model_revision
          ? PlaybackApplication::active
          : PlaybackApplication::pending;
  status_.change = std::move(initial_change);
}

bool RevisionTracker::accept_model_revision(std::uint64_t revision, ChangeDescriptor change) {
  if (revision == 0 || revision <= status_.model_revision) {
    return false;
  }
  status_.model_revision = revision;
  status_.application = PlaybackApplication::pending;
  status_.change = std::move(change);
  status_.failure.reset();
  return true;
}

bool RevisionTracker::retry_current_revision() noexcept {
  if (status_.application != PlaybackApplication::failed ||
      status_.model_revision == status_.active_graph_revision) {
    return false;
  }
  status_.application = PlaybackApplication::pending;
  status_.failure.reset();
  return true;
}

bool RevisionTracker::observe_active_graph(std::uint64_t revision) noexcept {
  if (revision < status_.active_graph_revision || revision > status_.model_revision) {
    return false;
  }
  status_.active_graph_revision = revision;
  if (revision == status_.model_revision) {
    status_.application = PlaybackApplication::active;
    status_.failure.reset();
  }
  return true;
}

bool RevisionTracker::fail_revision(std::uint64_t revision, foundation::Error error) {
  if (revision == 0 || revision != status_.model_revision ||
      revision == status_.active_graph_revision) {
    return false;
  }
  status_.application = PlaybackApplication::failed;
  status_.failure = std::move(error);
  return true;
}

} // namespace oscillate::application
