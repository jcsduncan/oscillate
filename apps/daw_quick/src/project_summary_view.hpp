#pragma once

#include "oscillate/project/model.hpp"

#include <QString>

namespace oscillate::quick {

// UI-thread read model for the saved project's format and master level.
// Replaced from one snapshot on every bridge refresh; it owns no project state.
class ProjectSummaryView final {
public:
  void refresh(const project::Snapshot& snapshot);

  [[nodiscard]] const QString& format() const noexcept { return format_; }
  [[nodiscard]] double master_level() const noexcept { return master_level_; }

private:
  QString format_;
  double master_level_ = 1.0;
};

} // namespace oscillate::quick
