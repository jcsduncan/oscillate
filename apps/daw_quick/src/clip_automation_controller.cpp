#include "clip_automation_controller.hpp"

#include <cmath>
#include <utility>

namespace oscillate::quick {

ClipAutomationController::ClipAutomationController(Session session, CanEdit can_edit, Report report,
                                                   Commit commit, QObject* parent)
    : QObject(parent), session_(std::move(session)), can_edit_(std::move(can_edit)),
      report_(std::move(report)), commit_(std::move(commit)) {}

application::ProjectSession* ClipAutomationController::editable_session() {
  if (auto* project = session_(); project != nullptr && can_edit_())
    return project;
  report_(QStringLiteral("Finish the current project operation before editing automation"));
  return nullptr;
}

void ClipAutomationController::failure(const foundation::Error& error) {
  report_(QStringLiteral("Clip channel automation: %1").arg(QString::fromStdString(error.message)));
}

void ClipAutomationController::accepted(const QString& action) {
  const auto revision = session_()->snapshot().revision;
  commit_();
  report_(QStringLiteral("%1 · revision %2").arg(action).arg(revision));
}

void ClipAutomationController::set_point(const QString& clip_id, int zero_based_bar, double value) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  if (!clip || zero_based_bar < 0 || zero_based_bar >= 65'536 || !std::isfinite(value) ||
      value < 0.0 || value > 4.0) {
    report_(QStringLiteral("Clip channel point needs a bar from 1 to 65536 and level from 0 to 4"));
    return;
  }
  const auto result =
      current->automation().set_audio_clip_channel_level_point(clip.value(), zero_based_bar, value);
  if (!result)
    return failure(result.error());
  accepted(QStringLiteral("Clip channel point added"));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): QML invokes both numeric values directly.
void ClipAutomationController::set_point_at_fraction(const QString& clip_id, double fraction,
                                                     double value) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  if (!clip || !std::isfinite(fraction) || fraction < 0.0 || fraction >= 1.0 ||
      !std::isfinite(value) || value < 0.0 || value > 4.0) {
    report_(QStringLiteral("Choose a position inside the clip and level from 0 to 4"));
    return;
  }
  const auto result = current->automation().set_audio_clip_channel_level_point_at_fraction(
      clip.value(), application::ClipFraction{fraction}, value);
  if (!result)
    return failure(result.error());
  accepted(QStringLiteral("Clip channel point added"));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): QML invokes both numeric values directly.
void ClipAutomationController::move_point(const QString& clip_id, const QString& point_id,
                                          double fraction, double value) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  const auto point = project::AutomationPointId::parse(point_id.toStdString());
  if (!clip || !point || !std::isfinite(fraction) || fraction < 0.0 || fraction >= 1.0 ||
      !std::isfinite(value) || value < 0.0 || value > 4.0) {
    report_(QStringLiteral("Choose a clip point, position and level from 0 to 4"));
    return;
  }
  const auto result = current->automation().move_audio_clip_channel_level_point(
      clip.value(), point.value(), application::ClipFraction{fraction}, value);
  if (!result)
    return failure(result.error());
  accepted(QStringLiteral("Clip channel point moved"));
}

void ClipAutomationController::set_value(const QString& clip_id, const QString& point_id,
                                         double value) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  const auto point = project::AutomationPointId::parse(point_id.toStdString());
  if (!clip || !point || !std::isfinite(value) || value < 0.0 || value > 4.0) {
    report_(QStringLiteral("Select a clip channel point and level from 0 to 4"));
    return;
  }
  const auto result =
      current->automation().set_audio_clip_channel_level_value(clip.value(), point.value(), value);
  if (!result)
    return failure(result.error());
  accepted(QStringLiteral("Clip channel level changed"));
}

void ClipAutomationController::remove_point(const QString& clip_id, const QString& point_id) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  const auto point = project::AutomationPointId::parse(point_id.toStdString());
  if (!clip || !point) {
    report_(QStringLiteral("Select a clip channel point to remove"));
    return;
  }
  const auto result =
      current->automation().remove_audio_clip_channel_level_point(clip.value(), point.value());
  if (!result)
    return failure(result.error());
  accepted(QStringLiteral("Clip channel point removed"));
}

void ClipAutomationController::remove_lane(const QString& clip_id) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  if (!clip) {
    report_(QStringLiteral("Select a clip channel lane to remove"));
    return;
  }
  const auto result = current->automation().remove_audio_clip_channel_level_lane(clip.value());
  if (!result)
    return failure(result.error());
  accepted(QStringLiteral("Clip channel lane removed"));
}

} // namespace oscillate::quick
