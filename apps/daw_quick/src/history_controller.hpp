#pragma once

#include "oscillate/application/project_session.hpp"

#include <QObject>
#include <QString>
#include <QVariantList>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace oscillate::quick {

// The undo history panel: every undo and redo step by what it changed, and
// a click to go back (or forward) to any of them in one go. Steps are named
// from the snapshots either side (application::describe_change), cached by
// snapshot so a long history is described once.
class HistoryController final : public QObject {
  Q_OBJECT
  // [{label, undone}] oldest first; redo steps (undone) after the undo ones.
  Q_PROPERTY(QVariantList steps READ steps NOTIFY changed)
  // How many steps are done: steps before it are applied, the rest undone.
  Q_PROPERTY(int position READ position NOTIFY changed)
  // Set while the panel shows; a hidden panel describes nothing.
  Q_PROPERTY(bool active READ active WRITE set_active NOTIFY activeChanged)

public:
  using Session = std::function<application::ProjectSession*()>;
  using CanEdit = std::function<bool()>;
  using Report = std::function<void(QString)>;
  using Commit = std::function<void()>;

  HistoryController(Session session, CanEdit can_edit, Report report, Commit commit,
                    QObject* parent = nullptr);

  [[nodiscard]] QVariantList steps() const { return steps_; }
  [[nodiscard]] int position() const noexcept { return position_; }
  [[nodiscard]] bool active() const noexcept { return active_; }
  void set_active(bool active);

  // Re-reads the history; call after every revision.
  Q_INVOKABLE void refresh();
  // Undoes or redoes until `position` steps are done (0: before the first).
  // The playing graph follows once, from the final snapshot.
  Q_INVOKABLE void jump_to(int position);

signals:
  void changed();
  void activeChanged();

private:
  using Key = std::pair<const project::Snapshot*, const project::Snapshot*>;
  struct Described {
    // Held so the key's addresses stay unique while cached.
    std::shared_ptr<const project::Snapshot> before;
    std::shared_ptr<const project::Snapshot> after;
    std::string label;
  };

  Session session_;
  CanEdit can_edit_;
  Report report_;
  Commit commit_;
  std::map<Key, Described> labels_;
  QVariantList steps_;
  int position_ = 0;
  bool active_ = false;
};

} // namespace oscillate::quick
