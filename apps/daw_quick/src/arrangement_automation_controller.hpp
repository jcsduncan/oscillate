#pragma once

#include "oscillate/application/project_session.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <functional>
#include <optional>

namespace oscillate::quick {

// The arrangement's automation lanes (ADR-0051): every track's lanes and the
// parameters it can automate, for drawing and editing on the timeline.
// Positions are zero-based fractional bars, as the arrangement draws them;
// values are normalised 0..1 along the parameter's own feel (decibels for
// level and sends, the engine's scale for a built-in parameter), and every
// point carries its display text. The session stays authoritative.
class ArrangementAutomationController final : public QObject {
  Q_OBJECT
  // Per track, in track order: {trackId, lanes: [{laneId, key, name, group,
  // discrete, points: [{id, bar, value, text, shape}]}], targets: [{group,
  // items: [{key, name}]}]}.
  Q_PROPERTY(QVariantList tracks READ tracks NOTIFY changed)

public:
  using Session = std::function<application::ProjectSession*()>;
  using CanEdit = std::function<bool()>;
  using Report = std::function<void(QString)>;
  using Commit = std::function<void()>;

  ArrangementAutomationController(Session session, CanEdit can_edit, Report report, Commit commit,
                                  QObject* parent = nullptr);

  [[nodiscard]] QVariantList tracks() const { return tracks_; }
  // Rebuilds the projection from the session's snapshot.
  void refresh();

  // Each edit is one undo step. A point outside the bar range or a value
  // outside 0..1 is refused with a status message.
  Q_INVOKABLE QString add_point(const QString& track_id, const QString& key, double bar,
                                double value);
  // moves: [{id, bar, value}]
  Q_INVOKABLE void move_points(const QString& lane_id, const QVariantList& moves);
  Q_INVOKABLE void remove_points(const QString& lane_id, const QStringList& point_ids);
  // points: [{bar, value}], ascending, inside [from_bar, to_bar].
  Q_INVOKABLE void draw(const QString& track_id, const QString& key, double from_bar, double to_bar,
                        const QVariantList& points);
  // shape: hold, linear, quadratic, exponential, ease_in_out.
  Q_INVOKABLE void set_curve(const QString& lane_id, const QString& point_id, const QString& shape);
  // Bows the segment after `point_id` (Ctrl-drag on the lane): `bend` in
  // -2/3..2/3 lifts the segment's start (positive) or holds it back
  // (negative) as a cubic Bezier; near zero it is a straight line again.
  Q_INVOKABLE void bend(const QString& lane_id, const QString& point_id, double bend);
  // The display text for a normalised value of `key` on `track_id`.
  Q_INVOKABLE QString value_text(const QString& track_id, const QString& key, double value) const;
  // The normalised value `key` has now (its default, before any point).
  Q_INVOKABLE double current_value(const QString& track_id, const QString& key) const;
  // The lane's curve as it plays, sampled at `samples` evenly spaced bars
  // across [from_bar, to_bar], normalised like its points: what the
  // arrangement draws between them.
  Q_INVOKABLE QVariantList curve(const QString& lane_id, double from_bar, double to_bar,
                                 int samples) const;

signals:
  void changed();

private:
  [[nodiscard]] application::ProjectSession* editable_session();
  void failure(const foundation::Error& error);
  void accepted(const QString& action);

  Session session_;
  CanEdit can_edit_;
  Report report_;
  Commit commit_;
  QVariantList tracks_;
};

} // namespace oscillate::quick
