#pragma once

#include "oscillate/application/project_session.hpp"

#include <QObject>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace oscillate::quick {

// UI-thread owner for track lifecycle and channel controls. ProjectBridge
// retains project lifecycle, projections and graph revision publication.
class TrackCommands final : public QObject {
  Q_OBJECT

public:
  using Session = std::function<application::ProjectSession*()>;
  using CanEdit = std::function<bool()>;
  using Report = std::function<void(QString)>;
  using Commit = std::function<void()>;
  using Created = std::function<void(int)>;

  TrackCommands(Session session, CanEdit can_create, CanEdit can_edit, Report report, Commit commit,
                Created created, QObject* parent = nullptr);

  Q_INVOKABLE void add_instrument_track();
  Q_INVOKABLE void add_synth_track();
  Q_INVOKABLE void add_bus_track(const QString& kind);
  // Sends the tracks to a group bus as one undo step: `bus_id`, or empty for
  // a new one just above them (RoutingService::route_tracks_to_bus).
  Q_INVOKABLE void route_to_bus(const QStringList& track_ids, const QString& bus_id);
  // Sends the tracks straight to the master again.
  Q_INVOKABLE void route_to_master(const QStringList& track_ids);
  // A track list drop as one undo step: the tracks route to the group bus
  // `bus_id` (empty for the master) and move to just before `before_id`, or
  // after the bus's last member (the end of the list for the master) when
  // it is empty (RoutingService::place_tracks).
  Q_INVOKABLE void place_tracks(const QStringList& track_ids, const QString& bus_id,
                                const QString& before_id);
  Q_INVOKABLE void add_audio_track();
  Q_INVOKABLE void remove_track(const QString& track_id);
  // Returns the copy's ID, or empty when nothing was duplicated.
  Q_INVOKABLE QString duplicate_track(const QString& track_id);
  Q_INVOKABLE bool move_track(const QString& track_id, int to_index);
  Q_INVOKABLE void set_track_muted(const QString& track_id, bool muted);
  Q_INVOKABLE void set_track_soloed(const QString& track_id, bool soloed);
  // Kick & Bass Lock (ADR-0072): the Nova bass `track_id` and every unmuted
  // Anvil track lock (or unlock) together, as one undo step.
  Q_INVOKABLE void lock_kick_and_bass(const QString& track_id, bool on);
  Q_INVOKABLE void set_track_level_pan(const QString& track_id, double level, double pan);
  Q_INVOKABLE void set_master_level(double level);
  Q_INVOKABLE qulonglong begin_channel_gesture();
  Q_INVOKABLE void set_track_level_pan_gesture(const QString& track_id, double level, double pan,
                                               qulonglong gesture);
  Q_INVOKABLE void set_master_level_gesture(double level, qulonglong gesture);

private:
  [[nodiscard]] application::ProjectSession* editable_session();
  void failure(const QString& context, const foundation::Error& error);
  void accepted(const QString& message);
  void set_track_level_pan_impl(const QString& track_id, double level, double pan,
                                std::optional<std::uint64_t> gesture);
  void set_master_level_impl(double level, std::optional<std::uint64_t> gesture);
  // The tracks named, or nullopt (reported) when one is not a track ID.
  [[nodiscard]] std::optional<std::vector<project::TrackId>> parse_tracks(const QStringList& ids);

  Session session_;
  CanEdit can_create_;
  CanEdit can_edit_;
  Report report_;
  Commit commit_;
  Created created_;
  qulonglong next_gesture_ = 1;
};

} // namespace oscillate::quick
