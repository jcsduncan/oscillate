#pragma once

#include "oscillate/application/project_session.hpp"
#include "oscillate/plugins/vst3_graph_node.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace oscillate::application {

enum class Vst3ManualEditPhase : std::uint8_t {
  idle,
  pending,
  applied,
  failed,
  saved_not_live,
  model_not_saved
};

struct Vst3ManualEditStatus {
  std::uint64_t model_revision{0};
  // Accepted model revision. The Quick editor may publish CURRENT later.
  std::uint64_t saved_revision{0};
  bool persistence_pending{false};
  std::uint64_t live_revision{0};
  std::int64_t applied_sample{-1};
  Vst3ManualEditPhase phase{Vst3ManualEditPhase::idle};
  plugins::Vst3ManualEventStatus admission{plugins::Vst3ManualEventStatus::unavailable};
  // Why a saved revision is not live when the playback owner refused it.
  std::optional<foundation::Error> live_refusal{};
};

// The routed playback owner that must adopt a manual model revision. preflight
// runs before the project edit. begin runs after model acceptance and before
// the event is submitted, so no event can be audible without a matching
// pending adoption. Disk publication may complete later on a worker; the UI
// reports that separately. settle reports the terminal receipt for every begun
// revision exactly once, including a submission refused after begin.
class Vst3ManualLiveAdoption {
public:
  virtual ~Vst3ManualLiveAdoption() = default;
  // Checked before the save; a refusal leaves the project unchanged.
  [[nodiscard]] virtual foundation::Result<void>
  preflight(project::PluginInstanceId plugin, plugins::Vst3ActiveGraphIdentity expected) = 0;
  [[nodiscard]] virtual foundation::Result<void>
  begin(const project::Snapshot& saved, project::PluginInstanceId plugin,
        plugins::Vst3ActiveGraphIdentity expected) = 0;
  [[nodiscard]] virtual foundation::Result<void>
  settle(plugins::Vst3ManualEventReceipt receipt) = 0;
};
using Vst3ManualEffectPhase = Vst3ManualEditPhase;
using Vst3ManualEffectStatus = Vst3ManualEditStatus;

// Control-thread owner for one live effect gesture. The caller captures opaque
// state and references on an independent, suspended clone before calling commit.
// This object and the slot have one serialized producer. No callback method is
// called here; the graph owner activates/retires the supplied slot. With an
// adoption owner, keep this object until a pending receipt is observed so the
// owner is settled. The adoption owner must outlive this object.
class Vst3ManualEffectEdit final {
public:
  Vst3ManualEffectEdit(ProjectSession& session, std::shared_ptr<plugins::Vst3ManualEventSlot> slot,
                       Vst3ManualLiveAdoption* adoption = nullptr) noexcept;

  [[nodiscard]] foundation::Result<Vst3ManualEffectStatus>
  commit(std::uint64_t opened_revision, project::TrackId track, project::InsertSlotId insert,
         const project::Vst3PluginInstance& original,
         std::vector<std::uint8_t> captured_component_state,
         std::vector<project::Vst3ParameterReference> captured_references,
         plugins::Vst3ParameterEvent parameter, plugins::Vst3ActiveGraphIdentity expected_graph,
         std::uint64_t gesture_sequence = 0,
         EditorSavePolicy save_policy = EditorSavePolicy::immediate);

  [[nodiscard]] Vst3ManualEffectStatus observe();

private:
  ProjectSession& session_;
  std::shared_ptr<plugins::Vst3ManualEventSlot> slot_;
  Vst3ManualLiveAdoption* adoption_;
  Vst3ManualEffectStatus status_;
};

// The equivalent control-thread transaction for a track's VST3 instrument
// source. The caller supplies the slot for the exact saved source and captures
// state from an independent, suspended clone before committing.
class Vst3ManualInstrumentEdit final {
public:
  Vst3ManualInstrumentEdit(ProjectSession& session,
                           std::shared_ptr<plugins::Vst3ManualEventSlot> slot,
                           Vst3ManualLiveAdoption* adoption = nullptr) noexcept;

  [[nodiscard]] foundation::Result<Vst3ManualEditStatus>
  commit(std::uint64_t opened_revision, project::TrackId track,
         const project::Vst3PluginInstance& original,
         std::vector<std::uint8_t> captured_component_state,
         std::vector<project::Vst3ParameterReference> captured_references,
         plugins::Vst3ParameterEvent parameter, plugins::Vst3ActiveGraphIdentity expected_graph,
         std::uint64_t gesture_sequence = 0,
         EditorSavePolicy save_policy = EditorSavePolicy::immediate);

  [[nodiscard]] Vst3ManualEditStatus observe();

private:
  ProjectSession& session_;
  std::shared_ptr<plugins::Vst3ManualEventSlot> slot_;
  Vst3ManualLiveAdoption* adoption_;
  Vst3ManualEditStatus status_;
};

} // namespace oscillate::application
