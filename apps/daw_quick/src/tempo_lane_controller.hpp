#pragma once

#include "oscillate/application/project_session.hpp"

#include <QObject>
#include <QString>
#include <QVariantList>

#include <functional>

namespace oscillate::quick {

// The project tempo lane under the arrangement's ruler (ADR-0112), and the
// song's opening tempo and meter. Positions are zero-based fractional bars,
// as the arrangement draws them; a point's `tick` is its exact position as
// text. The lane draws the tempo as it plays, through the shared tempo map;
// every edit is one undo step and plays live.
class TempoLaneController final : public QObject {
  Q_OBJECT
  // [{tick, bar, bpm, shape, initial, last}], in time order. `shape` is the
  // way to the next point: hold, straight, s_curve, ease_in or ease_out.
  Q_PROPERTY(QVariantList points READ points NOTIFY changed)
  // The lane's scale: whole BPM a little beyond the slowest and fastest.
  Q_PROPERTY(double lowBpm READ low_bpm NOTIFY changed)
  Q_PROPERTY(double highBpm READ high_bpm NOTIFY changed)
  // [{key, name}] in menu order.
  Q_PROPERTY(QVariantList shapes READ shapes CONSTANT)

public:
  using Session = std::function<application::ProjectSession*()>;
  using CanEdit = std::function<bool()>;
  using Report = std::function<void(QString)>;
  using Commit = std::function<void()>;

  TempoLaneController(Session session, CanEdit can_edit, Report report, Commit commit,
                      QObject* parent = nullptr);

  [[nodiscard]] QVariantList points() const { return points_; }
  [[nodiscard]] double low_bpm() const noexcept { return low_bpm_; }
  [[nodiscard]] double high_bpm() const noexcept { return high_bpm_; }
  [[nodiscard]] QVariantList shapes() const;
  // Rebuilds the projection from the session's snapshot.
  void refresh();

  // The opening tempo and meter (the project controls).
  void set_initial_bpm(double bpm);
  void set_initial_meter(int numerator, int denominator);

  // The tempo as it plays at `bar`, and across [from_bar, to_bar] at
  // `samples` evenly spaced bars: what the lane draws.
  Q_INVOKABLE double bpm_at(double bar) const;
  Q_INVOKABLE QVariantList curve(double from_bar, double to_bar, int samples) const;
  // A point at `bar`; it continues the way the tempo was going there.
  Q_INVOKABLE void add_point(double bar, double bpm);
  // The point at `tick` moved to `bar` with `bpm` (the first point keeps
  // its place at the start of the song).
  Q_INVOKABLE void move_point(const QString& tick, double bar, double bpm);
  Q_INVOKABLE void remove_point(const QString& tick);
  // The way from the point at `tick` to the next.
  Q_INVOKABLE void set_shape(const QString& tick, const QString& shape);
  // A drawn ramp from `from_bar` to `end_bpm` at `to_bar`.
  Q_INVOKABLE void ramp(double from_bar, double to_bar, double end_bpm, const QString& shape);
  // "Ramp to this track's tempo" across the clip's transition.
  Q_INVOKABLE void ramp_to_clip(const QString& clip_id);

signals:
  void changed();

private:
  [[nodiscard]] application::ProjectSession* editable_session();
  void finish(const foundation::Result<void>& applied, std::uint64_t before, const QString& done);

  Session session_;
  CanEdit can_edit_;
  Report report_;
  Commit commit_;
  QVariantList points_;
  double low_bpm_ = 0.0;
  double high_bpm_ = 0.0;
};

} // namespace oscillate::quick
