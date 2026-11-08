#pragma once

#include "oscillate/project/model.hpp"

#include <QString>
#include <QStringList>
#include <QVariantList>

#include <map>
#include <optional>
#include <utility>

namespace oscillate::quick {

// UI read model for track identity and route destinations in one project revision.
class TrackRoutingView final {
public:
  // Where a track shows in the track list. The saved order stays as it is;
  // the list derives its own: each group bus with its members right under
  // it (in saved order), other tracks and buses in saved order, and the
  // return buses last, in a section of their own.
  struct Nesting {
    // The track's row in the list, top down.
    int display_index = 0;
    // The group bus it feeds, shown nested under it.
    std::optional<project::TrackId> bus;
    // A group bus: how many tracks it holds.
    int member_count = 0;
    // The bus's last member, where its rail ends.
    bool last_member = false;
    // A return bus: the tracks whose sends feed it, top down.
    QStringList fed_by;
  };

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
  [[nodiscard]] const Nesting& nesting(project::TrackId id) const { return nesting_.at(id); }

private:
  void nest(const project::Snapshot& snapshot);

  // Kind-numbered names ("Group 1"), and the names the UI shows: the
  // track's own, else that one.
  std::map<project::TrackId, QString> names_;
  std::map<project::TrackId, QString> shown_names_;
  std::map<project::TrackId, QString> kinds_;
  std::map<std::pair<project::TrackId, project::InsertSlotId>, QString> sidechain_target_names_;
  QVariantList targets_;
  int target_count_ = 0;
  bool active_ = false;
  std::map<project::TrackId, Nesting> nesting_;
};

} // namespace oscillate::quick
