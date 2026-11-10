#pragma once

#include "background_job.hpp"
#include "meter_model.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/project/commands.hpp"
#include "oscillate/project/mix_roles.hpp"
#include "status_text.hpp"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace oscillate::application {
class ProjectSession;
} // namespace oscillate::application

// The master Rebalance panel (ADR-0123) for QML: one row per role (gain and
// a three-band EQ with its response curve, and a meter over the tracks the
// role's stage acts on), every track's role with why it has it and a
// chosen-role override, and Listen, which hears the audio tracks on a worker
// and saves what it heard as one undo step.
namespace oscillate::quick {

class RebalanceController;

// One role's settings as the device kit's controller interface (parameter,
// normalised, display, gestures, step), so a Kit.Fader or Kit.Knob drives it
// directly. Parameters: gain, low, lowHz, mid, midHz, midQ, high, highHz.
// Every change in one drag shares a gesture, so a drag is one undo step.
class RebalanceRole final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString id READ id CONSTANT)
  Q_PROPERTY(QString name READ name CONSTANT)
  Q_PROPERTY(QString meterId READ meter_id CONSTANT)
  Q_PROPERTY(int revision READ revision NOTIFY changed)
  Q_PROPERTY(bool editable READ editable NOTIFY changed)
  Q_PROPERTY(bool neutral READ neutral NOTIFY changed)
  // The names of the tracks playing this role, in track order.
  Q_PROPERTY(QStringList trackNames READ track_names NOTIFY changed)
  // The EQ's response in dB at RebalanceController::curve_points
  // frequencies, log-spaced from curve_from_hz to curve_to_hz.
  Q_PROPERTY(QVariantList curve READ curve NOTIFY changed)

public:
  RebalanceRole(RebalanceController& owner, project::MixRole role, QObject* parent = nullptr);

  [[nodiscard]] project::MixRole role() const noexcept { return role_; }
  [[nodiscard]] QString id() const;
  [[nodiscard]] QString name() const;
  [[nodiscard]] QString meter_id() const;
  [[nodiscard]] int revision() const noexcept { return revision_; }
  [[nodiscard]] bool editable() const;
  [[nodiscard]] bool neutral() const;
  [[nodiscard]] QStringList track_names() const { return track_names_; }
  [[nodiscard]] QVariantList curve() const { return curve_; }

  // {name, group, minimum, maximum, default, unit, bipolar}; empty for an
  // unknown id.
  Q_INVOKABLE QVariantMap parameter(const QString& id) const;
  Q_INVOKABLE double value(const QString& id) const;
  Q_INVOKABLE double normalised(const QString& id) const;
  Q_INVOKABLE QString display(const QString& id) const;
  Q_INVOKABLE void begin_gesture(const QString& id);
  Q_INVOKABLE void set_normalised(const QString& id, double position);
  Q_INVOKABLE void set_value(const QString& id, double value);
  Q_INVOKABLE void end_gesture(const QString& id);
  Q_INVOKABLE void cancel_gesture(const QString& id);
  Q_INVOKABLE void reset_to_default(const QString& id);
  Q_INVOKABLE void step(const QString& id, int direction, bool fine);
  // Every setting back to neutral, one undo step.
  Q_INVOKABLE void reset();

  // The controller saw a new snapshot.
  void update(QStringList track_names, QVariantList curve);

signals:
  void changed();

private:
  RebalanceController& owner_;
  project::MixRole role_;
  int revision_ = 0;
  QStringList track_names_;
  QVariantList curve_;
  std::optional<std::uint64_t> gesture_;
};

class RebalanceController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool enabled READ enabled WRITE set_enabled NOTIFY changed)
  Q_PROPERTY(QList<QObject*> roles READ roles CONSTANT)
  // Every track, in the track list's order: {id, name, color, order, kind,
  // nested, role, roleName, reason, chosen}. role is a role id; reason says
  // why ("Stick", "name", "heard").
  Q_PROPERTY(QVariantList tracks READ tracks NOTIFY tracksChanged)
  // The override menu's choices: {id, name}, "auto" first.
  Q_PROPERTY(QVariantList choices READ choices CONSTANT)
  Q_PROPERTY(bool listening READ listening NOTIFY changed)
  Q_PROPERTY(QString status READ status NOTIFY changed)
  Q_PROPERTY(double curveLowHz READ curve_low_hz CONSTANT)
  Q_PROPERTY(double curveHighHz READ curve_high_hz CONSTANT)
  Q_PROPERTY(double rangeDb READ range_db CONSTANT)

public:
  // A role meter's channel id in the MeterModel: this prefix, then the
  // role's id ("role:drums").
  static constexpr std::string_view meter_prefix = "role:";
  // The response curve's points and span.
  static constexpr int curve_points = 96;
  static constexpr double curve_from_hz = 20.0;
  static constexpr double curve_to_hz = 20'000.0;
  // Listen's progress is polled this often.
  static constexpr int poll_ms = 50;

  struct Hooks {
    std::function<application::ProjectSession*()> session;
    std::function<bool()> can_edit;
    std::function<void()> commit;
  };
  using TrackMeter = std::function<std::optional<MeterReading>(project::TrackId)>;

  explicit RebalanceController(Hooks hooks, QObject* parent = nullptr);
  RebalanceController(const RebalanceController&) = delete;
  RebalanceController& operator=(const RebalanceController&) = delete;
  ~RebalanceController() override;

  [[nodiscard]] bool enabled() const;
  void set_enabled(bool on);
  [[nodiscard]] QList<QObject*> roles() const;
  [[nodiscard]] QVariantList tracks() const { return tracks_; }
  [[nodiscard]] QVariantList choices() const;
  [[nodiscard]] bool listening() const noexcept { return job_.running(); }
  [[nodiscard]] QString status() const { return status_.text(); }
  [[nodiscard]] double curve_low_hz() const noexcept { return curve_from_hz; }
  [[nodiscard]] double curve_high_hz() const noexcept { return curve_to_hz; }
  [[nodiscard]] double range_db() const noexcept;

  // A track's chosen role: a role id, or "auto" to let it be inferred.
  Q_INVOKABLE void setTrackRole(const QString& track_id, const QString& role);
  // Hears every audio track on a worker; what it hears is saved as one
  // undo step. Cancel drops the result.
  Q_INVOKABLE void listen();
  Q_INVOKABLE void cancelListening();

  // The meter of a role channel id (meter_prefix + role id): the readings
  // of the tracks the role's stage acts on, combined. None for another id,
  // or when no such track reads.
  [[nodiscard]] std::optional<MeterReading> role_meter(const QString& id,
                                                       const TrackMeter& track) const;

  // The project's revision moved or another project opened.
  void project_changed();
  // Tests: waits for Listen to finish and installs its result.
  bool wait_for_listening();

  // RebalanceRole's edits: the whole setting with role's balance replaced
  // (and the rebalance switched on), coalesced under the gesture.
  void edit_role(project::MixRole role, const project::RoleBalance& balance,
                 std::optional<std::uint64_t> gesture);
  [[nodiscard]] project::RoleBalance balance(project::MixRole role) const;
  [[nodiscard]] bool can_edit_now() const;
  [[nodiscard]] std::uint64_t next_gesture() noexcept;

signals:
  void changed();
  void tracksChanged();

private:
  using Heard = foundation::Result<std::vector<project::Operation>>;
  void poll();
  void finish();
  void set_status(QString text);
  void set_tracks(QVariantList rows);
  void edit(const std::function<foundation::Result<void>(application::ProjectSession&)>& apply,
            const QString& what);

  Hooks hooks_;
  std::array<RebalanceRole*, project::mix_role_count> roles_{};
  QVariantList tracks_;
  // The tracks each role's stage acts on.
  std::array<std::vector<project::TrackId>, project::mix_role_count> stage_tracks_;
  StatusText status_;
  QTimer timer_;
  BackgroundJob<Heard> job_;
  std::shared_ptr<std::atomic<bool>> cancel_;
  // What Listen heard from: its tracks, to drop a result for a track whose
  // clips changed meanwhile.
  std::shared_ptr<const project::Snapshot> listened_;
  std::uint64_t listened_session_ = 0;
  // Panel gestures use their own range, so they never merge with others.
  std::uint64_t next_gesture_ = 0x5242'0000'0000'0001ULL;
};

// Combines tracks' meter readings into one: the highest peak, the RMS of
// their summed power, clipped if any clipped. None when there are none.
[[nodiscard]] std::optional<MeterReading>
combine_meters(const std::vector<MeterReading>& readings) noexcept;

} // namespace oscillate::quick
