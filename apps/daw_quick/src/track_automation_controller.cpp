#include "track_automation_controller.hpp"

#include <cmath>
#include <ranges>
#include <utility>

namespace oscillate::quick {

TrackAutomationController::TrackAutomationController(Session session, CanEdit can_edit,
                                                     Report report, Commit commit, QObject* parent)
    : QObject(parent), session_(std::move(session)), can_edit_(std::move(can_edit)),
      report_(std::move(report)), commit_(std::move(commit)) {}

application::ProjectSession* TrackAutomationController::editable_session() {
  if (auto* project = session_(); project != nullptr && can_edit_())
    return project;
  report_(QStringLiteral("Finish the current project operation before editing automation"));
  return nullptr;
}

void TrackAutomationController::failure(const QString& context, const foundation::Error& error) {
  report_(QStringLiteral("%1: %2").arg(context, QString::fromStdString(error.message)));
}

void TrackAutomationController::accepted(const QString& action) {
  const auto revision = session_()->snapshot().revision;
  commit_();
  report_(QStringLiteral("%1 · revision %2").arg(action).arg(revision));
}

void TrackAutomationController::set_point(const QString& track_id, int zero_based_bar,
                                          double value) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto parsed = project::TrackId::parse(track_id.toStdString());
  if (!parsed || zero_based_bar < 0 || zero_based_bar >= 65'536 || !std::isfinite(value) ||
      value < 0.0 || value > 4.0) {
    report_(QStringLiteral("Channel point needs a bar from 1 to 65536 and level from 0 to 4"));
    return;
  }
  const auto track =
      std::ranges::find(current->snapshot().tracks, parsed.value(), &project::Track::id);
  if (track == current->snapshot().tracks.end()) {
    report_(QStringLiteral("Select a track for channel automation"));
    return;
  }
  const auto result = current->parameter_automation().set_channel_level_automation_point(
      parsed.value(), zero_based_bar, value);
  if (!result)
    return failure(QStringLiteral("Channel automation"), result.error());
  accepted(QStringLiteral("Channel point added"));
}

void TrackAutomationController::set_value(const QString& track_id, const QString& point_id,
                                          double value) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto track = project::TrackId::parse(track_id.toStdString());
  const auto point = project::AutomationPointId::parse(point_id.toStdString());
  if (!track || !point || !std::isfinite(value) || value < 0.0 || value > 4.0) {
    report_(QStringLiteral("Select a channel point and level from 0 to 4"));
    return;
  }
  const auto result = current->parameter_automation().set_channel_level_automation_value(
      track.value(), point.value(), value);
  if (!result)
    return failure(QStringLiteral("Channel automation"), result.error());
  accepted(QStringLiteral("Channel level changed"));
}

void TrackAutomationController::move_point(const QString& track_id, const QString& point_id,
                                           int zero_based_bar, double value) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto track = project::TrackId::parse(track_id.toStdString());
  const auto point = project::AutomationPointId::parse(point_id.toStdString());
  if (!track || !point || zero_based_bar < 0 || zero_based_bar >= 65'536 || !std::isfinite(value) ||
      value < 0.0 || value > 4.0) {
    report_(QStringLiteral("Channel point needs a bar from 1 to 65536 and level from 0 to 4"));
    return;
  }
  const auto before = current->snapshot().revision;
  const auto result = current->parameter_automation().move_channel_level_automation_point(
      track.value(), point.value(), zero_based_bar, value);
  if (!result)
    return failure(QStringLiteral("Channel automation"), result.error());
  if (current->snapshot().revision != before)
    accepted(QStringLiteral("Channel point moved"));
}

void TrackAutomationController::remove_point(const QString& track_id, const QString& point_id) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto track = project::TrackId::parse(track_id.toStdString());
  const auto point = project::AutomationPointId::parse(point_id.toStdString());
  if (!track || !point) {
    report_(QStringLiteral("Select a channel point to remove"));
    return;
  }
  const auto result = current->parameter_automation().remove_channel_level_automation_point(
      track.value(), point.value());
  if (!result)
    return failure(QStringLiteral("Channel automation"), result.error());
  accepted(QStringLiteral("Channel point removed"));
}

void TrackAutomationController::remove_lane(const QString& track_id) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto track = project::TrackId::parse(track_id.toStdString());
  if (!track) {
    report_(QStringLiteral("Select a channel lane to remove"));
    return;
  }
  const auto result =
      current->parameter_automation().remove_channel_level_automation_lane(track.value());
  if (!result)
    return failure(QStringLiteral("Channel automation"), result.error());
  accepted(QStringLiteral("Channel lane removed"));
}

void TrackAutomationController::toggle_timebase(const QString& lane_id) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto lane = project::AutomationLaneId::parse(lane_id.toStdString());
  if (!lane) {
    report_(QStringLiteral("Select an automation lane"));
    return;
  }
  const auto result = current->automation().toggle_automation_lane_timebase(lane.value());
  if (!result)
    return failure(QStringLiteral("Automation timebase"), result.error());
  accepted(
      QStringLiteral("Automation timebase changed; check transport status for live application"));
}

QVariantMap TrackAutomationController::segment_curve(const QString& lane_id,
                                                     const QString& point_id,
                                                     qulonglong observed_revision) const {
  (void)observed_revision;
  const auto lane_id_parsed = project::AutomationLaneId::parse(lane_id.toStdString());
  const auto point_id_parsed = project::AutomationPointId::parse(point_id.toStdString());
  const auto* current = session_();
  if (!current || !lane_id_parsed || !point_id_parsed)
    return {{"present", false}};
  const project::AutomationLane* lane = nullptr;
  for (const auto& track : current->snapshot().tracks) {
    const auto find_in = [&](const auto& lanes) {
      return std::ranges::find(lanes, lane_id_parsed.value(), &project::AutomationLane::id);
    };
    if (const auto found = find_in(track.automation); found != track.automation.end()) {
      lane = &*found;
      break;
    }
    for (const auto& clip : track.clips) {
      if (const auto found = find_in(clip.automation); found != clip.automation.end()) {
        lane = &*found;
        break;
      }
    }
    if (lane != nullptr)
      break;
    for (const auto& clip : track.midi_clips) {
      if (const auto found = find_in(clip.automation); found != clip.automation.end()) {
        lane = &*found;
        break;
      }
    }
    if (lane != nullptr)
      break;
  }
  if (lane == nullptr)
    return {{"present", false}};
  const auto point =
      std::ranges::find(lane->points, point_id_parsed.value(), &project::AutomationPoint::id);
  if (point == lane->points.end() || point + 1 == lane->points.end())
    return {{"present", false}};
  return {{"present", true},
          {"discrete", lane->domain.discrete},
          {"shapeIndex", static_cast<int>(point->to_next)},
          {"x1", point->handles.x1},
          {"y1", point->handles.y1},
          {"x2", point->handles.x2},
          {"y2", point->handles.y2}};
}

void TrackAutomationController::set_segment_curve(const QString& lane_id, const QString& point_id,
                                                  const QVariantMap& curve) {
  auto* current = editable_session();
  if (!current)
    return;
  const auto lane = project::AutomationLaneId::parse(lane_id.toStdString());
  const auto point = project::AutomationPointId::parse(point_id.toStdString());
  bool shape_ok = false;
  const int index = curve.value(QStringLiteral("shapeIndex")).toInt(&shape_ok);
  const auto field = [&](const char* name, bool& valid) {
    const auto value = curve.value(QLatin1String(name));
    bool converted = false;
    const double result = value.toDouble(&converted);
    valid = valid && value.isValid() && converted;
    return result;
  };
  bool fields_ok = true;
  const project::AutomationHandles handles{field("x1", fields_ok), field("y1", fields_ok),
                                           field("x2", fields_ok), field("y2", fields_ok)};
  if (!lane || !point || !shape_ok || index < 0 ||
      index > static_cast<int>(project::AutomationShape::bezier) || !fields_ok) {
    report_(QStringLiteral("Select a valid segment shape and four Bézier handles"));
    return;
  }
  const auto revision = current->snapshot().revision;
  const auto result = current->automation().set_automation_segment_curve(
      lane.value(), point.value(), static_cast<project::AutomationShape>(index), handles);
  if (!result)
    return failure(QStringLiteral("Automation curve"), result.error());
  if (current->snapshot().revision != revision)
    accepted(QStringLiteral(
        "Automation segment curve changed; check transport status for live application"));
}

} // namespace oscillate::quick
