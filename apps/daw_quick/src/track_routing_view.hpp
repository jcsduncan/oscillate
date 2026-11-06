#pragma once

#include "oscillate/project/model.hpp"

#include <QString>
#include <QVariantList>

#include <map>
#include <optional>
#include <utility>

namespace oscillate::quick {

// UI read model for track identity and route destinations in one project revision.
class TrackRoutingView final {
public:
  explicit TrackRoutingView(const project::Snapshot& snapshot);

  [[nodiscard]] const QString& name(project::TrackId id) const { return names_.at(id); }
  [[nodiscard]] const QString& kind(project::TrackId id) const { return kinds_.at(id); }
  [[nodiscard]] QString main_destination_name(std::optional<project::TrackId> destination) const;
  [[nodiscard]] QString send_destination_name(project::TrackId destination) const;
  [[nodiscard]] QString sidechain_target_name(project::TrackId destination,
                                              project::InsertSlotId slot) const;
  [[nodiscard]] const QVariantList& targets() const noexcept { return targets_; }
  [[nodiscard]] int target_count() const noexcept { return target_count_; }
  [[nodiscard]] bool active() const noexcept { return active_; }

private:
  std::map<project::TrackId, QString> names_;
  std::map<project::TrackId, QString> kinds_;
  std::map<std::pair<project::TrackId, project::InsertSlotId>, QString> sidechain_target_names_;
  QVariantList targets_;
  int target_count_ = 0;
  bool active_ = false;
};

} // namespace oscillate::quick
