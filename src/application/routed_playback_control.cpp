#include "oscillate/application/routed_playback_control.hpp"

#include "routed_playback/edit_rules.hpp"

#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/foundation/trace.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "oscillate/native_inserts/instrument_binding.hpp"
#include "oscillate/native_inserts/native_stage.hpp"
#include "oscillate/track_chain/track_chain.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

// Routed playback's revision orchestration: what each saved edit asks of
// the running graph, and the device and revision state it settles.
namespace oscillate::application {
namespace {
using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;
using routed_playback::needs_whole_switch;
using routed_playback::rejected;
using routed_playback::same_audio_content;
using routed_playback::same_timing;

[[nodiscard]] bool native_synth_automation_changed(const project::Snapshot& before,
                                                   const project::Snapshot& after) {
  const auto native_lane = [](const project::AutomationLane& lane) {
    // A macro lane may drive the synth's parameters (ADR-0059).
    return std::holds_alternative<project::NativeParameterTarget>(lane.target) ||
           std::holds_alternative<project::MacroTarget>(lane.target);
  };
  for (const auto& old_track : before.tracks) {
    if (!std::holds_alternative<project::NativeInstrumentPatch>(old_track.instrument_source))
      continue;
    const auto next = std::ranges::find(after.tracks, old_track.id, &project::Track::id);
    if (next != after.tracks.end() &&
        !std::ranges::equal(old_track.automation | std::views::filter(native_lane),
                            next->automation | std::views::filter(native_lane)))
      return true;
  }
  return false;
}

// An edit the routed graph can switch to live: the same tracks, perhaps
// reordered, with unchanged sources (clips, built-in instruments, MIDI).
// Chains, routing, levels and sends may differ; the callback hands the
// running state over at the switching block (ADR-0049, ADR-0050). A VST3
// instrument is a chain stage, so it compares against the audible graph,
// which follows its live parameter edits.
[[nodiscard]] bool live_switch_eligible(const project::Snapshot& published,
                                        const project::Snapshot& source,
                                        const project::Snapshot& candidate) {
  if (published.tracks.size() != candidate.tracks.size() ||
      source.tracks.size() != candidate.tracks.size())
    return false;
  // Tracks are matched by identity: a reorder keeps every source, and graph
  // publication refuses one that would change the routed source order.
  for (const auto& next : candidate.tracks) {
    const auto old = std::ranges::find(published.tracks, next.id, &project::Track::id);
    const auto sounding = std::ranges::find(source.tracks, next.id, &project::Track::id);
    if (old == published.tracks.end() || sounding == source.tracks.end())
      return false;
    const auto& instrument =
        std::holds_alternative<project::Vst3PluginInstance>(next.instrument_source) ? *old
                                                                                    : *sounding;
    if (instrument.instrument_source != next.instrument_source ||
        sounding->midi_clips != next.midi_clips)
      return false;
  }
  return true;
}

// What keeps an edit from switching while playing, when it is neither a
// live graph switch, an instrument plan nor a primed source switch. The old
// graph plays on; the edit is saved for the next Play.
[[nodiscard]] std::string live_refusal(const project::Snapshot& published,
                                       const project::Snapshot& candidate) {
  if (needs_whole_switch(published, candidate))
    return "This edit is saved but not audible until its sources are prepared "
           "(capture_source_edit)";
  return "This edit cannot switch while playing";
}

// Nothing that plays changed but, perhaps, the project key (and with it
// nothing but presentation): the running synths hear the key; no graph,
// chain or source changes. An edit undone by the next one lands here too.
// The tracks without their rebalance roles, which the running stages take
// live (ADR-0123).
[[nodiscard]] std::vector<project::Track> without_roles(std::vector<project::Track> tracks) {
  for (auto& track : tracks) {
    track.mix_role.reset();
    track.heard_role.reset();
  }
  return tracks;
}

// Nothing a graph is built from changed: the snapshots differ at most in
// what is not heard and in the master rebalance, which the running stages
// take live (ADR-0123).
[[nodiscard]] bool same_audible_state(const project::Snapshot& before,
                                      const project::Snapshot& after) {
  return after.revision > before.revision && before.id == after.id &&
         before.sample_rate == after.sample_rate && same_timing(before, after) &&
         before.sources == after.sources && before.master_effects == after.master_effects &&
         before.master_level == after.master_level &&
         before.source_tempo_reviews == after.source_tempo_reviews &&
         before.source_key_reviews == after.source_key_reviews &&
         before.next_insert_slot_serial == after.next_insert_slot_serial &&
         without_roles(before.tracks) == without_roles(after.tracks);
}

[[nodiscard]] bool channel_controls_only(const project::Snapshot& before,
                                         const project::Snapshot& after) {
  if (after.revision <= before.revision || before.id != after.id ||
      before.sample_rate != after.sample_rate || !same_timing(before, after) ||
      before.sources != after.sources || before.master_effects != after.master_effects ||
      before.source_tempo_reviews != after.source_tempo_reviews ||
      before.source_key_reviews != after.source_key_reviews ||
      before.next_insert_slot_serial != after.next_insert_slot_serial ||
      before.tracks.size() != after.tracks.size())
    return false;
  bool changed = before.master_level != after.master_level;
  for (std::size_t index = 0; index < before.tracks.size(); ++index) {
    const auto& old_track = before.tracks[index];
    const auto& new_track = after.tracks[index];
    if (old_track.id != new_track.id || old_track.clips != new_track.clips ||
        old_track.kind != new_track.kind ||
        old_track.instrument_source != new_track.instrument_source ||
        old_track.midi_clips != new_track.midi_clips || old_track.effects != new_track.effects ||
        old_track.main_output != new_track.main_output || old_track.sends != new_track.sends ||
        old_track.automation != new_track.automation)
      return false;
    changed |= old_track.muted != new_track.muted || old_track.soloed != new_track.soloed ||
               old_track.level != new_track.level || old_track.pan != new_track.pan;
  }
  return changed;
}

[[nodiscard]] bool send_gains_only(const project::Snapshot& before,
                                   const project::Snapshot& after) {
  if (after.revision <= before.revision || before.id != after.id ||
      before.sample_rate != after.sample_rate || !same_timing(before, after) ||
      before.sources != after.sources || before.master_effects != after.master_effects ||
      before.master_level != after.master_level ||
      before.source_tempo_reviews != after.source_tempo_reviews ||
      before.source_key_reviews != after.source_key_reviews ||
      before.next_insert_slot_serial != after.next_insert_slot_serial ||
      before.tracks.size() != after.tracks.size())
    return false;
  bool changed = false;
  for (std::size_t index = 0; index < before.tracks.size(); ++index) {
    const auto& old_track = before.tracks[index];
    const auto& new_track = after.tracks[index];
    if (old_track.id != new_track.id || old_track.clips != new_track.clips ||
        old_track.kind != new_track.kind ||
        old_track.instrument_source != new_track.instrument_source ||
        old_track.midi_clips != new_track.midi_clips || old_track.effects != new_track.effects ||
        old_track.main_output != new_track.main_output ||
        old_track.automation != new_track.automation || old_track.muted != new_track.muted ||
        old_track.soloed != new_track.soloed || old_track.level != new_track.level ||
        old_track.pan != new_track.pan || old_track.sends.size() != new_track.sends.size())
      return false;
    for (std::size_t send = 0; send < old_track.sends.size(); ++send) {
      const auto& old_route = old_track.sends[send];
      const auto& new_route = new_track.sends[send];
      if (old_route.id != new_route.id || old_route.destination != new_route.destination ||
          old_route.purpose != new_route.purpose ||
          old_route.target_insert != new_route.target_insert)
        return false;
      changed |= old_route.gain != new_route.gain;
    }
  }
  return changed;
}

} // namespace

RoutedPlaybackControl::RoutedPlaybackControl(project::ProjectModel& model,
                                             engine::RoutedMixerLimits limits,
                                             TrackInsertProvider* inserts,
                                             std::filesystem::path project_root)
    : model_(model), limits_(limits), inserts_(inserts), project_root_(std::move(project_root)),
      revisions_(model.snapshot()->revision, 0, ChangeDescriptor{"open project", {}}),
      source_snapshot_(model.snapshot()) {
  limits_.latency.master = 1;
}

std::optional<Error> RoutedPlaybackControl::processor_fault() const {
  for (const auto& stage : vst3_stages_) {
    if (stage.prepared.process_failed && stage.prepared.process_failed())
      return Error{
          ErrorCode::unavailable,
          "Active VST3 stage on " +
              (stage.track ? "track " + stage.track->to_string() : std::string("the master")) +
              " failed during processing; stop and restart playback after resolving "
              "the plugin"};
  }
  return std::nullopt;
}

std::optional<Error> RoutedPlaybackControl::processor_fault(project::TrackId track) const {
  for (const auto& stage : vst3_stages_) {
    if (stage.track == track && stage.prepared.process_failed && stage.prepared.process_failed())
      return Error{ErrorCode::unavailable,
                   "Active VST3 stage on track " + track.to_string() +
                       " failed during processing; stop and restart playback after resolving "
                       "the plugin"};
  }
  return std::nullopt;
}

const RoutedPlaybackControl::ActiveChain*
RoutedPlaybackControl::active_chain(std::optional<project::TrackId> track) const {
  const auto found = std::ranges::find(active_chains_, track, &ActiveChain::track);
  return found == active_chains_.end() ? nullptr : &*found;
}

RoutedPlaybackControl::ActiveChain*
RoutedPlaybackControl::active_chain(std::optional<project::TrackId> track) {
  const auto found = std::ranges::find(active_chains_, track, &ActiveChain::track);
  return found == active_chains_.end() ? nullptr : &*found;
}

track_chain::Stage* RoutedPlaybackControl::active_stage(std::optional<project::TrackId> track,
                                                        project::InsertSlotId insert) {
  auto* chain = active_chain(track);
  if (chain == nullptr)
    return nullptr;
  const auto found =
      std::ranges::find(chain->chain.stages, std::optional(insert),
                        [](const track_chain::Stage& stage) { return stage.saved.slot; });
  return found == chain->chain.stages.end() ? nullptr : &*found;
}

Result<void> RoutedPlaybackControl::fail(std::uint64_t revision, Error error) {
  (void)revisions_.fail_revision(revision, error);
  return Result<void>::failure(std::move(error));
}

void RoutedPlaybackControl::follow_published_notes() {
  refresh_effect_key_notes();
  refresh_synth_locks();
}

void RoutedPlaybackControl::refresh_effect_key_notes() {
  // Effects that read their keying tracks' notes ahead (Pump's MIDI
  // trigger, ADR-0058) take the new notes on a kept stage, no rebuild. A
  // failure keeps the notes it had.
  for (auto& chain : active_chains_) {
    for (auto& stage : chain.chain.stages) {
      if (auto* effect = dynamic_cast<native_inserts::NativeEffectStage*>(stage.runtime.get()))
        (void)effect->refresh_key_notes(*published_snapshot_);
    }
  }
}

void RoutedPlaybackControl::settle_live_candidate() {
  if (!live_candidate_)
    return;
  // Pending first: the callback clears it only after storing an adopted
  // candidate as active, so "not pending and not active" means withdrawn.
  const bool pending = publisher_.has_pending();
  if (publisher_.active_revision() >= live_candidate_->revision) {
    live_candidate_.reset(); // Adopted.
    return;
  }
  if (pending) {
    if (!live_candidate_->withdrawing &&
        publisher_.live_candidate_passed_blocks() - live_candidate_->passed_at_publication >=
            live_candidate_passed_block_limit)
      live_candidate_->withdrawing = publisher_.withdraw_pending();
    return;
  }
  // Withdrawn, or discarded by the callback as prepared against another
  // graph: the old graph played on, so the control state it replaced returns.
  auto prior = std::move(*live_candidate_);
  live_candidate_.reset();
  published_snapshot_ = std::move(prior.published_snapshot);
  source_snapshot_ = std::move(prior.source_snapshot);
  published_route_plan_ = std::move(prior.route_plan);
  active_chains_ = std::move(prior.chains);
  vst3_stages_ = std::move(prior.vst3_stages);
  active_vst3_automation_ = std::move(prior.vst3_automation);
  prepared_limiter_ = prior.prepared_limiter;
  master_output_latency_frames_ = prior.master_output_latency_frames;
  if (!prior.topological_order.empty()) {
    sources_ = std::move(prior.sources);
    topological_order_ = std::move(prior.topological_order);
  }
  if (published_snapshot_)
    follow_published_notes();
  (void)revisions_.fail_revision(
      prior.revision, {ErrorCode::unavailable,
                       "The edit's graph could not take over playback; the previous graph plays "
                       "on. The edit is saved: edit again or retry to apply it"});
}

void RoutedPlaybackControl::refresh_synth_locks() {
  if (live_source_ != nullptr && published_snapshot_) {
    live_source_->follow_project_key(published_snapshot_->key);
    live_source_->refresh_synth_locks(*published_snapshot_);
  }
}

Result<void> RoutedPlaybackControl::submit_current() {
  if (publisher_.active_revision() != 0 || publisher_.has_pending()) {
    return rejected(ErrorCode::conflict, "Routed playback is already starting or active");
  }
  const auto current = model_.snapshot();
  if (current->revision > revisions_.snapshot().model_revision) {
    (void)revisions_.accept_model_revision(current->revision,
                                           ChangeDescriptor{"project refresh", {}});
  }
  return build_and_publish(current);
}

void RoutedPlaybackControl::bind_live_source(RoutedAudioSource& source) noexcept {
  live_source_ = &source;
}

void RoutedPlaybackControl::unbind_live_source() noexcept {
  // Audio is quiescent. Do not let a later control-side status poll dereference
  // the retired synth runtime while its final mailbox offer is pending.
  if (pending_native_revision_ && (std::holds_alternative<native_dsp::synth::NativeSynthRuntime*>(
                                       pending_native_revision_->runtime) ||
                                   std::holds_alternative<native_inserts::NativeSynthPlayer*>(
                                       pending_native_revision_->runtime))) {
    pending_native_revision_.reset();
  }
  pending_players_.clear();
  pending_vst3_.clear();
  pending_vst3_tracks_.clear();
  deferred_revision_.reset();
  live_source_ = nullptr;
}

Result<void> RoutedPlaybackControl::begin_current() {
  if (publisher_.has_pending()) {
    return rejected(ErrorCode::conflict, "A routed graph update is already pending");
  }
  const auto current = model_.snapshot();
  if (publisher_.active_revision() == 0) {
    return build_and_publish(current);
  }
  (void)publisher_.collect_retired();
  // Chain, routing and level edits switch at the next block while the
  // current graph keeps playing (ADR-0049, ADR-0050). Sources switch through
  // their own primed handoff (capture_source_edit); anything else is refused
  // at once, with the old graph playing on (ADR-0094).
  if (published_snapshot_ && source_snapshot_ && same_audio_content(*source_snapshot_, *current) &&
      live_switch_eligible(*published_snapshot_, *source_snapshot_, *current))
    return build_and_publish(current);
  return fail(current->revision, {ErrorCode::unsupported,
                                  published_snapshot_ ? live_refusal(*published_snapshot_, *current)
                                                      : "Routed playback has no published graph"});
}

Result<void> RoutedPlaybackControl::reject_edit(const project::AppliedRevision& applied,
                                                std::string_view command_label, Error error) {
  observe();
  const auto current = model_.snapshot();
  if (applied.snapshot != current || current->revision <= revisions_.snapshot().model_revision)
    return rejected(ErrorCode::conflict, "Routed project edit was superseded");
  (void)revisions_.accept_model_revision(
      current->revision, ChangeDescriptor{std::string(command_label), applied.changes});
  deferred_revision_.reset();
  return fail(current->revision, std::move(error));
}

Result<void> RoutedPlaybackControl::begin_edit(const project::AppliedRevision& applied,
                                               std::string_view command_label) {
  OSCILLATE_TRACE_SPAN("edit.begin");
  observe();
  const auto current = model_.snapshot();
  if (applied.snapshot != current || current->revision <= revisions_.snapshot().model_revision) {
    return rejected(ErrorCode::conflict, "Routed project edit was superseded");
  }
  (void)revisions_.accept_model_revision(
      current->revision, ChangeDescriptor{std::string(command_label), applied.changes});
  preparing_source_.reset(); // A newer saved edit invalidates any worker capture.
  return apply_current();
}

Result<void> RoutedPlaybackControl::apply_current() {
  OSCILLATE_TRACE_SPAN("edit.apply");
  const auto current = model_.snapshot();
  deferred_revision_.reset();
  // The running rebalance stages follow every edit at once; a graph this
  // edit builds takes them as they are.
  if (auto rebalanced = rebalance_.update(*current); !rebalanced)
    return fail(current->revision, rebalanced.error());
  // An edit made while an earlier one is still switching in waits for it;
  // observe() then applies the newest saved revision, so rapid edits
  // coalesce instead of being refused.
  const auto defer = [this, &current] {
    deferred_revision_ = current->revision;
    return Result<void>::success();
  };
  if (publisher_.has_pending() || pending_manual_ ||
      (published_snapshot_ && !published_snapshot_audible()))
    return defer();
  if (published_snapshot_ && published_snapshot_audible() && !pending_native_revision_ &&
      same_audible_state(*published_snapshot_, *current)) {
    // Every running synth adopts the key at its next block; nothing else
    // that plays changed.
    if (live_source_ != nullptr && published_snapshot_->key != current->key)
      live_source_->follow_project_key(current->key);
    published_snapshot_ = current;
    source_snapshot_ = current;
    (void)revisions_.observe_active_graph(current->revision);
    return Result<void>::success();
  }
  if (published_snapshot_ && published_route_plan_ && published_snapshot_audible() &&
      !pending_native_revision_ && channel_controls_only(*published_snapshot_, *current)) {
    auto controls = routing::project_node_controls(*current, *published_route_plan_);
    if (!controls)
      return fail(current->revision, controls.error());
    // A mute or solo also decides which sidechain keys sound.
    auto edges = routing::project_edge_controls(*current, *published_route_plan_);
    if (!edges)
      return fail(current->revision, edges.error());
    (void)publisher_.collect_retired();
    auto offered =
        publisher_.prepare_mix_overlay(current->revision, controls.value(), edges.value());
    if (offered) {
      // The source bank and every processor remain active. Only the post-insert
      // controls and audible revision change at the next callback boundary.
      published_snapshot_ = current;
      refresh_effect_key_notes();
      return Result<void>::success();
    }
    // Revision-sensitive VST3 owners or channel automation retain the normal
    // guarded graph path. Its status still distinguishes saved from audible.
  }
  if (published_snapshot_ && published_route_plan_ && published_snapshot_audible() &&
      !pending_native_revision_ && send_gains_only(*published_snapshot_, *current)) {
    auto edges = routing::project_edge_controls(*current, *published_route_plan_);
    if (!edges)
      return fail(current->revision, edges.error());
    (void)publisher_.collect_retired();
    auto offered = publisher_.prepare_edge_overlay(current->revision, edges.value());
    if (offered) {
      published_snapshot_ = current;
      refresh_effect_key_notes(); // A key at zero gain keys no notes.
      return Result<void>::success();
    }
    // A bound automation edge or a revision-sensitive processor follows the
    // guarded route path, with its saved-versus-audible status preserved.
  }
  auto fast_synth = try_queue_live_synth(current);
  if (!fast_synth)
    return fail(current->revision, fast_synth.error());
  if (fast_synth.value())
    return Result<void>::success();
  auto fast_players = try_queue_live_players(current);
  if (!fast_players)
    return fail(current->revision, fast_players.error());
  if (fast_players.value())
    return Result<void>::success();
  if (published_snapshot_ && native_synth_automation_changed(*published_snapshot_, *current)) {
    if (pending_native_revision_)
      return defer();
    return fail(current->revision,
                {ErrorCode::unsupported, "Live synth automation must be an isolated lane edit; "
                                         "other saved changes need a restart"});
  }
  auto fast_effect_automation = try_queue_live_effect_automation(current);
  if (!fast_effect_automation)
    return fail(current->revision, fast_effect_automation.error());
  if (fast_effect_automation.value())
    return Result<void>::success();
  auto fast_master = try_queue_live_master_native(current);
  if (!fast_master)
    return fail(current->revision, fast_master.error());
  if (fast_master.value())
    return Result<void>::success();
  auto fast_native = try_queue_live_native(current);
  if (!fast_native)
    return fail(current->revision, fast_native.error());
  if (fast_native.value())
    return Result<void>::success();
  auto fast_group = try_queue_live_effect_group(current);
  if (!fast_group)
    return fail(current->revision, fast_group.error());
  if (fast_group.value())
    return Result<void>::success();
  if (pending_native_revision_)
    return defer();
  return begin_current();
}

Result<void> RoutedPlaybackControl::retry() {
  if (revisions_.snapshot().application != PlaybackApplication::failed ||
      revisions_.snapshot().model_revision != model_.snapshot()->revision) {
    return rejected(ErrorCode::conflict, "Only the failed current routed revision can be retried");
  }
  (void)revisions_.retry_current_revision();
  return apply_current();
}

void RoutedPlaybackControl::observe() {
  OSCILLATE_TRACE_SPAN("edit.observe");
  settle_live_candidate();
  (void)publisher_.collect_retired();
  // A group is applied when its slowest stage is.
  const auto group_applied = [this] {
    std::uint64_t applied = std::numeric_limits<std::uint64_t>::max();
    for (const auto& member : pending_native_revision_->group)
      applied = std::min(applied, std::max(member.stage->applied_values_revision(),
                                           member.stage->applied_automation_revision()));
    return applied;
  };
  // An instrument edit is applied when its slowest player is.
  const auto players_applied = [this] {
    std::uint64_t applied = std::numeric_limits<std::uint64_t>::max();
    for (const auto* player : pending_players_)
      applied = std::min(applied, player->applied_plan_revision());
    for (const auto& probe : pending_vst3_)
      applied = std::min(applied, probe());
    return applied;
  };
  if (pending_native_revision_ && !pending_native_revision_->group.empty()) {
    if (group_applied() >= pending_native_revision_->snapshot->revision) {
      // The running stages now hold the offered settings, so their saved
      // identities follow.
      bool chains = true;
      for (const auto& member : pending_native_revision_->group) {
        const auto track = std::ranges::find(pending_native_revision_->snapshot->tracks,
                                             member.track, &project::Track::id);
        auto* running = active_stage(member.track, member.slot);
        if (!running || track == pending_native_revision_->snapshot->tracks.end()) {
          chains = false;
          continue;
        }
        const auto effect =
            std::ranges::find(track->effects, member.slot, &project::EffectInsert::id);
        if (effect == track->effects.end()) {
          chains = false;
          continue;
        }
        running->saved.processor = effect->processor;
      }
      if (chains) {
        published_snapshot_ = std::move(pending_native_revision_->snapshot);
        (void)revisions_.observe_active_graph(published_snapshot_->revision);
      } else {
        (void)revisions_.fail_revision(
            pending_native_revision_->snapshot->revision,
            {ErrorCode::internal, "Applied linked revision lost an active track chain"});
      }
      pending_native_revision_.reset();
    }
  } else if (pending_native_revision_ &&
             std::visit(
                 [&players_applied](auto* runtime) {
                   using Runtime = std::remove_pointer_t<decltype(runtime)>;
                   if constexpr (std::is_same_v<Runtime, native_dsp::synth::NativeSynthRuntime>)
                     return runtime->applied_revision();
                   else if constexpr (std::is_same_v<Runtime, native_inserts::NativeSynthPlayer>)
                     return players_applied();
                   else if constexpr (std::is_same_v<Runtime, native_inserts::NativeEffectStage>)
                     // A revision offers either values or lanes; the other stays older.
                     return std::max(runtime->applied_values_revision(),
                                     runtime->applied_automation_revision());
                   else
                     return runtime->applied_prepared_revision();
                 },
                 pending_native_revision_->runtime) >=
                 pending_native_revision_->snapshot->revision) {
    if (std::holds_alternative<native_dsp::synth::NativeSynthRuntime*>(
            pending_native_revision_->runtime) ||
        std::holds_alternative<native_inserts::NativeSynthPlayer*>(
            pending_native_revision_->runtime)) {
      published_snapshot_ = std::move(pending_native_revision_->snapshot);
      source_snapshot_ = published_snapshot_;
      pending_players_.clear();
      pending_vst3_.clear();
      pending_vst3_tracks_.clear();
      (void)revisions_.observe_active_graph(published_snapshot_->revision);
      // A Lock switch, a followed track's values or notes, or notes an
      // effect reads ahead (Pump's MIDI trigger) changed.
      follow_published_notes();
    } else {
      if (auto* chain = active_chain(pending_native_revision_->track)) {
        // The running stages now hold the offered values, so their saved
        // identities follow. A lane-only effect edit carries no slots.
        for (const auto& effect : pending_native_revision_->effects) {
          for (auto& stage : chain->chain.stages) {
            if (stage.saved.slot == effect.id)
              stage.saved.processor = effect.processor;
          }
        }
        published_snapshot_ = std::move(pending_native_revision_->snapshot);
        (void)revisions_.observe_active_graph(published_snapshot_->revision);
      } else {
        (void)revisions_.fail_revision(
            pending_native_revision_->snapshot->revision,
            {ErrorCode::internal, "Applied native revision lost its active track chain"});
      }
    }
    pending_native_revision_.reset();
  }
  (void)revisions_.observe_active_graph(publisher_.active_revision());
  // A deferred edit applies the newest saved revision once the earlier
  // switch has settled; it may wait again for another.
  // Only the revision that waited, while it is still the current, pending one.
  const auto status = revisions_.snapshot();
  if (deferred_revision_ && (*deferred_revision_ != model_.snapshot()->revision ||
                             status.model_revision != *deferred_revision_ ||
                             status.application != PlaybackApplication::pending))
    deferred_revision_.reset(); // Superseded or failed: nothing waits any more.
  if (deferred_revision_ && *deferred_revision_ == model_.snapshot()->revision &&
      status.model_revision == *deferred_revision_ &&
      status.application == PlaybackApplication::pending && !publisher_.has_pending() &&
      !pending_manual_)
    (void)apply_current();
}

} // namespace oscillate::application
