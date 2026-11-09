#pragma once

#include "oscillate/application/project_session.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"

#include <QObject>
#include <QString>

#include <optional>
#include <string_view>

namespace oscillate::quick {

// Where an open built-in instrument's, or a built-in effect slot's, automation
// lives on the arrangement (ADR-0051): its track and the lane key the
// arrangement shows for it, the device's first automated parameter, else its
// first automatable one. The device panel's Automate action opens that lane.
class NativeSynthAutomationController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString trackId READ track_id CONSTANT)
  Q_PROPERTY(QString laneKey READ lane_key NOTIFY changed)

public:
  NativeSynthAutomationController(const application::ProjectSession& session,
                                  project::TrackId track,
                                  const native_dsp::synth::SynthEngine& engine,
                                  QObject* parent = nullptr);
  // A built-in effect slot on `track`'s chain. `effect` must outlive this.
  NativeSynthAutomationController(const application::ProjectSession& session,
                                  project::TrackId track, project::InsertSlotId slot,
                                  const native_dsp::effects::EffectEngine& effect,
                                  QObject* parent = nullptr);

  [[nodiscard]] QString track_id() const;
  [[nodiscard]] QString lane_key() const;
  // The project changed: the lane key may now name an automated parameter.
  void refresh() { emit changed(); }

signals:
  void changed();

private:
  [[nodiscard]] QString key_of(std::string_view parameter) const;

  const application::ProjectSession& session_;
  project::TrackId track_;
  std::optional<project::InsertSlotId> slot_;
  const native_dsp::EngineDescriptor& descriptor_;
};

} // namespace oscillate::quick
