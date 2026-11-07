#pragma once

#include "oscillate/application/project_session.hpp"

#include <QObject>
#include <QString>
#include <QVariantMap>

#include <functional>

namespace oscillate::quick {

// UI-thread commands for track-owned channel automation. The bridge owns
// project lifecycle and publishes each accepted revision.
class TrackAutomationController final : public QObject {
  Q_OBJECT

public:
  using Session = std::function<application::ProjectSession*()>;
  using CanEdit = std::function<bool()>;
  using Report = std::function<void(QString)>;
  using Commit = std::function<void()>;

  TrackAutomationController(Session session, CanEdit can_edit, Report report, Commit commit,
                            QObject* parent = nullptr);

  Q_INVOKABLE void set_point(const QString& track_id, int zero_based_bar, double value);
  Q_INVOKABLE void set_value(const QString& track_id, const QString& point_id, double value);
  Q_INVOKABLE void move_point(const QString& track_id, const QString& point_id, int zero_based_bar,
                              double value);
  Q_INVOKABLE void remove_point(const QString& track_id, const QString& point_id);
  Q_INVOKABLE void remove_lane(const QString& track_id);
  Q_INVOKABLE void toggle_timebase(const QString& lane_id);
  // Generic saved-lane segment controls shared by channel, clip and plugin
  // editors. `observed_revision` makes QML refresh its read after a commit.
  Q_INVOKABLE [[nodiscard]] QVariantMap segment_curve(const QString& lane_id,
                                                      const QString& point_id,
                                                      qulonglong observed_revision) const;
  Q_INVOKABLE void set_segment_curve(const QString& lane_id, const QString& point_id,
                                     const QVariantMap& curve);

private:
  [[nodiscard]] application::ProjectSession* editable_session();
  void failure(const QString& context, const foundation::Error& error);
  void accepted(const QString& action);

  Session session_;
  CanEdit can_edit_;
  Report report_;
  Commit commit_;
};

} // namespace oscillate::quick
