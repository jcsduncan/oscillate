#include "oscillate/application/routed_playback_control.hpp"

#include "edit_rules.hpp"

#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "oscillate/native_inserts/instrument_binding.hpp"
#include "oscillate/native_inserts/native_stage.hpp"
#include "oscillate/track_chain/track_chain.hpp"

#include <algorithm>
#include <functional>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

// Edits offered to running stages and players without a graph rebuild.
namespace oscillate::application {
namespace {
using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;
using routed_playback::player_edit_only;
using routed_playback::runs;
using routed_playback::same_timing;
using routed_playback::vst3_midi_edit_only;

[[nodiscard]] bool same_track_outside_effects(const project::Track& left,
                                              const project::Track& right) {
  return left.id == right.id && left.clips == right.clips && left.kind == right.kind &&
         left.instrument_source == right.instrument_source && left.midi_clips == right.midi_clips &&
         left.main_output == right.main_output && left.sends == right.sends &&
         left.automation == right.automation && left.muted == right.muted &&
         left.soloed == right.soloed && left.level == right.level && left.pan == right.pan &&
         left.note_effects == right.note_effects && left.macros == right.macros &&
         left.frozen == right.frozen;
}

// An instrument track with no instrument plays nothing, so an edit to its
// MIDI clips alone is heard nowhere (an effect keyed by its notes follows
// the published snapshot).
[[nodiscard]] bool silent_midi_edit_only(const project::Track& before,
                                         const project::Track& after) {
  const auto unautomated = [](const project::Track& track) {
    return std::ranges::all_of(track.midi_clips,
                               [](const auto& clip) { return clip.automation.empty(); });
  };
  if (before.id != after.id || before.kind != project::TrackKind::instrument ||
      !std::holds_alternative<std::monostate>(before.instrument_source) || !unautomated(before) ||
      !unautomated(after))
    return false;
  auto masked = before;
  masked.midi_clips = after.midi_clips;
  return masked == after;
}

// A built-in effect's values edit prepared for its running stage (ADR-0043):
// no rebuild, the kernel glides. Declines (nullopt) when the stage is not
// the one prepared for this slot, its media changed or the values would
// change its latency;
// fails when the stage cannot take the offer now. Nothing is offered until
// the prepared offer is committed.
struct PreparedStageOffer {
  native_inserts::NativeEffectStage* stage = nullptr;
  native_dsp::effects::NativeEffectRuntime::PreparedOffer offer;
};

[[nodiscard]] Result<std::optional<PreparedStageOffer>>
prepare_effect_values(engine::ProcessorRuntime* stage, const project::NativeEffectPatch& old_patch,
                      const project::NativeEffectPatch& next, std::uint64_t revision) {
  using R = Result<std::optional<PreparedStageOffer>>;
  auto* runtime = dynamic_cast<native_inserts::NativeEffectStage*>(stage);
  // New managed media need decoding into a new stage (ADR-0115).
  if (!runtime || next.engine != old_patch.engine ||
      next.engine_version != old_patch.engine_version || next.generation <= old_patch.generation ||
      next.document.media != old_patch.document.media)
    return R::success(std::nullopt);
  auto resolved = native_inserts::resolve_native_effect(next);
  if (!resolved)
    return R::failure(resolved.error());
  if (resolved.value().engine->descriptor != runtime->engine().descriptor)
    return R::success(std::nullopt);
  // `latency_frames` is fixed at creation; a latency change needs a rebuild.
  // The stage's own rate makes the comparison exact.
  if (resolved.value().engine->latency_frames(resolved.value().values, runtime->sample_rate()) !=
      runtime->latency_frames())
    return R::success(std::nullopt);
  auto prepared = runtime->prepare_offer(
      resolved.value().values, next.document != old_patch.document ? &next.document : nullptr,
      revision);
  if (!prepared)
    return R::failure(prepared.error());
  return R::success(PreparedStageOffer{runtime, std::move(prepared).value()});
}

[[nodiscard]] Result<void> commit_effect_values(PreparedStageOffer& prepared) {
  if (!prepared.stage->commit_offer(std::move(prepared.offer)))
    return Result<void>::failure(
        {ErrorCode::internal, "An effect stage refused an edit it had prepared"});
  return Result<void>::success();
}

// One stage's edit, prepared and committed at once.
[[nodiscard]] Result<bool> offer_effect_values(engine::ProcessorRuntime* stage,
                                               const project::NativeEffectPatch& old_patch,
                                               const project::NativeEffectPatch& next,
                                               std::uint64_t revision,
                                               native_inserts::NativeEffectStage*& offered_to) {
  auto prepared = prepare_effect_values(stage, old_patch, next, revision);
  if (!prepared)
    return Result<bool>::failure(prepared.error());
  if (!prepared.value())
    return Result<bool>::success(false);
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): engaged above.
  auto& offer = *prepared.value();
  if (auto committed = commit_effect_values(offer); !committed)
    return Result<bool>::failure(committed.error());
  offered_to = offer.stage;
  return Result<bool>::success(true);
}

} // namespace

Result<bool>
RoutedPlaybackControl::try_queue_live_native(std::shared_ptr<const project::Snapshot> snapshot) {
  // A newer value may replace an unacknowledged offer only on the same
  // processor instance. The active graph still owns that instance and all
  // downstream state; a second graph publication is neither needed nor safe.
  const bool superseding =
      pending_native_revision_ && pending_native_revision_->track &&
      pending_native_revision_->group.empty() &&
      std::holds_alternative<native_inserts::NativeEffectStage*>(pending_native_revision_->runtime);
  const auto* previous =
      superseding ? pending_native_revision_->snapshot.get() : published_snapshot_.get();
  if (!previous || !published_snapshot_ || publisher_.active_revision() == 0 ||
      publisher_.has_pending() || (pending_native_revision_ && !superseding) ||
      (!superseding && revisions_.snapshot().active_graph_revision != previous->revision))
    return Result<bool>::success(false);
  if (previous->id != snapshot->id || previous->sample_rate != snapshot->sample_rate ||
      previous->sources != snapshot->sources ||
      previous->master_effects != snapshot->master_effects ||
      previous->source_tempo_reviews != snapshot->source_tempo_reviews ||
      previous->source_key_reviews != snapshot->source_key_reviews ||
      previous->next_insert_slot_serial != snapshot->next_insert_slot_serial ||
      previous->master_level != snapshot->master_level ||
      previous->tracks.size() != snapshot->tracks.size() || !same_timing(*previous, *snapshot))
    return Result<bool>::success(false);
  if (published_snapshot_->tracks.size() != previous->tracks.size())
    return Result<bool>::success(false);

  const project::Track* changed = nullptr;
  const project::Track* old_track = nullptr;
  for (std::size_t index = 0; index < snapshot->tracks.size(); ++index) {
    const auto& old = previous->tracks[index];
    const auto& next = snapshot->tracks[index];
    if (!same_track_outside_effects(old, next))
      return Result<bool>::success(false);
    if (old.effects != next.effects) {
      if (changed)
        return Result<bool>::success(false);
      old_track = &old;
      changed = &next;
    }
  }
  if (!changed ||
      (changed->kind != project::TrackKind::audio &&
       changed->kind != project::TrackKind::instrument &&
       changed->kind != project::TrackKind::group_bus &&
       changed->kind != project::TrackKind::return_bus) ||
      old_track->effects.empty() || old_track->effects.size() != changed->effects.size())
    return Result<bool>::success(false);
  const auto changed_slot = std::ranges::mismatch(old_track->effects, changed->effects);
  if (changed_slot.in1 == old_track->effects.end() ||
      !std::ranges::equal(std::next(changed_slot.in1), old_track->effects.end(),
                          std::next(changed_slot.in2), changed->effects.end()) ||
      changed_slot.in1->id != changed_slot.in2->id || !changed_slot.in1->enabled ||
      !changed_slot.in2->enabled)
    return Result<bool>::success(false);
  // A value offer never touches the detector wiring, so a built-in effect on
  // a sidechained chain still edits live.
  auto* running = active_stage(changed->id, changed_slot.in1->id);
  // NOLINTBEGIN(bugprone-unchecked-optional-access): engaged when superseding.
  if (!running ||
      !(running->saved ==
        track_chain::SavedStage{changed_slot.in1->id, changed_slot.in1->processor}) ||
      (superseding && (pending_native_revision_->track != changed->id ||
                       pending_native_revision_->effects != old_track->effects)))
    return Result<bool>::success(false);
  try {
    PendingNativeRevision pending{{}, changed->id, changed->effects, std::move(snapshot), {}};
    if (const auto* old_patch =
            std::get_if<project::NativeEffectPatch>(&changed_slot.in1->processor)) {
      const auto* next = std::get_if<project::NativeEffectPatch>(&changed_slot.in2->processor);
      if (!next)
        return Result<bool>::success(false);
      auto* stage = running->runtime.get();
      if (superseding &&
          pending_native_revision_->runtime !=
              decltype(pending.runtime){dynamic_cast<native_inserts::NativeEffectStage*>(stage)})
        return Result<bool>::success(false);
      native_inserts::NativeEffectStage* offered = nullptr;
      auto queued =
          offer_effect_values(stage, *old_patch, *next, pending.snapshot->revision, offered);
      if (!queued || !queued.value())
        return queued;
      pending.runtime = offered;
    } else {
      return Result<bool>::success(false);
    }
    if (superseding) {
      pending_native_revision_->effects = std::move(pending.effects);
      pending_native_revision_->snapshot = std::move(pending.snapshot);
    } else {
      pending_native_revision_.emplace(std::move(pending));
    }
    // NOLINTEND(bugprone-unchecked-optional-access)
    return Result<bool>::success(true);
  } catch (const std::bad_alloc&) {
    return Result<bool>::failure(
        {ErrorCode::resource_exhausted, "Live native parameter preparation ran out of memory"});
  }
}

Result<bool> RoutedPlaybackControl::try_queue_live_effect_group(
    std::shared_ptr<const project::Snapshot> snapshot) {
  // A linked group's shape edit changes built-in effects on several chains
  // at once. Each running stage takes the offer through its own mailbox;
  // the revision is active once every one has applied it. A newer group
  // edit to the same stages supersedes an unacknowledged one.
  const bool superseding = pending_native_revision_ && !pending_native_revision_->group.empty();
  const auto* previous =
      superseding ? pending_native_revision_->snapshot.get() : published_snapshot_.get();
  if (!previous || !published_snapshot_ || publisher_.active_revision() == 0 ||
      publisher_.has_pending() || (pending_native_revision_ && !superseding) ||
      (!superseding && revisions_.snapshot().active_graph_revision != previous->revision))
    return Result<bool>::success(false);
  if (previous->id != snapshot->id || previous->sample_rate != snapshot->sample_rate ||
      previous->sources != snapshot->sources ||
      previous->master_effects != snapshot->master_effects ||
      previous->source_tempo_reviews != snapshot->source_tempo_reviews ||
      previous->source_key_reviews != snapshot->source_key_reviews ||
      previous->next_insert_slot_serial != snapshot->next_insert_slot_serial ||
      previous->master_level != snapshot->master_level ||
      previous->tracks.size() != snapshot->tracks.size() || !same_timing(*previous, *snapshot))
    return Result<bool>::success(false);

  struct Change {
    const project::Track* track;
    const project::NativeEffectPatch* old_patch;
    const project::NativeEffectPatch* next;
    project::InsertSlotId slot;
  };
  std::vector<Change> changes;
  for (std::size_t index = 0; index < snapshot->tracks.size(); ++index) {
    const auto& old = previous->tracks[index];
    const auto& next = snapshot->tracks[index];
    if (!same_track_outside_effects(old, next))
      return Result<bool>::success(false);
    if (old.effects == next.effects)
      continue;
    if (old.effects.size() != next.effects.size())
      return Result<bool>::success(false);
    for (std::size_t e = 0; e < old.effects.size(); ++e) {
      const auto& before = old.effects[e];
      const auto& after = next.effects[e];
      if (before == after)
        continue;
      const auto* old_patch = std::get_if<project::NativeEffectPatch>(&before.processor);
      const auto* next_patch = std::get_if<project::NativeEffectPatch>(&after.processor);
      if (!old_patch || !next_patch || before.id != after.id || !before.enabled || !after.enabled)
        return Result<bool>::success(false);
      changes.push_back({&next, old_patch, next_patch, after.id});
    }
  }
  if (changes.size() < 2)
    return Result<bool>::success(false);
  if (superseding && (pending_native_revision_->group.size() != changes.size() ||
                      !std::ranges::equal(changes, pending_native_revision_->group,
                                          [](const Change& change, const PendingGroupStage& stage) {
                                            return change.track->id == stage.track &&
                                                   change.slot == stage.slot;
                                          })))
    return Result<bool>::success(false);
  try {
    std::vector<PendingGroupStage> group;
    group.reserve(changes.size());
    for (std::size_t i = 0; i < changes.size(); ++i) {
      const auto& change = changes[i];
      auto* running = active_stage(change.track->id, change.slot);
      // A superseded offer has not landed, so its stage still holds the
      // published settings: it must be the same instance.
      if (!running ||
          (!superseding &&
           !(running->saved ==
             track_chain::SavedStage{change.slot, project::EffectProcessor{*change.old_patch}})))
        return Result<bool>::success(false);
      auto* stage = dynamic_cast<native_inserts::NativeEffectStage*>(running->runtime.get());
      if (!stage || stage->slot() != change.slot ||
          // NOLINTNEXTLINE(bugprone-unchecked-optional-access): engaged when superseding.
          (superseding && pending_native_revision_->group[i].stage != stage))
        return Result<bool>::success(false);
      group.push_back({change.track->id, change.slot, stage});
    }
    // Every stage prepares the edit before any is offered, so a stage that
    // declines or refuses leaves them all as they were: a decline rebuilds,
    // a refusal fails the edit with every stage still in step with the
    // published project.
    std::vector<PreparedStageOffer> offers;
    offers.reserve(changes.size());
    for (std::size_t i = 0; i < changes.size(); ++i) {
      const auto& change = changes[i];
      auto prepared = prepare_effect_values(group[i].stage, *change.old_patch, *change.next,
                                            snapshot->revision);
      if (!prepared)
        return Result<bool>::failure(prepared.error());
      if (!prepared.value())
        return Result<bool>::success(false);
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access): engaged above.
      offers.push_back(std::move(*prepared.value()));
    }
    for (auto& offer : offers) {
      if (auto committed = commit_effect_values(offer); !committed)
        return Result<bool>::failure(committed.error());
    }
    const auto first = changes.front().track;
    PendingNativeRevision pending{group.front().stage, first->id, first->effects,
                                  std::move(snapshot), std::move(group)};
    pending_native_revision_ = std::move(pending);
    return Result<bool>::success(true);
  } catch (const std::bad_alloc&) {
    return Result<bool>::failure(
        {ErrorCode::resource_exhausted, "Live linked effect preparation ran out of memory"});
  }
}

Result<bool> RoutedPlaybackControl::try_queue_live_master_native(
    std::shared_ptr<const project::Snapshot> snapshot) {
  // The same stateful master processor can consume a newer bounded mailbox
  // offer without rebuilding its chain or discarding downstream DSP history.
  const bool superseding =
      pending_native_revision_ && !pending_native_revision_->track &&
      std::holds_alternative<native_inserts::NativeEffectStage*>(pending_native_revision_->runtime);
  const auto* previous =
      superseding ? pending_native_revision_->snapshot.get() : published_snapshot_.get();
  if (!previous || !published_snapshot_ || publisher_.active_revision() == 0 ||
      publisher_.has_pending() || (pending_native_revision_ && !superseding) ||
      (!superseding && revisions_.snapshot().active_graph_revision != previous->revision) ||
      (superseding && pending_native_revision_->effects != previous->master_effects))
    return Result<bool>::success(false);
  if (previous->id != snapshot->id || previous->sample_rate != snapshot->sample_rate ||
      previous->sources != snapshot->sources || previous->tracks != snapshot->tracks ||
      previous->source_tempo_reviews != snapshot->source_tempo_reviews ||
      previous->source_key_reviews != snapshot->source_key_reviews ||
      previous->next_insert_slot_serial != snapshot->next_insert_slot_serial ||
      previous->master_level != snapshot->master_level || !same_timing(*previous, *snapshot) ||
      previous->master_effects.empty() ||
      previous->master_effects.size() != snapshot->master_effects.size())
    return Result<bool>::success(false);
  const auto changed = std::ranges::mismatch(previous->master_effects, snapshot->master_effects);
  if (changed.in1 == previous->master_effects.end() ||
      !std::ranges::equal(std::next(changed.in1), previous->master_effects.end(),
                          std::next(changed.in2), snapshot->master_effects.end()) ||
      changed.in1->id != changed.in2->id || !changed.in1->enabled || !changed.in2->enabled)
    return Result<bool>::success(false);
  auto* running = active_stage(std::nullopt, changed.in1->id);
  if (!running ||
      !(running->saved == track_chain::SavedStage{changed.in1->id, changed.in1->processor}))
    return Result<bool>::success(false);
  try {
    PendingNativeRevision pending{
        {}, std::nullopt, snapshot->master_effects, std::move(snapshot), {}};
    if (const auto* old_patch = std::get_if<project::NativeEffectPatch>(&changed.in1->processor)) {
      const auto* next = std::get_if<project::NativeEffectPatch>(&changed.in2->processor);
      if (!next)
        return Result<bool>::success(false);
      auto* stage = running->runtime.get();
      // NOLINTBEGIN(bugprone-unchecked-optional-access): engaged when superseding.
      if (superseding &&
          pending_native_revision_->runtime !=
              decltype(pending.runtime){dynamic_cast<native_inserts::NativeEffectStage*>(stage)})
        return Result<bool>::success(false);
      native_inserts::NativeEffectStage* offered = nullptr;
      auto queued =
          offer_effect_values(stage, *old_patch, *next, pending.snapshot->revision, offered);
      if (!queued || !queued.value())
        return queued;
      pending.runtime = offered;
    } else {
      return Result<bool>::success(false);
    }
    if (superseding) {
      pending_native_revision_->effects = std::move(pending.effects);
      pending_native_revision_->snapshot = std::move(pending.snapshot);
    } else {
      pending_native_revision_.emplace(std::move(pending));
    }
    // NOLINTEND(bugprone-unchecked-optional-access)
    return Result<bool>::success(true);
  } catch (const std::bad_alloc&) {
    return Result<bool>::failure(
        {ErrorCode::resource_exhausted, "Live master effect preparation ran out of memory"});
  }
}

// A built-in synth edit that keeps the engine and every other project field
// is offered to the running synth: continuous values glide, next-note values
// apply to new notes, and sounding voices are never reset.
Result<bool>
RoutedPlaybackControl::try_queue_live_synth(std::shared_ptr<const project::Snapshot> snapshot) {
  const bool superseding =
      pending_native_revision_ && std::holds_alternative<native_dsp::synth::NativeSynthRuntime*>(
                                      pending_native_revision_->runtime);
  const auto* previous =
      superseding ? pending_native_revision_->snapshot.get() : published_snapshot_.get();
  if (!previous || !live_source_ || publisher_.active_revision() == 0 || publisher_.has_pending() ||
      (pending_native_revision_ && !superseding) ||
      (!superseding && revisions_.snapshot().active_graph_revision != previous->revision) ||
      previous->id != snapshot->id || previous->sample_rate != snapshot->sample_rate ||
      previous->sources != snapshot->sources ||
      previous->master_effects != snapshot->master_effects ||
      previous->master_level != snapshot->master_level ||
      previous->source_tempo_reviews != snapshot->source_tempo_reviews ||
      previous->source_key_reviews != snapshot->source_key_reviews ||
      previous->next_insert_slot_serial != snapshot->next_insert_slot_serial ||
      previous->tracks.size() != snapshot->tracks.size() || !same_timing(*previous, *snapshot))
    return Result<bool>::success(false);
  const project::Track* changed = nullptr;
  const project::NativeInstrumentPatch* patch = nullptr;
  for (std::size_t index = 0; index < previous->tracks.size(); ++index) {
    const auto& old_track = previous->tracks[index];
    const auto& next_track = snapshot->tracks[index];
    if (old_track == next_track)
      continue;
    if (changed)
      return Result<bool>::success(false);
    const auto* old_patch =
        std::get_if<project::NativeInstrumentPatch>(&old_track.instrument_source);
    const auto* next_patch =
        std::get_if<project::NativeInstrumentPatch>(&next_track.instrument_source);
    if (!old_patch || !next_patch || old_patch->engine != next_patch->engine ||
        old_patch->engine_version != next_patch->engine_version ||
        old_track.kind != project::TrackKind::instrument)
      return Result<bool>::success(false);
    auto masked = old_track;
    masked.instrument_source = next_track.instrument_source;
    if (masked != next_track)
      return Result<bool>::success(false);
    changed = &next_track;
    patch = next_patch;
  }
  if (!changed || !patch)
    return Result<bool>::success(false);
  auto* runtime = live_source_->synth_runtime(changed->id);
  if (!runtime)
    return Result<bool>::failure(
        {ErrorCode::unavailable, "Live synth is unavailable; saved edit is not active"});
  if (superseding && (pending_native_revision_->track != changed->id ||
                      std::get<native_dsp::synth::NativeSynthRuntime*>(
                          pending_native_revision_->runtime) != runtime))
    return Result<bool>::success(false);
  auto resolved = native_inserts::resolve_native_instrument(*patch);
  if (!resolved)
    return Result<bool>::failure(resolved.error());
  if (resolved.value().engine->descriptor != runtime->engine().descriptor)
    return Result<bool>::success(false);
  // A document goes ahead of its revision's values (ADR-0057). New managed
  // media need decoding, so those edits rebuild the source instead.
  if (resolved.value().document != runtime->document()) {
    auto offered = runtime->offer_document(resolved.value().document, snapshot->revision);
    if (!offered)
      return Result<bool>::failure(offered.error());
    if (!offered.value())
      return Result<bool>::success(false);
  }
  if (!runtime->offer_values(resolved.value().values, snapshot->revision)) {
    // A drag may end on this value: it applies once the callback drains an
    // offer (observe()).
    deferred_revision_ = snapshot->revision;
    return Result<bool>::success(true);
  }
  if (superseding) {
    pending_native_revision_->snapshot = std::move(snapshot);
  } else {
    PendingNativeRevision pending{{}, changed->id, {}, std::move(snapshot), {}};
    pending.runtime = runtime;
    pending_native_revision_.emplace(std::move(pending));
  }
  return Result<bool>::success(true);
}

// An edit confined to built-in instrument tracks' notes, pedals, synth and
// note effect lanes, note effect values and switches and macro values goes
// to each track's running player as one plan, adopted at its next block: no
// rebuild and no gap, and the voices keep playing (ADR-0093). Any number of
// tracks may change in one edit; each acknowledges the same revision.
Result<bool>
RoutedPlaybackControl::try_queue_live_players(std::shared_ptr<const project::Snapshot> snapshot) {
  const bool superseding =
      pending_native_revision_ &&
      std::holds_alternative<native_inserts::NativeSynthPlayer*>(pending_native_revision_->runtime);
  // Compared with what plays, so a superseded offer's tracks are re-offered.
  const auto* previous = published_snapshot_.get();
  if (!previous || publisher_.active_revision() == 0 || publisher_.has_pending() ||
      (pending_native_revision_ && !superseding) ||
      revisions_.snapshot().active_graph_revision != previous->revision ||
      previous->id != snapshot->id || previous->sample_rate != snapshot->sample_rate ||
      previous->sources != snapshot->sources ||
      previous->master_effects != snapshot->master_effects ||
      previous->master_level != snapshot->master_level ||
      previous->source_tempo_reviews != snapshot->source_tempo_reviews ||
      previous->source_key_reviews != snapshot->source_key_reviews ||
      previous->next_insert_slot_serial != snapshot->next_insert_slot_serial ||
      previous->tracks.size() != snapshot->tracks.size() || !same_timing(*previous, *snapshot))
    return Result<bool>::success(false);
  std::vector<const project::Track*> changed;
  // VST3 instrument tracks whose MIDI changed (ADR-0101).
  std::vector<const project::Track*> plugins;
  bool silent = false;
  for (std::size_t index = 0; index < previous->tracks.size(); ++index) {
    const auto& old_track = previous->tracks[index];
    const auto& next_track = snapshot->tracks[index];
    const bool offered =
        superseding &&
        (std::ranges::any_of(
             pending_players_,
             [&](const auto* player) { return player->track() == next_track.id; }) ||
         std::ranges::find(pending_vst3_tracks_, next_track.id) != pending_vst3_tracks_.end());
    if (old_track == next_track && !offered)
      continue;
    if (silent_midi_edit_only(old_track, next_track)) {
      silent = true;
      continue;
    }
    if (vst3_midi_edit_only(old_track, next_track)) {
      plugins.push_back(&next_track);
      continue;
    }
    if (!player_edit_only(old_track, next_track))
      return Result<bool>::success(false);
    changed.push_back(&next_track);
  }
  if (changed.empty() && plugins.empty() && silent && !superseding) {
    // Nothing that sounds changed: the revision is audible at once.
    published_snapshot_ = snapshot;
    source_snapshot_ = std::move(snapshot);
    (void)revisions_.observe_active_graph(published_snapshot_->revision);
    follow_published_notes();
    return Result<bool>::success(true);
  }
  if (changed.empty() && plugins.empty())
    return Result<bool>::success(false);
  // Each changed VST3 instrument's running stage takes its new MIDI.
  std::vector<const Vst3Stage*> stages;
  stages.reserve(plugins.size());
  for (const auto* track : plugins) {
    const auto stage = std::ranges::find_if(vst3_stages_, [track](const Vst3Stage& record) {
      return record.track == track->id && !record.slot;
    });
    if (stage == vst3_stages_.end() || !stage->prepared.prepare_events ||
        !stage->prepared.applied_events_revision)
      return Result<bool>::success(false);
    stages.push_back(&*stage);
  }
  std::vector<native_inserts::NativeSynthPlayer*> players;
  players.reserve(changed.size());
  for (const auto* track : changed) {
    auto* player = live_source_ ? live_source_->synth_player(track->id) : nullptr;
    // A new note effect layout (an effect added, moved or removed) rebuilds
    // the source.
    if (!player || !player->same_note_effects(track->note_effects))
      return Result<bool>::success(false);
    players.push_back(player);
  }
  // Every track takes the revision, or none does: a full mailbox waits for
  // the callback to move on (observe() applies the newest revision then).
  if (!std::ranges::all_of(players, [](auto* player) { return player->has_room(); })) {
    deferred_revision_ = snapshot->revision;
    return Result<bool>::success(true);
  }
  auto midi = engine::compile_midi_events(*snapshot);
  if (!midi)
    return Result<bool>::failure(midi.error());
  auto prepared = automation_project::prepare_project_automation(*snapshot, snapshot->revision);
  if (!prepared)
    return Result<bool>::failure(prepared.error());
  auto limit = automation_sample_limit(*snapshot, limits_.max_frames);
  if (!limit)
    return Result<bool>::failure(limit.error());
  // Every plan is built and checked before any player gets one, so the
  // edit reaches all of its tracks or none.
  std::vector<native_inserts::NativeSynthPlayer::PreparedPlan> plans;
  try {
    plans.reserve(changed.size());
    for (std::size_t index = 0; index < changed.size(); ++index) {
      const auto& track = *changed[index];
      const engine::InstrumentEvents empty{track.id, {}};
      const auto events =
          std::ranges::find(midi.value().instruments(), track.id, &engine::InstrumentEvents::track);
      auto plan = players[index]->prepare_plan(
          *snapshot, track, events == midi.value().instruments().end() ? empty : *events,
          prepared.value(), limit.value());
      if (!plan)
        return Result<bool>::failure(plan.error());
      plans.push_back(std::move(plan).value());
    }
  } catch (const std::bad_alloc&) {
    return Result<bool>::failure(
        {ErrorCode::resource_exhausted, "Live instrument edit ran out of memory"});
  }
  // The same for VST3 instruments: every plan is built and given room before
  // any instrument gets one.
  std::vector<std::function<void()>> commits;
  std::vector<std::function<std::uint64_t()>> probes;
  std::vector<project::TrackId> plugin_tracks;
  try {
    commits.reserve(stages.size());
    probes.reserve(stages.size());
    plugin_tracks.reserve(stages.size());
    for (std::size_t index = 0; index < stages.size(); ++index) {
      auto commit = stages[index]->prepared.prepare_events(*snapshot, false);
      if (!commit) {
        // A full mailbox applies the newest revision once the callback moves on.
        if (commit.error().code == ErrorCode::resource_exhausted) {
          deferred_revision_ = snapshot->revision;
          return Result<bool>::success(true);
        }
        return Result<bool>::failure(commit.error());
      }
      commits.push_back(std::move(commit).value());
      probes.push_back(stages[index]->prepared.applied_events_revision);
      plugin_tracks.push_back(plugins[index]->id);
    }
  } catch (const std::bad_alloc&) {
    return Result<bool>::failure(
        {ErrorCode::resource_exhausted, "Live instrument edit ran out of memory"});
  }
  const auto first = changed.empty() ? plugins.front()->id : changed.front()->id;
  PendingNativeRevision pending{{}, first, {}, std::move(snapshot), {}};
  pending.runtime = players.empty() ? nullptr : players.front();
  for (std::size_t index = 0; index < players.size(); ++index)
    players[index]->commit_plan(std::move(plans[index]), *pending.snapshot);
  for (auto& commit : commits)
    commit();
  pending_players_ = std::move(players);
  pending_vst3_ = std::move(probes);
  pending_vst3_tracks_ = std::move(plugin_tracks);
  pending_native_revision_ = std::move(pending);
  return Result<bool>::success(true);
}

Result<bool> RoutedPlaybackControl::try_queue_live_effect_automation(
    std::shared_ptr<const project::Snapshot> snapshot) {
  const bool superseding = pending_native_revision_ && pending_native_revision_->track &&
                           std::holds_alternative<native_inserts::NativeEffectStage*>(
                               pending_native_revision_->runtime) &&
                           pending_native_revision_->effects.empty();
  const auto* previous =
      superseding ? pending_native_revision_->snapshot.get() : published_snapshot_.get();
  if (!previous || publisher_.active_revision() == 0 || publisher_.has_pending() ||
      (pending_native_revision_ && !superseding) ||
      (!superseding && revisions_.snapshot().active_graph_revision != previous->revision) ||
      previous->id != snapshot->id || previous->sample_rate != snapshot->sample_rate ||
      previous->sources != snapshot->sources ||
      previous->master_effects != snapshot->master_effects ||
      previous->master_level != snapshot->master_level ||
      previous->source_tempo_reviews != snapshot->source_tempo_reviews ||
      previous->source_key_reviews != snapshot->source_key_reviews ||
      previous->next_insert_slot_serial != snapshot->next_insert_slot_serial ||
      previous->tracks.size() != snapshot->tracks.size() || !same_timing(*previous, *snapshot))
    return Result<bool>::success(false);
  const project::Track* changed = nullptr;
  std::optional<project::InsertSlotId> slot;
  for (std::size_t index = 0; index < previous->tracks.size(); ++index) {
    const auto& old_track = previous->tracks[index];
    const auto& next_track = snapshot->tracks[index];
    if (old_track == next_track)
      continue;
    if (changed)
      return Result<bool>::success(false);
    auto masked = old_track;
    masked.automation = next_track.automation;
    if (masked != next_track)
      return Result<bool>::success(false);
    // Every differing lane must target one built-in effect slot.
    const auto lane_slot = [](const project::AutomationLane& lane) {
      const auto* target = std::get_if<project::NativeEffectParameterTarget>(&lane.target);
      return target ? std::optional(target->slot) : std::nullopt;
    };
    const auto note = [&](const project::AutomationLane& lane) {
      const auto target = lane_slot(lane);
      if (!target || (slot && *slot != *target))
        return false;
      slot = target;
      return true;
    };
    for (const auto& lane : old_track.automation) {
      if (std::ranges::find(next_track.automation, lane) == next_track.automation.end() &&
          !note(lane))
        return Result<bool>::success(false);
    }
    for (const auto& lane : next_track.automation) {
      if (std::ranges::find(old_track.automation, lane) == old_track.automation.end() &&
          !note(lane))
        return Result<bool>::success(false);
    }
    changed = &next_track;
  }
  if (!changed || !slot)
    return Result<bool>::success(false);
  const auto* chain = active_chain(changed->id);
  if (!chain || !runs(chain->chain, track_chain::saved_chain(*changed)))
    return Result<bool>::success(false);
  auto* running = active_stage(changed->id, *slot);
  auto* stage =
      running ? dynamic_cast<native_inserts::NativeEffectStage*>(running->runtime.get()) : nullptr;
  if (!stage || stage->slot() != *slot)
    return Result<bool>::success(false);
  // NOLINTBEGIN(bugprone-unchecked-optional-access): engaged when superseding.
  if (superseding &&
      (pending_native_revision_->track != changed->id ||
       std::get<native_inserts::NativeEffectStage*>(pending_native_revision_->runtime) != stage))
    return Result<bool>::success(false);
  auto prepared = automation_project::prepare_project_automation(*snapshot, snapshot->revision);
  if (!prepared)
    return Result<bool>::failure(prepared.error());
  auto limit = automation_sample_limit(*snapshot, limits_.max_frames);
  if (!limit)
    return Result<bool>::failure(limit.error());
  auto offered = stage->offer_automation(*snapshot, prepared.value(), limit.value());
  if (!offered) {
    // A full mailbox applies the newest revision once the callback drains it.
    if (offered.error().code == ErrorCode::resource_exhausted) {
      deferred_revision_ = snapshot->revision;
      return Result<bool>::success(true);
    }
    return Result<bool>::failure(offered.error());
  }
  if (superseding) {
    pending_native_revision_->snapshot = std::move(snapshot);
  } else {
    // No effects: the chain's saved slots are unchanged by a lane edit.
    PendingNativeRevision pending{{}, changed->id, {}, std::move(snapshot), {}};
    pending.runtime = stage;
    pending_native_revision_.emplace(std::move(pending));
  }
  // NOLINTEND(bugprone-unchecked-optional-access)
  return Result<bool>::success(true);
}

} // namespace oscillate::application
