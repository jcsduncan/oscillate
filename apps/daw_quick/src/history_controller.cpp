#include "history_controller.hpp"

#include "oscillate/application/history_labels.hpp"

#include <QVariantMap>

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace oscillate::quick {

HistoryController::HistoryController(Session session, CanEdit can_edit, Report report,
                                     Commit commit, QObject* parent)
    : QObject(parent), session_(std::move(session)), can_edit_(std::move(can_edit)),
      report_(std::move(report)), commit_(std::move(commit)) {}

void HistoryController::set_active(bool active) {
  if (active_ == active)
    return;
  active_ = active;
  emit activeChanged();
  if (active_) {
    refresh();
  } else {
    labels_.clear();
  }
}

void HistoryController::refresh() {
  if (!active_)
    return;
  auto* session = session_();
  const auto undo =
      session ? session->history().undo_steps() : std::vector<project::ProjectModel::HistoryStep>{};
  const auto redo =
      session ? session->history().redo_steps() : std::vector<project::ProjectModel::HistoryStep>{};
  std::map<Key, Described> kept;
  QVariantList next;
  next.reserve(static_cast<qsizetype>(undo.size()) + static_cast<qsizetype>(redo.size()));
  const auto add = [&](const project::ProjectModel::HistoryStep& step, bool undone) {
    const Key key{step.before.get(), step.after.get()};
    auto found = labels_.find(key);
    std::string label = found != labels_.end()
                            ? found->second.label
                            : application::describe_change(*step.before, *step.after);
    next.push_back(QVariantMap{{"label", QString::fromStdString(label)}, {"undone", undone}});
    kept.insert_or_assign(key, Described{step.before, step.after, std::move(label)});
  };
  for (const auto& step : undo)
    add(step, false);
  for (const auto& step : redo)
    add(step, true);
  labels_ = std::move(kept);
  const auto position = static_cast<int>(undo.size());
  if (next == steps_ && position == position_)
    return;
  steps_ = std::move(next);
  position_ = position;
  emit changed();
}

void HistoryController::jump_to(int position) {
  auto* session = session_();
  if (session == nullptr || !can_edit_())
    return;
  const auto done = static_cast<int>(session->history().undo_steps().size());
  const auto total = done + static_cast<int>(session->history().redo_steps().size());
  const auto target = std::clamp(position, 0, total);
  if (target == done)
    return;
  int moved = 0;
  QString failure;
  for (auto at = done; at != target; at += target < done ? -1 : 1) {
    const auto result = target < done ? session->history().undo() : session->history().redo();
    if (!result) {
      failure = QString::fromStdString(result.error().message);
      break;
    }
    ++moved;
  }
  // One graph update for the whole jump, even a partial one.
  if (moved > 0)
    commit_();
  refresh();
  if (!failure.isEmpty()) {
    report_(QStringLiteral("History stopped after %1 of %2 steps: %3")
                .arg(moved)
                .arg(std::abs(target - done))
                .arg(failure));
    return;
  }
  const auto plural = moved == 1 ? QString() : QStringLiteral("s");
  report_((target < done ? QStringLiteral("Undid %1 step%2") : QStringLiteral("Redid %1 step%2"))
              .arg(moved)
              .arg(plural));
}

} // namespace oscillate::quick
