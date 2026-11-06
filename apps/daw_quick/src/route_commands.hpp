#pragma once

#include "oscillate/application/project_session.hpp"

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

namespace oscillate::quick {

// UI-thread owner for saved main and send routing commands. ProjectBridge
// supplies lifecycle, edit availability and revision publication callbacks.
class RouteCommands final : public QObject {
  Q_OBJECT

public:
  using Session = std::function<application::ProjectSession*()>;
  using CanEdit = std::function<bool()>;
  using Report = std::function<void(QString)>;
  using Commit = std::function<void()>;

  RouteCommands(Session session, CanEdit can_edit, Report report, Commit commit,
                QObject* parent = nullptr);

  Q_INVOKABLE void set_main_route(const QString& track_id, const QString& destination_id);
  Q_INVOKABLE void add_send_route(const QString& track_id, const QString& destination_id,
                                  double gain);
  Q_INVOKABLE void add_sidechain_route(const QString& track_id, const QString& destination_id,
                                       const QString& effect_slot_id, double gain);
  // From an effect's own panel: the track keying its sidechain input
  // (empty for none) and the key gain, in one undoable step.
  Q_INVOKABLE void set_sidechain_source(const QString& destination_id,
                                        const QString& effect_slot_id, const QString& source_id,
                                        double gain);
  // Duck tracks to a key in one undoable step: each target's Pump (an
  // existing one or a new one at the end of its chain) is keyed from the
  // source, and all of them are linked to one shape.
  Q_INVOKABLE void duck_tracks_to(const QString& source_id, const QStringList& target_ids);
  Q_INVOKABLE void set_send_gain(const QString& send_id, double gain);
  Q_INVOKABLE void remove_send(const QString& send_id);

private:
  [[nodiscard]] application::ProjectSession* editable_session();
  void failure(const QString& context, const foundation::Error& error);
  void accepted(const QString& message);

  Session session_;
  CanEdit can_edit_;
  Report report_;
  Commit commit_;
};

} // namespace oscillate::quick
