#pragma once

#include "oscillate/application/session/session_service.hpp"

#include <thread>

namespace oscillate::application {

struct ManagedMediaIssue {
  project::SourceId id;
  std::optional<project::SourceRecord> source;
  persistence::LoadedGeneration::SourceDiagnostic::Status status;
};

class MediaRepairService : public SessionService {
public:
  explicit MediaRepairService(SessionState& state) noexcept : SessionService(state) {}

  // Lists saved CURRENT's unavailable sources without changing the model.
  // Only a missing managed asset may be repaired from an exact pinned file;
  // external links require a separate relink operation.
  [[nodiscard]] foundation::Result<std::vector<ManagedMediaIssue>> managed_media_issues() const;
  // Control-thread begin/poll/cancel. A private worker verifies and publishes
  // without blocking the UI; the stopped lease lasts through completion.
  [[nodiscard]] foundation::Result<void> begin_managed_source_repair(project::SourceId source,
                                                                     int candidate_fd);
  [[nodiscard]] bool managed_source_repair_busy() const noexcept;
  [[nodiscard]] bool managed_source_repair_ready() const noexcept;
  void cancel_managed_source_repair() noexcept;
  [[nodiscard]] std::optional<foundation::Result<persistence::RestoreStatus>>
  take_managed_source_repair_result();

private:
  // Stopped and joined before the session state it reads goes.
  std::jthread repair_worker_;
};

} // namespace oscillate::application
