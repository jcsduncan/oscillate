#pragma once

#include "oscillate/application/project_session.hpp"

#include <QObject>
#include <QString>

#include <functional>

namespace oscillate::quick {

// UI-thread command adapter for clip-owned channel automation. The owning
// bridge supplies project lifecycle and publishes an accepted edit once.
class ClipAutomationController final : public QObject {
  Q_OBJECT

public:
  using Session = std::function<application::ProjectSession*()>;
  using CanEdit = std::function<bool()>;
  using Report = std::function<void(QString)>;
  using Commit = std::function<void()>;

  ClipAutomationController(Session session, CanEdit can_edit, Report report, Commit commit,
                           QObject* parent = nullptr);

  Q_INVOKABLE void set_point_at_fraction(const QString& clip_id, double fraction, double value);
  Q_INVOKABLE void move_point(const QString& clip_id, const QString& point_id, double fraction,
                              double value);
  Q_INVOKABLE void set_value(const QString& clip_id, const QString& point_id, double value);
  Q_INVOKABLE void remove_point(const QString& clip_id, const QString& point_id);
  Q_INVOKABLE void remove_lane(const QString& clip_id);

private:
  [[nodiscard]] application::ProjectSession* editable_session();
  void failure(const foundation::Error& error);
  void accepted(const QString& action);

  Session session_;
  CanEdit can_edit_;
  Report report_;
  Commit commit_;
};

} // namespace oscillate::quick
