#include "oscillate/application/routed_playback_control.hpp"

#include "edit_rules.hpp"

#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "oscillate/native_inserts/native_stage.hpp"
#include "oscillate/routing/route_gain_blocks.hpp"

#include <algorithm>
#include <cmath>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// Live controls on the published graph: VST3 manual edits, channel and
// send levels, synth values, effect telemetry and meters.
namespace oscillate::application {
namespace {
using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;
using routed_playback::rejected;
using routed_playback::same_timing;

template <typename TrackRef>
[[nodiscard]] auto* saved_vst3(TrackRef& track, project::PluginInstanceId plugin) {
  auto* found = std::get_if<project::Vst3PluginInstance>(&track.instrument_source);
  if (found && found->id == plugin)
    return found;
  for (auto& effect : track.effects) {
    found = std::get_if<project::Vst3PluginInstance>(&effect.processor);
    if (found && found->id == plugin)
      return found;
  }
  return decltype(found){nullptr};
}

// The saved plugin on `track`'s chain, or on the master's with no track.
[[nodiscard]] const project::Vst3PluginInstance* saved_vst3(const project::Snapshot& snapshot,
                                                            std::optional<project::TrackId> track,
                                                            project::PluginInstanceId plugin) {
  if (track) {
    const auto owner = std::ranges::find(snapshot.tracks, *track, &project::Track::id);
    return owner == snapshot.tracks.end() ? nullptr : saved_vst3(*owner, plugin);
  }
  for (const auto& effect : snapshot.master_effects) {
    const auto* found = std::get_if<project::Vst3PluginInstance>(&effect.processor);
    if (found && found->id == plugin)
      return found;
  }
  return nullptr;
}

// True when after differs from before only in one saved VST3 plugin's opaque
// component state and parameter references. Identity, slot, enablement,
// automation and every other track or project field must be unchanged.
[[nodiscard]] bool manual_vst3_state_only(const project::Snapshot& before,
                                          const project::Snapshot& after,
                                          project::PluginInstanceId plugin) {
  if (before.id != after.id || before.sample_rate != after.sample_rate ||
      !same_timing(before, after) || before.sources != after.sources ||
      before.master_effects != after.master_effects || before.master_level != after.master_level ||
      before.source_tempo_reviews != after.source_tempo_reviews ||
      before.source_key_reviews != after.source_key_reviews ||
      before.next_insert_slot_serial != after.next_insert_slot_serial ||
      before.tracks.size() != after.tracks.size())
    return false;
  bool changed = false;
  for (std::size_t index = 0; index < before.tracks.size(); ++index) {
    const auto& old_track = before.tracks[index];
    const auto& next_track = after.tracks[index];
    if (old_track == next_track)
      continue;
    const auto* old_plugin = saved_vst3(old_track, plugin);
    const auto* next_plugin = saved_vst3(next_track, plugin);
    if (changed || !old_plugin || !next_plugin)
      return false;
    auto restored = next_track;
    auto* target = saved_vst3(restored, plugin);
    target->component_state = old_plugin->component_state;
    target->parameters = old_plugin->parameters;
    if (restored != old_track)
      return false;
    changed = true;
  }
  return changed;
}

} // namespace

std::optional<project::TrackId> RoutedPlaybackControl::bound_instrument_track() const noexcept {
  const auto found = std::ranges::find_if(
      vst3_stages_, [](const Vst3Stage& stage) { return stage.track && !stage.slot; });
  if (found != vst3_stages_.end())
    return found->track;
  return std::nullopt;
}

bool RoutedPlaybackControl::has_bound_instrument_track(project::TrackId track) const noexcept {
  if (!published_snapshot_)
    return false;
  const auto owner = std::ranges::find(published_snapshot_->tracks, track, &project::Track::id);
  if (owner == published_snapshot_->tracks.end())
    return false;
  const auto* saved = std::get_if<project::Vst3PluginInstance>(&owner->instrument_source);
  return saved && std::ranges::any_of(vst3_stages_, [track, saved](const Vst3Stage& stage) {
           return stage.track == track && !stage.slot && stage.saved == *saved;
         });
}

std::optional<ActiveVst3EffectManualEvents> RoutedPlaybackControl::active_vst3_effect_manual_events(
    project::TrackId track, project::InsertSlotId insert, project::PluginInstanceId plugin,
    std::uint64_t graph_revision) const {
  if (graph_revision == 0 || graph_revision != publisher_.active_revision() ||
      graph_handoff_in_progress())
    return std::nullopt;
  const auto found = std::ranges::find_if(vst3_stages_, [&](const Vst3Stage& candidate) {
    return candidate.track == track && candidate.slot == insert && candidate.saved.id == plugin;
  });
  if (found == vst3_stages_.end() || !found->prepared.manual_events ||
      !found->prepared.active_manual_graph ||
      (found->prepared.process_failed && found->prepared.process_failed()))
    return std::nullopt;

  // The model may have advanced while the previous render graph continues.
  // Never hand out that graph's slot for a changed saved insert.
  const auto current = model_.snapshot();
  const auto owner = std::ranges::find(current->tracks, track, &project::Track::id);
  if (owner == current->tracks.end())
    return std::nullopt;
  const auto effect = std::ranges::find(owner->effects, insert, &project::EffectInsert::id);
  const auto* saved = effect == owner->effects.end()
                          ? nullptr
                          : std::get_if<project::Vst3PluginInstance>(&effect->processor);
  if (!saved || !effect->enabled || saved->missing || *saved != found->saved)
    return std::nullopt;
  const auto identity = found->prepared.active_manual_graph();
  if (identity.graph_revision != graph_revision || identity.instance_id == 0 ||
      identity.compatibility_key == 0)
    return std::nullopt;
  return ActiveVst3EffectManualEvents{found->prepared.manual_events, identity};
}

std::optional<ActiveVst3InstrumentManualEvents>
RoutedPlaybackControl::active_vst3_instrument_manual_events(project::TrackId track,
                                                            project::PluginInstanceId plugin,
                                                            std::uint64_t graph_revision) const {
  if (graph_revision == 0 || graph_revision != publisher_.active_revision() ||
      graph_handoff_in_progress())
    return std::nullopt;
  const auto found = std::ranges::find_if(vst3_stages_, [&](const Vst3Stage& candidate) {
    return candidate.track == track && !candidate.slot && candidate.saved.id == plugin;
  });
  if (found == vst3_stages_.end() || !found->prepared.manual_events ||
      !found->prepared.active_manual_graph ||
      (found->prepared.process_failed && found->prepared.process_failed()))
    return std::nullopt;

  const auto current = model_.snapshot();
  const auto owner = std::ranges::find(current->tracks, track, &project::Track::id);
  if (owner == current->tracks.end() || owner->kind != project::TrackKind::instrument)
    return std::nullopt;
  const auto* saved = std::get_if<project::Vst3PluginInstance>(&owner->instrument_source);
  if (!saved || saved->missing || *saved != found->saved)
    return std::nullopt;
  const auto identity = found->prepared.active_manual_graph();
  if (identity.graph_revision != graph_revision || identity.instance_id == 0 ||
      identity.compatibility_key == 0)
    return std::nullopt;
  return ActiveVst3InstrumentManualEvents{found->prepared.manual_events, identity};
}

std::optional<PendingVst3ManualPreviewEvents>
RoutedPlaybackControl::pending_vst3_manual_preview_events(
    project::TrackId track, std::optional<project::InsertSlotId> insert,
    project::PluginInstanceId plugin) const {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  if (!pending_manual_ || pending_manual_->plugin != plugin ||
      pending_manual_->snapshot != model_.snapshot() || publisher_.has_pending() ||
      pending_native_revision_ || publisher_.active_revision() == 0)
    return std::nullopt;
  const auto found = std::ranges::find_if(vst3_stages_, [&](const Vst3Stage& candidate) {
    return candidate.track == track && candidate.slot == insert && candidate.saved.id == plugin;
  });
  if (found == vst3_stages_.end() || !found->prepared.manual_events ||
      !found->prepared.active_manual_graph ||
      (found->prepared.process_failed && found->prepared.process_failed()))
    return std::nullopt;
  const auto identity = found->prepared.active_manual_graph();
  if (identity.graph_revision != pending_manual_->identity.graph_revision ||
      identity.instance_id != pending_manual_->identity.instance_id ||
      identity.compatibility_key != pending_manual_->identity.compatibility_key ||
      identity.graph_revision != publisher_.active_revision())
    return std::nullopt;
  const auto receipt = found->prepared.manual_events->receipt();
  if (receipt.revision != pending_manual_->snapshot->revision)
    return std::nullopt;
  const auto phase = receipt.phase;
  if (phase != plugins::Vst3ManualEventPhase::pending &&
      phase != plugins::Vst3ManualEventPhase::processing &&
      phase != plugins::Vst3ManualEventPhase::applied)
    return std::nullopt;
  return PendingVst3ManualPreviewEvents{found->prepared.manual_events, identity};
#else
  (void)track;
  (void)insert;
  (void)plugin;
  return std::nullopt;
#endif
}

Result<const RoutedPlaybackControl::Vst3Stage*>
RoutedPlaybackControl::manual_slot_ready(project::PluginInstanceId plugin,
                                         plugins::Vst3ActiveGraphIdentity expected) const {
  const auto refuse = [](ErrorCode code, const char* message) {
    return Result<const Vst3Stage*>::failure({code, message});
  };
  const auto status = revisions_.snapshot();
  // Audible state must equal the latest saved revision. After an unapplied
  // manual value the audible plugin lacks it, so a later event cannot make
  // the newer saved state truthfully audible.
  if (graph_handoff_in_progress() || pending_native_revision_ || !published_snapshot_ ||
      publisher_.active_revision() == 0 || status.application != PlaybackApplication::active ||
      status.active_graph_revision != status.model_revision ||
      published_snapshot_->revision != status.active_graph_revision)
    return refuse(ErrorCode::conflict, "Live VST3 parameter needs a settled audible revision");
  const auto found = std::ranges::find_if(
      vst3_stages_, [&](const Vst3Stage& stage) { return stage.saved.id == plugin; });
  if (found == vst3_stages_.end() || !found->prepared.manual_events ||
      !found->prepared.active_manual_graph ||
      (found->prepared.process_failed && found->prepared.process_failed()))
    return refuse(ErrorCode::unavailable, "VST3 plugin has no active qualified manual slot");
  const auto identity = found->prepared.active_manual_graph();
  if (expected.graph_revision != publisher_.active_revision() || identity.instance_id == 0 ||
      identity.graph_revision != expected.graph_revision ||
      identity.instance_id != expected.instance_id ||
      identity.compatibility_key != expected.compatibility_key)
    return refuse(ErrorCode::conflict, "VST3 manual edit names a stale active instance");
  const auto* audible = saved_vst3(*published_snapshot_, found->track, plugin);
  if (!audible || *audible != found->saved)
    return refuse(ErrorCode::conflict, "Audible VST3 state differs from its active plugin");
  return Result<const Vst3Stage*>::success(&*found);
}

bool RoutedPlaybackControl::published_snapshot_audible() const {
  // The callback-accepted topology carries the published snapshot, or an
  // acknowledged live parameter revision advanced it past that topology.
  return published_snapshot_ &&
         published_snapshot_->revision ==
             std::max(publisher_.active_revision(), revisions_.snapshot().active_graph_revision);
}

Result<void>
RoutedPlaybackControl::preflight_vst3_manual_edit(project::PluginInstanceId plugin,
                                                  plugins::Vst3ActiveGraphIdentity expected) const {
  if (model_.snapshot()->revision != revisions_.snapshot().model_revision)
    return rejected(ErrorCode::conflict, "Routed playback has not accepted the saved revision");
  auto ready = manual_slot_ready(plugin, expected);
  if (!ready)
    return Result<void>::failure(ready.error());
  return Result<void>::success();
}

Result<void> RoutedPlaybackControl::begin_vst3_manual_edit(
    const project::AppliedRevision& applied, std::string_view command_label,
    project::PluginInstanceId plugin, plugins::Vst3ActiveGraphIdentity expected) {
  observe();
  const auto current = model_.snapshot();
  if (applied.snapshot != current || current->revision <= revisions_.snapshot().model_revision)
    return rejected(ErrorCode::conflict, "Routed VST3 manual edit was superseded");
  // Readiness describes the previous saved revision, so check it first.
  auto ready = manual_slot_ready(plugin, expected);
  (void)revisions_.accept_model_revision(
      current->revision, ChangeDescriptor{std::string(command_label), applied.changes});
  preparing_source_.reset();
  deferred_revision_.reset();
  if (!ready)
    return fail(current->revision, ready.error());
  if (!manual_vst3_state_only(*published_snapshot_, *current, plugin))
    return fail(current->revision,
                {ErrorCode::unsupported,
                 "Live VST3 revision changes more than one plugin's parameter state"});
  pending_manual_.emplace(PendingManualRevision{current, plugin, expected});
  return Result<void>::success();
}

Result<void>
RoutedPlaybackControl::settle_vst3_manual_edit(plugins::Vst3ManualEventReceipt receipt) {
  if (!pending_manual_ || receipt.revision != pending_manual_->snapshot->revision ||
      (receipt.phase != plugins::Vst3ManualEventPhase::applied &&
       receipt.phase != plugins::Vst3ManualEventPhase::failed))
    return rejected(ErrorCode::conflict, "VST3 manual receipt does not match the pending edit");
  auto pending = std::move(*pending_manual_);
  pending_manual_.reset();
  if (receipt.phase != plugins::Vst3ManualEventPhase::applied || receipt.project_sample < 0)
    return fail(pending.snapshot->revision,
                {ErrorCode::unavailable,
                 "Live VST3 parameter was not applied; the saved revision is not audible"});
  const auto found = std::ranges::find_if(
      vst3_stages_, [&](const Vst3Stage& stage) { return stage.saved.id == pending.plugin; });
  const project::Vst3PluginInstance* saved =
      found == vst3_stages_.end() ? nullptr
                                  : saved_vst3(*pending.snapshot, found->track, pending.plugin);
  if (!saved || !found->prepared.active_manual_graph)
    return fail(pending.snapshot->revision,
                {ErrorCode::internal, "Applied VST3 manual revision lost its active plugin"});
  const auto identity = found->prepared.active_manual_graph();
  if (identity.graph_revision != pending.identity.graph_revision ||
      identity.instance_id != pending.identity.instance_id ||
      identity.compatibility_key != pending.identity.compatibility_key)
    return fail(pending.snapshot->revision,
                {ErrorCode::conflict, "Active VST3 instance changed before its receipt"});
  // The running plugin now holds the saved state, so its chain stage does too.
  found->saved = *saved;
  if (auto* chain = active_chain(found->track)) {
    for (auto& stage : chain->chain.stages) {
      if (stage.runtime == found->prepared.processor)
        stage.saved.processor = *saved;
    }
  }
  published_snapshot_ = std::move(pending.snapshot);
  (void)revisions_.observe_active_graph(published_snapshot_->revision);
  return Result<void>::success();
}

std::optional<engine::RoutedNodeMeterReading>
RoutedPlaybackControl::active_track_meter(project::TrackId track) const noexcept {
  if (!published_route_plan_)
    return std::nullopt;
  const auto route = std::ranges::find(published_route_plan_->track_nodes, track,
                                       &routing::ProjectRouteTrackNode::track);
  if (route == published_route_plan_->track_nodes.end())
    return std::nullopt;
  return publisher_.active_node_meter(route->node);
}

std::optional<engine::RoutedNodeMeterReading>
RoutedPlaybackControl::heard_track_meter(project::TrackId track, std::uint64_t heard_sample,
                                         std::uint64_t generation) const noexcept {
  if (!published_route_plan_)
    return std::nullopt;
  const auto route = std::ranges::find(published_route_plan_->track_nodes, track,
                                       &routing::ProjectRouteTrackNode::track);
  if (route == published_route_plan_->track_nodes.end())
    return std::nullopt;
  return publisher_.active_node_meter_heard(route->node, heard_sample, generation);
}

bool RoutedPlaybackControl::set_live_channel(std::optional<project::TrackId> track, double level,
                                             double pan) noexcept {
  if (!published_route_plan_ || !std::isfinite(level) || !std::isfinite(pan))
    return false;
  if (!track)
    return publisher_.push_live_node_gain(published_route_plan_->master,
                                          {static_cast<float>(level), static_cast<float>(level)});
  const auto route = std::ranges::find(published_route_plan_->track_nodes, *track,
                                       &routing::ProjectRouteTrackNode::track);
  if (route == published_route_plan_->track_nodes.end())
    return false;
  const auto gain = routing::channel_stereo_gain(level, std::clamp(pan, -1.0, 1.0));
  return publisher_.push_live_node_gain(route->node, {gain.left, gain.right});
}

bool RoutedPlaybackControl::set_live_send(project::TrackId track, project::SendId send,
                                          double gain) noexcept {
  if (!published_route_plan_ || !std::isfinite(gain))
    return false;
  const auto route = std::ranges::find_if(published_route_plan_->sends, [&](const auto& item) {
    return item.track == track && item.send == send;
  });
  if (route == published_route_plan_->sends.end())
    return false;
  // A silent key stays silent while its gain is dragged.
  float heard = static_cast<float>(gain);
  if (published_snapshot_) {
    const auto& tracks = published_snapshot_->tracks;
    const auto source = std::ranges::find(tracks, track, &project::Track::id);
    if (source != tracks.end()) {
      const auto saved = std::ranges::find(source->sends, send, &project::SendRoute::id);
      if (saved != source->sends.end() && saved->purpose == project::SendPurpose::sidechain) {
        try {
          if (routing::silent_key_sources(*published_snapshot_).contains(track))
            heard = 0.0F;
        } catch (const std::bad_alloc&) {
          return false;
        }
      }
    }
  }
  return publisher_.push_live_edge_gain(route->edge, heard);
}

bool RoutedPlaybackControl::set_live_synth_values(project::TrackId track,
                                                  const native_dsp::EngineDescriptor& engine,
                                                  std::span<const double> values) noexcept {
  auto* runtime = live_source_ ? live_source_->synth_runtime(track) : nullptr;
  return runtime && runtime->engine().descriptor == &engine && runtime->offer_live_values(values);
}

bool RoutedPlaybackControl::live_note(project::TrackId track, std::uint8_t key,
                                      std::uint8_t velocity, bool on) noexcept {
  auto* player = live_source_ ? live_source_->synth_player(track) : nullptr;
  return player && player->queue_live_note(key, velocity, on);
}

bool RoutedPlaybackControl::set_live_effect_values(std::optional<project::TrackId> track,
                                                   project::InsertSlotId slot,
                                                   const native_dsp::EngineDescriptor& engine,
                                                   std::span<const double> values) noexcept {
  auto* stage = active_effect_stage(track, slot);
  if (!stage || stage->engine().descriptor != &engine)
    return false;
  // Latency is fixed at creation: a latency change waits for the saved
  // edit, which rebuilds the stage.
  const auto latency = stage->engine().latency_frames
                           ? stage->engine().latency_frames(values, stage->sample_rate())
                           : 0U;
  return latency == stage->latency_frames() && stage->offer_live_values(values);
}

native_inserts::NativeEffectStage*
RoutedPlaybackControl::active_effect_stage(std::optional<project::TrackId> track,
                                           project::InsertSlotId insert) const noexcept {
  const auto* chain = active_chain(track);
  if (chain == nullptr)
    return nullptr;
  for (const auto& stage : chain->chain.stages) {
    auto* effect = dynamic_cast<native_inserts::NativeEffectStage*>(stage.runtime.get());
    if (effect && effect->slot() == insert)
      return effect;
  }
  return nullptr;
}

bool RoutedPlaybackControl::active_effect_telemetry(std::optional<project::TrackId> track,
                                                    project::InsertSlotId insert,
                                                    std::vector<float>& frame) {
  auto* effect = active_effect_stage(track, insert);
  return effect != nullptr && effect->read_telemetry(frame);
}

bool RoutedPlaybackControl::set_active_effect_key_listen(project::TrackId track,
                                                         project::InsertSlotId insert,
                                                         bool listen) noexcept {
  auto* effect = active_effect_stage(track, insert);
  if (effect == nullptr)
    return false;
  effect->set_key_listen(listen);
  return true;
}

std::optional<float>
RoutedPlaybackControl::active_effect_key_peak(project::TrackId track,
                                              project::InsertSlotId insert) const noexcept {
  const auto* effect = active_effect_stage(track, insert);
  return effect == nullptr ? std::nullopt : std::optional(effect->key_peak());
}

} // namespace oscillate::application
