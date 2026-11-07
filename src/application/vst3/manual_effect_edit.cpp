#include "oscillate/application/vst3/manual_effect_edit.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <utility>

namespace oscillate::application {
namespace {
foundation::Error conflict(const char* message) {
  return {foundation::ErrorCode::conflict, message};
}
const char* refusal(plugins::Vst3ManualEventStatus status) noexcept {
  switch (status) {
  case plugins::Vst3ManualEventStatus::busy:
    return "VST3 manual event slot is busy";
  case plugins::Vst3ManualEventStatus::stale_instance:
    return "VST3 active graph changed";
  case plugins::Vst3ManualEventStatus::invalid_revision:
    return "VST3 project revision is invalid";
  case plugins::Vst3ManualEventStatus::invalid_value:
    return "VST3 normalized value is invalid";
  case plugins::Vst3ManualEventStatus::automated:
    return "VST3 parameter is automated";
  case plugins::Vst3ManualEventStatus::unavailable:
    return "VST3 parameter is unavailable";
  case plugins::Vst3ManualEventStatus::latency_unverified:
    return "VST3 parameter latency is unverified for live editing";
  case plugins::Vst3ManualEventStatus::accepted:
    return "VST3 parameter accepted";
  }
  return "VST3 manual event unavailable";
}

template <typename ValidOwner, typename Persist>
foundation::Result<Vst3ManualEditStatus> commit_manual(
    ProjectSession& session, const std::shared_ptr<plugins::Vst3ManualEventSlot>& slot,
    Vst3ManualLiveAdoption* adoption, Vst3ManualEditStatus& status, std::uint64_t opened_revision,
    project::TrackId track, const project::Vst3PluginInstance& original,
    std::vector<std::uint8_t> captured_component_state,
    std::vector<project::Vst3ParameterReference> captured_references,
    plugins::Vst3ParameterEvent parameter, plugins::Vst3ActiveGraphIdentity expected_graph,
    std::uint64_t gesture_sequence, EditorSavePolicy save_policy, ValidOwner valid_owner,
    Persist persist, const char* identity_error) {
  if (status.phase == Vst3ManualEditPhase::pending)
    return foundation::Result<Vst3ManualEditStatus>::failure(
        conflict("VST3 edit receipt is pending"));
  const auto& snapshot = session.snapshot();
  if (snapshot.revision != opened_revision ||
      opened_revision == std::numeric_limits<std::uint64_t>::max())
    return foundation::Result<Vst3ManualEditStatus>::failure(
        conflict("VST3 editor revision changed"));
  const auto owner = std::ranges::find(snapshot.tracks, track, &project::Track::id);
  if (owner == snapshot.tracks.end() || !valid_owner(*owner))
    return foundation::Result<Vst3ManualEditStatus>::failure(conflict(identity_error));
  const project::AutomationTarget target = project::Vst3ParameterTarget{original.id, parameter.id};
  const auto has_lane = [&target](const auto& lanes) {
    return std::ranges::any_of(
        lanes, [&target](const project::AutomationLane& lane) { return lane.target == target; });
  };
  if (has_lane(owner->automation) ||
      std::ranges::any_of(owner->clips,
                          [&](const project::Clip& clip) { return has_lane(clip.automation); }) ||
      std::ranges::any_of(owner->midi_clips,
                          [&](const project::MidiClip& clip) { return has_lane(clip.automation); }))
    return foundation::Result<Vst3ManualEditStatus>::failure(
        conflict("VST3 parameter has an automation lane"));
  const auto reference =
      std::ranges::find(captured_references, parameter.id, &project::Vst3ParameterReference::id);
  if (reference == captured_references.end() ||
      reference->normalized_value != parameter.normalized_value ||
      (captured_component_state == original.component_state &&
       captured_references == original.parameters))
    return foundation::Result<Vst3ManualEditStatus>::failure(
        conflict("VST3 clone capture does not contain the parameter edit"));
  if (!slot)
    return foundation::Result<Vst3ManualEditStatus>::failure(
        conflict("VST3 live slot unavailable"));
  const auto next_revision = opened_revision + 1;
  const auto admission = slot->admission(expected_graph, next_revision, parameter);
  if (admission != plugins::Vst3ManualEventStatus::accepted)
    return foundation::Result<Vst3ManualEditStatus>::failure(
        {foundation::ErrorCode::unsupported, refusal(admission)});
  if (adoption) {
    auto ready = adoption->preflight(original.id, expected_graph);
    if (!ready)
      return foundation::Result<Vst3ManualEditStatus>::failure(ready.error());
  }

  auto committed = persist(std::move(captured_component_state), std::move(captured_references));
  if (!committed) {
    if (session.snapshot().revision != opened_revision)
      status = {.model_revision = session.snapshot().revision,
                .phase = Vst3ManualEditPhase::model_not_saved,
                .admission = plugins::Vst3ManualEventStatus::unavailable};
    return foundation::Result<Vst3ManualEditStatus>::failure(committed.error());
  }
  status = {.model_revision = session.snapshot().revision,
            .saved_revision = session.snapshot().revision,
            .persistence_pending = save_policy == EditorSavePolicy::background,
            .live_revision = 0,
            .applied_sample = -1,
            .phase = Vst3ManualEditPhase::saved_not_live,
            .admission = plugins::Vst3ManualEventStatus::unavailable};
  if (status.saved_revision != next_revision)
    return foundation::Result<Vst3ManualEditStatus>::success(status);
  if (adoption) {
    auto begun = adoption->begin(session.snapshot(), original.id, expected_graph);
    if (!begun) {
      status.live_refusal = begun.error();
      return foundation::Result<Vst3ManualEditStatus>::success(status);
    }
  }
  const auto result = slot->submit(expected_graph, next_revision, parameter, gesture_sequence);
  status.admission = result;
  if (result == plugins::Vst3ManualEventStatus::accepted) {
    status.phase = Vst3ManualEditPhase::pending;
    return foundation::Result<Vst3ManualEditStatus>::success(status);
  }
  if (result == plugins::Vst3ManualEventStatus::stale_instance)
    (void)slot->collect();
  if (adoption) {
    auto settled = adoption->settle({next_revision, plugins::Vst3ManualEventPhase::failed, -1});
    if (!settled)
      status.live_refusal = settled.error();
  }
  return foundation::Result<Vst3ManualEditStatus>::success(status);
}

Vst3ManualEditStatus observe_manual(Vst3ManualEditStatus& status,
                                    const std::shared_ptr<plugins::Vst3ManualEventSlot>& slot,
                                    Vst3ManualLiveAdoption* adoption) {
  if (status.phase != Vst3ManualEditPhase::pending || !slot)
    return status;
  const auto receipt = slot->collect();
  if (!receipt || receipt->revision != status.saved_revision)
    return status;
  if (adoption) {
    auto settled = adoption->settle(*receipt);
    if (!settled && receipt->phase == plugins::Vst3ManualEventPhase::applied) {
      status.phase = Vst3ManualEditPhase::failed;
      status.live_refusal = settled.error();
      return status;
    }
    if (!settled)
      status.live_refusal = settled.error();
  }
  if (receipt->phase == plugins::Vst3ManualEventPhase::applied) {
    status.phase = Vst3ManualEditPhase::applied;
    status.live_revision = receipt->revision;
    status.applied_sample = receipt->project_sample;
  } else {
    status.phase = Vst3ManualEditPhase::failed;
  }
  return status;
}
} // namespace

Vst3ManualEffectEdit::Vst3ManualEffectEdit(ProjectSession& session,
                                           std::shared_ptr<plugins::Vst3ManualEventSlot> slot,
                                           Vst3ManualLiveAdoption* adoption) noexcept
    : session_(session), slot_(std::move(slot)), adoption_(adoption) {}

foundation::Result<Vst3ManualEffectStatus> Vst3ManualEffectEdit::commit(
    std::uint64_t opened_revision, project::TrackId track, project::InsertSlotId insert,
    const project::Vst3PluginInstance& original, std::vector<std::uint8_t> captured_component_state,
    std::vector<project::Vst3ParameterReference> captured_references,
    plugins::Vst3ParameterEvent parameter, plugins::Vst3ActiveGraphIdentity expected_graph,
    std::uint64_t gesture_sequence, EditorSavePolicy save_policy) {
  return commit_manual(
      session_, slot_, adoption_, status_, opened_revision, track, original,
      std::move(captured_component_state), std::move(captured_references), parameter,
      expected_graph, gesture_sequence, save_policy,
      [&](const project::Track& owner) {
        const auto found = std::ranges::find(owner.effects, insert, &project::EffectInsert::id);
        const auto* saved = found == owner.effects.end()
                                ? nullptr
                                : std::get_if<project::Vst3PluginInstance>(&found->processor);
        return saved && found->enabled && !saved->missing && *saved == original;
      },
      [&](std::vector<std::uint8_t> state,
          std::vector<project::Vst3ParameterReference> references) {
        return session_.effects().set_vst3_editor_state(opened_revision, track, insert, original,
                                                        std::move(state), std::move(references),
                                                        save_policy);
      },
      "VST3 effect identity or state changed");
}

Vst3ManualEffectStatus Vst3ManualEffectEdit::observe() {
  return observe_manual(status_, slot_, adoption_);
}

Vst3ManualInstrumentEdit::Vst3ManualInstrumentEdit(
    ProjectSession& session, std::shared_ptr<plugins::Vst3ManualEventSlot> slot,
    Vst3ManualLiveAdoption* adoption) noexcept
    : session_(session), slot_(std::move(slot)), adoption_(adoption) {}

foundation::Result<Vst3ManualEditStatus> Vst3ManualInstrumentEdit::commit(
    std::uint64_t opened_revision, project::TrackId track,
    const project::Vst3PluginInstance& original, std::vector<std::uint8_t> captured_component_state,
    std::vector<project::Vst3ParameterReference> captured_references,
    plugins::Vst3ParameterEvent parameter, plugins::Vst3ActiveGraphIdentity expected_graph,
    std::uint64_t gesture_sequence, EditorSavePolicy save_policy) {
  return commit_manual(
      session_, slot_, adoption_, status_, opened_revision, track, original,
      std::move(captured_component_state), std::move(captured_references), parameter,
      expected_graph, gesture_sequence, save_policy,
      [&](const project::Track& owner) {
        const auto* saved = std::get_if<project::Vst3PluginInstance>(&owner.instrument_source);
        return owner.kind == project::TrackKind::instrument && saved && !saved->missing &&
               *saved == original;
      },
      [&](std::vector<std::uint8_t> state,
          std::vector<project::Vst3ParameterReference> references) {
        return session_.instruments().set_vst3_instrument_state(
            opened_revision, track, original, std::move(state), std::move(references), save_policy);
      },
      "VST3 instrument identity or state changed");
}

Vst3ManualEditStatus Vst3ManualInstrumentEdit::observe() {
  return observe_manual(status_, slot_, adoption_);
}

} // namespace oscillate::application
