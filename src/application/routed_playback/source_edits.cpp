#include "oscillate/application/routed_playback_control.hpp"

#include "edit_rules.hpp"

#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/native_inserts/instrument_binding.hpp"
#include "oscillate/native_inserts/native_stage.hpp"
#include "oscillate/track_chain/track_chain.hpp"

#include <algorithm>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// Source edits: an edit that needs prepared sources is captured, prepared
// off the control thread and published or restarted at a primed block.
namespace oscillate::application {
namespace {
using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;
using routed_playback::audio_clip_edit_only;
using routed_playback::needs_whole_switch;
using routed_playback::rejected;
using routed_playback::same_sources;
using routed_playback::same_timing;

// The routed source layout of `snapshot`. It depends on the tracks and their
// routes, never on the stages' latencies, so receipts stand in for stages
// that are not prepared yet.
[[nodiscard]] Result<routing::ProjectRoutePlan>
layout_route(routing::ProjectRouteIdRegistry& registry, const project::Snapshot& snapshot,
             std::uint32_t max_compensation_frames) {
  std::vector<routing::BoundStage> stages;
  for (const auto& track : snapshot.tracks) {
    for (const auto& stage : track_chain::saved_chain(track)) {
      routing::BoundStage bound{track.id, stage.slot};
      bound.detector = true;
      if (const auto* plugin = std::get_if<project::Vst3PluginInstance>(&stage.processor)) {
        bound.processor = routing::StageProcessor::vst3;
        bound.plugin = plugin->id;
      } else if (const auto* patch = std::get_if<project::NativeEffectPatch>(&stage.processor)) {
        bound.generation = patch->generation;
      }
      stages.push_back(bound);
    }
  }
  return registry.compile(snapshot, max_compensation_frames, stages);
}

[[nodiscard]] bool source_overlay_clip_only(const project::Snapshot& before,
                                            const project::Snapshot& after) {
  const auto stable_native_chain = [](std::span<const project::EffectInsert> effects) {
    return std::ranges::all_of(effects, [](const auto& insert) {
      return std::holds_alternative<project::NativeEffectPatch>(insert.processor);
    });
  };
  // This bounded source-only plan shares the active mixer and its processor
  // owners. Source bank order is the unchanged track order. A single exact
  // clip partition may add one source view inside an existing audio slot.
  if (!audio_clip_edit_only(before, after) || before.tracks.empty() || before.tracks.size() > 4 ||
      before.sources.empty() || before.sources.size() > 16 ||
      !stable_native_chain(before.master_effects))
    return false;
  std::size_t clip_count = 0;
  std::size_t changed_clips = 0;
  bool changed = false;
  bool split = false;
  for (std::size_t index = 0; index < before.tracks.size(); ++index) {
    const auto& old_track = before.tracks[index];
    const auto& new_track = after.tracks[index];
    if (old_track.kind != project::TrackKind::audio ||
        new_track.clips.size() < old_track.clips.size() ||
        new_track.clips.size() > old_track.clips.size() + 1 ||
        !stable_native_chain(old_track.effects) || !old_track.automation.empty() ||
        !old_track.sends.empty() || old_track.main_output || !new_track.automation.empty() ||
        !new_track.sends.empty() || new_track.main_output)
      return false;
    clip_count += new_track.clips.size();
    if (clip_count > 16)
      return false;
    const bool split_track = new_track.clips.size() == old_track.clips.size() + 1;
    if (split_track && split)
      return false;
    const project::Clip* original_split = nullptr;
    const project::Clip* left_split = nullptr;
    for (const auto& original : old_track.clips) {
      const auto found = std::ranges::find(new_track.clips, original.id, &project::Clip::id);
      if (found == new_track.clips.end() || original.source != found->source ||
          !original.automation.empty() || !found->automation.empty() ||
          original.processing.requires_rendering() || found->processing.requires_rendering() ||
          original.processing != found->processing || original.fades != found->fades)
        return false;
      const auto source =
          std::ranges::find(before.sources, original.source, &project::SourceRecord::id);
      if (source == before.sources.end() || source->encoding != project::SourceEncoding::wav ||
          source->external_link || source->sample_rate != before.sample_rate)
        return false;
      const bool geometry_changed =
          original.placement != found->placement || original.source_range != found->source_range;
      changed_clips += geometry_changed ? 1U : 0U;
      if (split_track && geometry_changed) {
        if (original_split)
          return false;
        original_split = &original;
        left_split = &*found;
      }
      changed |= geometry_changed;
    }
    if (split_track) {
      // Source bank topology is unchanged: one existing clip becomes two
      // contiguous views of the same managed frames in the same audio slot.
      // Keep this separate from general clip insertion or duplication.
      if (!original_split || !left_split || original_split->fades != project::ClipFades{} ||
          !original_split->automation.empty())
        return false;
      const auto new_clip = std::ranges::find_if(new_track.clips, [&](const auto& clip) {
        return std::ranges::none_of(old_track.clips,
                                    [&](const auto& old) { return old.id == clip.id; });
      });
      if (new_clip == new_track.clips.end() || new_clip->source != original_split->source ||
          new_clip->fades != original_split->fades ||
          new_clip->processing != original_split->processing ||
          new_clip->processing.requires_rendering() || !new_clip->automation.empty())
        return false;
      const auto* original_time = std::get_if<project::ElapsedInterval>(&original_split->placement);
      const auto* left_time = std::get_if<project::ElapsedInterval>(&left_split->placement);
      const auto* right_time = std::get_if<project::ElapsedInterval>(&new_clip->placement);
      if (!original_time || !left_time || !right_time || left_time->start != original_time->start ||
          left_time->end != right_time->start || right_time->end != original_time->end ||
          left_split->source_range.start != original_split->source_range.start ||
          left_split->source_range.end != new_clip->source_range.start ||
          new_clip->source_range.end != original_split->source_range.end ||
          original_time->start.value < 0 ||
          original_time->end.value - original_time->start.value !=
              original_split->source_range.end.value - original_split->source_range.start.value ||
          left_time->end.value - left_time->start.value !=
              left_split->source_range.end.value - left_split->source_range.start.value ||
          right_time->end.value - right_time->start.value !=
              new_clip->source_range.end.value - new_clip->source_range.start.value)
        return false;
      split = true;
    }
  }
  return changed && (!split || changed_clips == 1);
}

} // namespace

Result<RoutedSourcePreparationCapture>
RoutedPlaybackControl::capture_source_edit(const project::AppliedRevision& applied,
                                           std::string_view command_label) {
  observe();
  const auto current = model_.snapshot();
  if (applied.snapshot != current || current->revision <= revisions_.snapshot().model_revision) {
    return Result<RoutedSourcePreparationCapture>::failure(
        {ErrorCode::conflict, "Routed source edit was superseded"});
  }
  (void)revisions_.accept_model_revision(
      current->revision, ChangeDescriptor{std::string(command_label), applied.changes});
  preparing_source_.reset();
  deferred_revision_.reset();
  const auto rejected_capture = [&](Error error) {
    (void)fail(current->revision, error);
    return Result<RoutedSourcePreparationCapture>::failure(std::move(error));
  };
  if (publisher_.active_revision() == 0 || publisher_.has_pending() || pending_native_revision_ ||
      pending_manual_ || !published_snapshot_audible()) {
    return rejected_capture(
        {ErrorCode::conflict, "Routed source edit needs an active, settled graph"});
  }
  const bool clips = audio_clip_edit_only(*published_snapshot_, *current);
  const bool structural = !clips && needs_whole_switch(*published_snapshot_, *current);
  if (!clips && !structural) {
    return rejected_capture(
        {ErrorCode::unsupported, "Live source preparation requires an isolated source edit"});
  }
  try {
    RoutedSourcePreparationCapture capture{
        current,
        sources_,
        topological_order_,
        limits_.max_frames,
        publisher_.active_revision(),
        clips && source_overlay_clip_only(*published_snapshot_, *current),
        {},
        structural};
    if (structural) {
      auto layout = layout_route(registry_, *current, limits_.latency.max_compensation_frames);
      if (!layout)
        return rejected_capture(layout.error());
      capture.source_layout = std::move(layout.value().sources);
      capture.topological_order = std::move(layout.value().latency.topological_order);
      auto kept = keep_players(*current);
      if (!kept)
        return rejected_capture(kept.error());
      capture.kept_players = std::move(kept).value();
    } else {
      // Every built-in instrument the edit leaves alone plays on in the
      // replacement source, voices and all.
      for (const auto& track : current->tracks) {
        const auto before =
            std::ranges::find(published_snapshot_->tracks, track.id, &project::Track::id);
        if (live_source_ == nullptr || before == published_snapshot_->tracks.end() ||
            !(*before == track))
          continue;
        if (auto player = live_source_->shared_synth_player(track.id))
          capture.kept_players.push_back({track.id, std::move(player)});
      }
    }
    preparing_source_ = current;
    return Result<RoutedSourcePreparationCapture>::success(std::move(capture));
  } catch (const std::bad_alloc&) {
    return rejected_capture(
        {ErrorCode::resource_exhausted, "Routed source preparation capture ran out of memory"});
  }
}

bool RoutedPlaybackControl::can_capture_source_edit(const project::Snapshot& candidate) const {
  return !pending_manual_ && published_snapshot_audible() &&
         (audio_clip_edit_only(*published_snapshot_, candidate) ||
          needs_whole_switch(*published_snapshot_, candidate));
}

Result<std::vector<KeptPlayer>> RoutedPlaybackControl::keep_players(const project::Snapshot& next) {
  using Kept = Result<std::vector<KeptPlayer>>;
  std::vector<KeptPlayer> kept;
  if (live_source_ == nullptr)
    return Kept::success(std::move(kept));
  const auto& published = *published_snapshot_;
  const bool timing = !same_timing(published, next);
  auto midi = engine::compile_midi_events(next);
  if (!midi)
    return Kept::failure(midi.error());
  auto automation = automation_project::prepare_project_automation(next, next.revision);
  if (!automation)
    return Kept::failure(automation.error());
  auto limit = automation_sample_limit(next, limits_.max_frames);
  if (!limit)
    return Kept::failure(limit.error());
  // Every plan is built before any player gets one, so a failure leaves
  // them all as they were.
  std::vector<std::pair<std::size_t, native_inserts::NativeSynthPlayer::PreparedPlan>> plans;
  for (const auto& before : published.tracks) {
    auto player = live_source_->shared_synth_player(before.id);
    const auto after = std::ranges::find(next.tracks, before.id, &project::Track::id);
    // A removed track fades out with its node.
    if (!player || after == next.tracks.end())
      continue;
    if (after->instrument_source == before.instrument_source &&
        player->same_note_effects(after->note_effects)) {
      if (*after == before && !timing) {
        kept.push_back({before.id, std::move(player)});
        continue;
      }
      // The same instrument plays on with what the edit changed.
      const engine::InstrumentEvents empty{before.id, {}};
      const auto events = std::ranges::find(midi.value().instruments(), before.id,
                                            &engine::InstrumentEvents::track);
      auto plan = player->prepare_plan(next, *after,
                                       events == midi.value().instruments().end() ? empty : *events,
                                       automation.value(), limit.value());
      if (!plan)
        return Kept::failure(plan.error());
      plans.emplace_back(kept.size(), std::move(plan).value());
      kept.push_back({before.id, std::move(player), next.revision});
      continue;
    }
    // A new instrument: the old one releases its notes and rings out, with
    // the Lock it had.
    auto silent = before;
    silent.automation.clear();
    silent.midi_clips.clear();
    const engine::InstrumentEvents none{before.id, {}};
    auto plan = player->prepare_plan(next, silent, none, {}, limit.value());
    if (!plan)
      return Kept::failure(plan.error());
    plans.emplace_back(kept.size(), std::move(plan).value());
    kept.push_back({before.id, std::move(player), next.revision, true});
  }
  if (!std::ranges::all_of(
          plans, [&kept](const auto& plan) { return kept[plan.first].player->has_room(); }))
    return Kept::failure(
        {ErrorCode::conflict, "A live synth plan mailbox is full; edit again to apply it"});
  for (auto& [index, plan] : plans)
    (void)kept[index].player->stage_plan(std::move(plan));
  return Kept::success(std::move(kept));
}

Result<void> RoutedPlaybackControl::publish_captured_source_overlay(
    const RoutedSourcePreparationCapture& capture) {
  observe();
  const auto current = model_.snapshot();
  if (!capture.source_overlay_eligible || current != capture.snapshot ||
      current != preparing_source_ || revisions_.snapshot().model_revision != current->revision ||
      revisions_.snapshot().application != PlaybackApplication::pending ||
      publisher_.active_revision() != capture.active_graph_revision ||
      !same_sources(sources_, capture.source_layout) ||
      !std::ranges::equal(topological_order_, capture.topological_order) ||
      limits_.max_frames != capture.max_frames || publisher_.has_pending() ||
      pending_native_revision_ || pending_manual_ ||
      !source_overlay_clip_only(*published_snapshot_, *current))
    return rejected(ErrorCode::conflict, "Routed source overlay capture is stale or busy");
  (void)publisher_.collect_retired();
  auto offered = publisher_.prepare_source_overlay(current->revision);
  if (!offered)
    return fail(current->revision, offered.error());
  published_snapshot_ = current;
  source_snapshot_ = current;
  preparing_source_.reset();
  return Result<void>::success();
}

Result<void>
RoutedPlaybackControl::publish_captured_source(const RoutedSourcePreparationCapture& capture) {
  observe();
  const auto current = model_.snapshot();
  if (current != preparing_source_ && current->revision > revisions_.snapshot().model_revision) {
    preparing_source_.reset();
    (void)revisions_.accept_model_revision(current->revision,
                                           ChangeDescriptor{"project refresh", {}});
    return fail(current->revision, {ErrorCode::conflict,
                                    "Routed source preparation was superseded by a project edit"});
  }
  if (current != preparing_source_ || current != capture.snapshot ||
      revisions_.snapshot().model_revision != current->revision ||
      revisions_.snapshot().application != PlaybackApplication::pending ||
      publisher_.active_revision() != capture.active_graph_revision ||
      (!capture.structural &&
       (!same_sources(sources_, capture.source_layout) ||
        !std::ranges::equal(topological_order_, capture.topological_order))) ||
      limits_.max_frames != capture.max_frames || publisher_.has_pending() ||
      pending_native_revision_ || pending_manual_) {
    return rejected(ErrorCode::conflict, "Routed source preparation capture is stale or busy");
  }
  (void)publisher_.collect_retired();
  preparing_source_.reset();
  return build_and_publish(current, true, &capture);
}

Result<void>
RoutedPlaybackControl::restart_captured_source(const RoutedSourcePreparationCapture& capture) {
  observe();
  const auto current = model_.snapshot();
  const auto status = revisions_.snapshot();
  if (current != capture.snapshot || status.model_revision != current->revision ||
      status.application != PlaybackApplication::failed || publisher_.has_pending() ||
      publisher_.active_revision() != capture.active_graph_revision)
    return rejected(ErrorCode::conflict, "Routed source capture cannot be primed again");
  (void)revisions_.retry_current_revision();
  preparing_source_ = current;
  return Result<void>::success();
}

Result<void>
RoutedPlaybackControl::fail_captured_source_edit(const RoutedSourcePreparationCapture& capture,
                                                 Error error) {
  const auto current = model_.snapshot();
  if (current != capture.snapshot || current != preparing_source_ ||
      revisions_.snapshot().model_revision != current->revision ||
      revisions_.snapshot().application != PlaybackApplication::pending) {
    return rejected(ErrorCode::conflict, "Routed source preparation failure is stale");
  }
  preparing_source_.reset();
  return fail(current->revision, std::move(error));
}

} // namespace oscillate::application
