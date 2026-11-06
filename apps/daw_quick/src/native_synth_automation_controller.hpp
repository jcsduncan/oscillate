#pragma once

#include "oscillate/application/project_session.hpp"
#include "oscillate/application/revision_status.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <functional>
#include <optional>

namespace oscillate::quick {

// UI-thread editor for one selected built-in instrument's parameters, or a
// built-in effect slot's on a track (ADR-0043). The session remains
// authoritative; this object owns no duplicate automation state.
class NativeSynthAutomationController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList parameters READ parameters CONSTANT)
  Q_PROPERTY(QVariantMap lane READ lane NOTIFY changed)
  Q_PROPERTY(QString message READ message NOTIFY changed)
  Q_PROPERTY(QString playbackStatus READ playback_status NOTIFY playbackStatusChanged)

public:
  using Changed = std::function<void(std::uint64_t)>;
  using AudibleRevision = std::function<std::optional<application::RevisionStatus>()>;
  using SaveHook = std::function<void()>;
  NativeSynthAutomationController(application::ProjectSession& session, project::TrackId track,
                                  const native_dsp::synth::SynthEngine& engine, Changed changed,
                                  AudibleRevision audible_revision, QObject* parent = nullptr,
                                  SaveHook request_save = {});
  // A built-in effect slot on `track`'s chain. `effect` must outlive this.
  NativeSynthAutomationController(application::ProjectSession& session, project::TrackId track,
                                  project::InsertSlotId slot,
                                  const native_dsp::effects::EffectEngine& effect, Changed changed,
                                  AudibleRevision audible_revision, QObject* parent = nullptr,
                                  SaveHook request_save = {});

  [[nodiscard]] QVariantList parameters() const;
  [[nodiscard]] QVariantMap lane() const;
  [[nodiscard]] QString selected_parameter() const { return selected_parameter_; }
  [[nodiscard]] QString message() const { return message_; }
  [[nodiscard]] QString playback_status() const;
  Q_INVOKABLE void select_parameter(const QString& id);
  Q_INVOKABLE bool set_point(int zero_based_bar, double normalized_value);
  Q_INVOKABLE bool set_value(const QString& point_id, double normalized_value);
  Q_INVOKABLE bool remove_point(const QString& point_id);
  Q_INVOKABLE bool remove_lane();
  Q_INVOKABLE bool toggle_timebase();
  Q_INVOKABLE void refresh() {
    emit changed();
    refresh_playback_status();
  }
  void refresh_playback_status();

signals:
  void changed();
  void playbackStatusChanged();

private:
  [[nodiscard]] const native_dsp::synth::ParameterDescriptor* selected_descriptor() const;
  [[nodiscard]] const project::AutomationLane* selected_lane() const;
  [[nodiscard]] bool commit(const foundation::Result<void>& result);
  [[nodiscard]] bool targets(const project::AutomationTarget& target, std::string_view id) const;
  [[nodiscard]] foundation::Result<std::vector<double>> saved_values() const;
  void select_first();

  application::ProjectSession& session_;
  project::TrackId track_;
  std::optional<project::InsertSlotId> slot_;
  const native_dsp::synth::EngineDescriptor& descriptor_;
  Changed after_edit_;
  AudibleRevision audible_revision_;
  SaveHook request_save_;
  QString selected_parameter_;
  QString message_;
  QString last_playback_status_;
};

} // namespace oscillate::quick
