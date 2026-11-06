#pragma once

#include "oscillate/application/session/session_service.hpp"

namespace oscillate::application {

class HistoryService : public SessionService {
public:
  explicit HistoryService(SessionState& state) noexcept : SessionService(state) {}

  [[nodiscard]] foundation::Result<void> undo();
  [[nodiscard]] foundation::Result<void> redo();
  // Read-only history for the history panel (history_labels.hpp names it).
  [[nodiscard]] std::vector<project::ProjectModel::HistoryStep> undo_steps() const;
  [[nodiscard]] std::vector<project::ProjectModel::HistoryStep> redo_steps() const;
};

} // namespace oscillate::application
