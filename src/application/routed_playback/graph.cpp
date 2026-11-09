#include "oscillate/application/routed_playback_control.hpp"

#include "edit_rules.hpp"

#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/engine/chunked_processor.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/foundation/trace.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "oscillate/native_inserts/native_stage.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/routing/route_gain_blocks.hpp"
#include "oscillate/source_prep/synth_media.hpp"
#include "oscillate/track_chain/track_chain.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// Graph preparation: the routed graph, its chains and stages built from a
// snapshot and published to the mixer, and the prepared route's tails.
namespace oscillate::application {
namespace {
using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;
using routed_playback::audio_clip_edit_only;
using routed_playback::rejected;
using routed_playback::runs;
using routed_playback::same_audio_content;
using routed_playback::same_sources;
using routed_playback::same_timing;

[[nodiscard]] std::optional<std::uint64_t>
sum_export_tail(std::optional<std::uint64_t> total, std::optional<std::uint64_t> stage) noexcept {
  if (!total || !stage || *stage > std::numeric_limits<std::uint64_t>::max() - *total)
    return std::nullopt;
  return *total + *stage;
}

// How long a removed stage that cannot bound its tail rings out before the
// mixer fades it (ADR-0050).
constexpr std::uint64_t unbounded_ring_out_seconds = 4;

// A prepared VST3 binding as a chain stage. Only an independently qualified
// tail bounds export; a declared one still bounds a live ring-out.
[[nodiscard]] track_chain::Stage vst3_stage(const track_chain::SavedStage& saved,
                                            const PreparedTrackInsert& prepared) {
  std::optional<std::uint64_t> declared;
  if (prepared.finite_tail_frames)
    declared = *prepared.finite_tail_frames;
  return {saved,
          prepared.processor,
          prepared.latency_frames,
          prepared.fixed_tail_qualified ? declared : std::nullopt,
          declared,
          prepared.routed_sidechain};
}

} // namespace

Result<std::uint64_t> automation_sample_limit(const project::Snapshot& snapshot,
                                              std::size_t max_frames) {
  if (max_frames == 0 || max_frames > engine::max_midi_block_frames)
    return Result<std::uint64_t>::failure(
        {ErrorCode::invalid_argument, "Automation block capacity is invalid"});
  long double peak_bpm = 0.0L;
  for (const auto& event : snapshot.tempo.events())
    peak_bpm = std::max(peak_bpm, static_cast<long double>(event.bpm));
  const long double bound =
      (static_cast<long double>(std::numeric_limits<std::int64_t>::max()) / 4.0L) * 60.0L *
      snapshot.sample_rate / (peak_bpm * static_cast<long double>(timeline::ticks_per_quarter));
  if (!std::isfinite(bound) || bound < static_cast<long double>(max_frames))
    return Result<std::uint64_t>::failure(
        {ErrorCode::unsupported, "Automation tempo exceeds live sample-coordinate headroom"});
  const auto maximum =
      static_cast<long double>(std::numeric_limits<std::int64_t>::max() - max_frames);
  return Result<std::uint64_t>::success(static_cast<std::uint64_t>(std::min(bound, maximum)));
}

Result<void>
RoutedPlaybackControl::build_and_publish(std::shared_ptr<const project::Snapshot> prepared_snapshot,
                                         bool replaces_source,
                                         const RoutedSourcePreparationCapture* capture) {
  const bool structural = replaces_source && capture != nullptr && capture->structural;
  OSCILLATE_TRACE_SPAN("edit.build_and_publish");
  const auto& snapshot = *prepared_snapshot;
  if (limits_.max_frames == 0 || limits_.max_frames > 8192) {
    return fail(snapshot.revision,
                {ErrorCode::invalid_argument, "Routed live block limit must be 1..8192"});
  }
  if (source_snapshot_ && !structural && !same_audio_content(*source_snapshot_, snapshot) &&
      !(replaces_source && published_snapshot_ &&
        audio_clip_edit_only(*published_snapshot_, snapshot))) {
    return fail(snapshot.revision,
                {ErrorCode::unsupported,
                 "Routed live source content changed; prepare and restart playback"});
  }
  auto automation = automation_project::prepare_project_automation(snapshot, snapshot.revision);
  if (!automation)
    return fail(snapshot.revision, automation.error());
  // Every native stage this revision prepares sees the project's tempo and
  // its slot's automation, bounded like the synths' (ADR-0043).
  auto automation_limit = automation_sample_limit(snapshot, limits_.max_frames);
  if (!automation_limit)
    return fail(snapshot.revision, automation_limit.error());
  native_inserts::StageContext stage_context{snapshot.sample_rate};
  stage_context.snapshot = &snapshot;
  stage_context.automation = automation.value();
  stage_context.max_frames = limits_.max_frames;
  stage_context.last_safe_automation_sample = automation_limit.value();
  // A built-in effect's managed media (Cast's impulse response) is decoded
  // from the project's store as its stage is prepared.
  std::optional<persistence::ProjectStore> media_store;
  stage_context.media =
      [&](const project::NativeSynthDocument& document) -> Result<engine::SynthMedia> {
    if (!media_store) {
      if (project_root_.empty())
        return Result<engine::SynthMedia>::failure(
            {ErrorCode::unavailable, "Built-in effect media needs the project's store"});
      auto opened = persistence::ProjectStore::open_reader(project_root_);
      if (!opened)
        return Result<engine::SynthMedia>::failure(opened.error());
      media_store.emplace(std::move(opened).value());
    }
    return source_prep::load_synth_media(*media_store, snapshot, document);
  };
  std::vector<std::pair<project::Vst3ParameterTarget, std::uint64_t>> vst3_automation;
  for (const auto& item : automation.value()) {
    if (const auto* target = std::get_if<project::Vst3ParameterTarget>(&item.target()))
      vst3_automation.emplace_back(*target, item.semantics_key());
  }
  std::ranges::sort(vst3_automation, {}, [](const auto& item) {
    return std::pair{item.first.plugin.bits(), item.first.parameter_id};
  });
  if (publisher_.active_revision() != 0 && vst3_automation != active_vst3_automation_)
    return fail(snapshot.revision, {ErrorCode::unsupported,
                                    "Live VST3 automation changed; restart playback to rebind"});
  if (std::ranges::any_of(automation.value(), [&snapshot](const auto& item) {
        if (std::holds_alternative<project::SendGainTarget>(item.target()))
          return false;
        if (std::holds_alternative<project::Vst3ParameterTarget>(item.target()))
          return false;
        if (std::holds_alternative<project::NativeParameterTarget>(item.target()))
          return false; // Preparation verified the owning built-in synth.
        if (std::holds_alternative<project::NativeEffectParameterTarget>(item.target()))
          return false; // Preparation verified the owning built-in effect slot.
        const auto* control = std::get_if<project::TrackControlTarget>(&item.target());
        return control == nullptr || !project::is_channel_control(control->key);
      }))
    return fail(snapshot.revision, {ErrorCode::unsupported,
                                    "Routed live playback cannot apply this automation target"});
  // Every track and the master run one chain (ADR-0050). A chain whose saved
  // stages are unchanged keeps running as it is; a changed one is prepared
  // again, carrying over each unchanged running stage, and switches live.
  const bool playing = publisher_.active_revision() != 0;
  const bool retimed =
      playing && published_snapshot_ && !same_timing(*published_snapshot_, snapshot);
  const auto unbounded_ring_out = unbounded_ring_out_seconds * snapshot.sample_rate;
  std::vector<ActiveChain> chains_next;
  std::vector<Vst3Stage> vst3_next;
  struct Switched {
    std::optional<project::TrackId> track;
    track_chain::ChainSwitch change;
  };
  std::vector<Switched> switches;
  const auto build_chain = [&](std::optional<project::TrackId> track,
                               std::vector<track_chain::SavedStage> saved,
                               std::optional<project::InsertSlotId> sidechain) -> Result<void> {
    const auto* prior = active_chain(track);
    // A VST3 effect reads the tempo once, when it is prepared: a tempo or
    // meter change replaces it, and the old one rings out (ADR-0101).
    const bool fresh_vst3 =
        retimed && std::ranges::any_of(saved, [](const auto& stage) {
          return stage.slot && std::holds_alternative<project::Vst3PluginInstance>(stage.processor);
        });
    if (prior && prior->sidechain_target == sidechain && runs(prior->chain, saved) && !fresh_vst3) {
      chains_next.push_back(*prior);
      return Result<void>::success();
    }
    if (saved.empty() && !prior)
      return Result<void>::success();
    // A replaced VST3 instrument releases its notes on its drain (ADR-0101).
    track_chain::ChainContext context{stage_context};
    context.detector_slot = sidechain;
    context.fresh_vst3_effects = fresh_vst3;
    if (inserts_) {
      const auto owner = track ? "track " + track->to_string() : std::string("the master");
      context.vst3 = [&,
                      owner](const track_chain::SavedStage& stage) -> Result<track_chain::Stage> {
        const auto& plugin = std::get<project::Vst3PluginInstance>(stage.processor);
        const auto host_frames =
            std::min(static_cast<std::uint32_t>(limits_.max_frames), vst3_host_block_frames);
        auto prepared = inserts_->prepare(snapshot, plugin, host_frames);
        if (!prepared)
          return Result<track_chain::Stage>::failure(
              {prepared.error().code, "VST3 stage on " + owner + ": " + prepared.error().message});
        if (!prepared.value().processor || !prepared.value().process_failed)
          return Result<track_chain::Stage>::failure(
              {ErrorCode::corrupt_data,
               "VST3 provider returned an incomplete processor for " + owner});
        if (prepared.value().instrument_source != !stage.slot)
          return Result<track_chain::Stage>::failure(
              {ErrorCode::unsupported,
               "Verified VST3 processor kind does not match the saved stage on " + owner});
        if (limits_.max_frames > host_frames) {
          auto chunked =
              engine::ChunkedProcessorRuntime::prepare(prepared.value().processor, host_frames);
          if (!chunked)
            return Result<track_chain::Stage>::failure(chunked.error());
          prepared.value().processor = std::move(chunked).value();
        }
        auto result = vst3_stage(stage, prepared.value());
        vst3_next.push_back({track, stage.slot, plugin, std::move(prepared).value()});
        return Result<track_chain::Stage>::success(std::move(result));
      };
    }
    auto prepared = track_chain::prepare_chain(saved, context, prior ? &prior->chain : nullptr);
    if (!prepared)
      return Result<void>::failure(prepared.error());
    if (prior && prior->chain.runtime) {
      switches.push_back(
          {track, track_chain::switch_chain(prior->chain, prepared.value(), unbounded_ring_out)});
    }
    if (prepared.value().runtime)
      chains_next.push_back({track, sidechain, std::move(prepared).value()});
    return Result<void>::success();
  };
  try {
    for (const auto& track : snapshot.tracks) {
      if (track.kind != project::TrackKind::instrument &&
          !std::holds_alternative<std::monostate>(track.instrument_source))
        return fail(snapshot.revision,
                    {ErrorCode::unsupported, "Routed effect path has an instrument source"});
      auto built = build_chain(track.id, track_chain::saved_chain(track),
                               routing::sidechain_target(snapshot, track.id));
      if (!built)
        return fail(snapshot.revision, built.error());
    }
    auto built =
        build_chain(std::nullopt, track_chain::saved_chain(snapshot.master_effects), std::nullopt);
    if (!built)
      return fail(snapshot.revision, built.error());
    // Every running VST3 stage keeps its host record; new ones were recorded
    // as they were prepared.
    for (const auto& chain : chains_next) {
      for (const auto& stage : chain.chain.stages) {
        if (!std::holds_alternative<project::Vst3PluginInstance>(stage.saved.processor) ||
            std::ranges::any_of(vst3_next, [&](const Vst3Stage& record) {
              return record.prepared.processor == stage.runtime;
            }))
          continue;
        const auto kept = std::ranges::find_if(vst3_stages_, [&](const Vst3Stage& record) {
          return record.prepared.processor == stage.runtime;
        });
        if (kept == vst3_stages_.end())
          return fail(snapshot.revision,
                      {ErrorCode::internal, "Running VST3 stage lost its host record"});
        if (kept->prepared.process_failed && kept->prepared.process_failed())
          return fail(snapshot.revision, {ErrorCode::unavailable,
                                          "An active VST3 stage has failed during processing"});
        vst3_next.push_back(*kept);
      }
    }
    std::erase_if(vst3_next, [&](const Vst3Stage& record) {
      return std::ranges::none_of(chains_next, [&](const ActiveChain& chain) {
        return std::ranges::any_of(chain.chain.stages, [&](const track_chain::Stage& stage) {
          return stage.runtime == record.prepared.processor;
        });
      });
    });
  } catch (const std::bad_alloc&) {
    return fail(snapshot.revision,
                {ErrorCode::resource_exhausted, "Chain preparation ran out of memory"});
  }
  const auto chain_of = [&](std::optional<project::TrackId> track) -> const ActiveChain* {
    const auto found = std::ranges::find(chains_next, track, &ActiveChain::track);
    return found == chains_next.end() ? nullptr : &*found;
  };
  const auto switch_of = [&](std::optional<project::TrackId> track) -> const Switched* {
    const auto found = std::ranges::find(switches, track, &Switched::track);
    return found == switches.end() ? nullptr : &*found;
  };
  std::vector<routing::BoundStage> receipts;
  for (const auto& track : snapshot.tracks) {
    if (const auto* chain = chain_of(track.id)) {
      auto stages = track_chain::receipts(track.id, chain->chain);
      receipts.insert(receipts.end(), stages.begin(), stages.end());
    }
  }
  auto route = registry_.compile(snapshot, limits_.latency.max_compensation_frames, receipts);
  if (!route) {
    return fail(snapshot.revision, route.error());
  }
  // A structural switch reads the layout its replacement source was
  // prepared for; any other keeps the running one.
  const auto& expected_sources =
      structural ? std::span<const routing::ProjectRouteSource>(capture->source_layout)
                 : std::span<const routing::ProjectRouteSource>(sources_);
  const auto& expected_order =
      structural ? std::span<const engine::RoutingNodeId>(capture->topological_order)
                 : std::span<const engine::RoutingNodeId>(topological_order_);
  if (!topological_order_.empty() &&
      (!std::ranges::equal(expected_order, route.value().latency.topological_order) ||
       !same_sources(expected_sources, route.value().sources))) {
    return fail(snapshot.revision,
                {ErrorCode::conflict,
                 "Routed live source layout changed; restart playback with prepared sources"});
  }
  const auto* master_chain = chain_of(std::nullopt);
  const auto master_latency = master_chain ? master_chain->chain.latency_frames() : 0;
  // The graph's nodes as export builds them, with the master chain on the
  // master node and each switched chain's predecessor retiring beside it.
  auto graph = routing::graph_nodes(
      snapshot, route.value(),
      {.track_processor = [&](project::TrackId track) -> std::shared_ptr<engine::ProcessorRuntime> {
         const auto* chain = chain_of(track);
         return chain ? chain->chain.runtime : nullptr;
       },
       .master_at_node = true,
       .master_processor = master_chain ? master_chain->chain.runtime : nullptr,
       .master_latency_frames = master_latency});
  if (!graph)
    return fail(snapshot.revision, graph.error());
  auto& nodes = graph.value().nodes;
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    if (const auto* switched = switch_of(graph.value().owners[index])) {
      nodes[index].replacement = switched->change.drain;
      nodes[index].state_handoff_prior = switched->change.handoff;
    }
  }
  auto gains = routing::PreparedRouteGainBlocks::prepare(snapshot, route.value(),
                                                         automation.value(), limits_.max_frames);
  if (!gains)
    return fail(snapshot.revision, gains.error());
  if (route.value().latency.master_output_latency_frames >
      std::numeric_limits<std::uint32_t>::max() - master_latency)
    return fail(snapshot.revision,
                {ErrorCode::resource_exhausted, "Routed output latency exceeds 32-bit range"});
  // Every built-in effect that plays on across the switch takes the edited
  // tempo and lanes on its switching block (ADR-0098).
  if (structural) {
    for (const auto& chain : chains_next) {
      for (const auto& stage : chain.chain.stages) {
        auto* effect = dynamic_cast<native_inserts::NativeEffectStage*>(stage.runtime.get());
        const bool running = std::ranges::any_of(active_chains_, [&](const ActiveChain& prior) {
          return std::ranges::any_of(prior.chain.stages, [&](const track_chain::Stage& old) {
            return old.runtime == stage.runtime;
          });
        });
        if (effect == nullptr || !running)
          continue;
        if (auto staged =
                effect->stage_automation(snapshot, automation.value(), automation_limit.value());
            !staged)
          return fail(snapshot.revision, staged.error());
      }
    }
    // A VST3 instrument that plays on takes its new MIDI and tempo there too.
    for (const auto& record : vst3_next) {
      if (record.slot || !record.track || !record.prepared.prepare_events ||
          std::ranges::none_of(vst3_stages_, [&record](const Vst3Stage& running) {
            return running.prepared.processor == record.prepared.processor;
          }))
        continue;
      const auto before =
          std::ranges::find(published_snapshot_->tracks, *record.track, &project::Track::id);
      const auto after = std::ranges::find(snapshot.tracks, *record.track, &project::Track::id);
      if (before == published_snapshot_->tracks.end() || after == snapshot.tracks.end() ||
          (!retimed && before->midi_clips == after->midi_clips))
        continue;
      auto staged = record.prepared.prepare_events(snapshot, true);
      if (!staged)
        return fail(snapshot.revision, staged.error());
      staged.value()();
    }
  }
  auto published =
      playing ? publisher_.prepare_and_publish_live(snapshot.revision, nodes, route.value().edges,
                                                    limits_, std::move(gains).value(), structural)
              : publisher_.prepare_and_publish(snapshot.revision, nodes, route.value().edges,
                                               limits_, std::move(gains).value());
  if (!published)
    return fail(snapshot.revision, published.error());
  if (playing) {
    live_candidate_ =
        LiveCandidate{.revision = snapshot.revision,
                      .passed_at_publication = publisher_.live_candidate_passed_blocks(),
                      .published_snapshot = published_snapshot_,
                      .source_snapshot = source_snapshot_,
                      .route_plan = published_route_plan_,
                      .chains = active_chains_,
                      .vst3_stages = vst3_stages_,
                      .vst3_automation = active_vst3_automation_,
                      .prepared_limiter = prepared_limiter_,
                      .master_output_latency_frames = master_output_latency_frames_,
                      .sources = {},
                      .topological_order = {}};
    if (structural) {
      live_candidate_->sources = sources_;
      live_candidate_->topological_order = topological_order_;
    }
  }
  const auto route_master_output_latency = route.value().latency.master_output_latency_frames;
  if (topological_order_.empty() || structural) {
    sources_ = std::move(route.value().sources);
    topological_order_ = route.value().latency.topological_order;
  }
  if (initial_route_revision_ == 0) {
    initial_route_revision_ = snapshot.revision;
    initial_route_plan_ = route.value();
  }
  published_route_plan_ = std::move(route.value());
  if (replaces_source)
    source_snapshot_ = prepared_snapshot;
  published_snapshot_ = std::move(prepared_snapshot);
  active_chains_ = std::move(chains_next);
  follow_published_notes();
  vst3_stages_ = std::move(vst3_next);
  active_vst3_automation_ = std::move(vst3_automation);
  // A lookahead stage (a limiter such as Ceiling) holds audio Stop must drain.
  prepared_limiter_ = std::ranges::any_of(
      active_chains_, [](const ActiveChain& chain) { return chain.chain.latency_frames() != 0; });
  master_output_latency_frames_ =
      master_latency + static_cast<std::uint32_t>(route_master_output_latency);
  return Result<void>::success();
}

Result<PreparedRouteTailReceipt>
RoutedPlaybackControl::prepared_route_tail_receipt(std::uint64_t expected_revision,
                                                   std::optional<std::uint64_t> export_end) const {
  using Receipt = Result<PreparedRouteTailReceipt>;
  const auto current = model_.snapshot();
  const auto active = publisher_.active_revision();
  const bool initial_pending = active == 0 && publisher_.has_pending();
  if (!current || !published_snapshot_ || !initial_route_plan_ || expected_revision == 0 ||
      current->revision != expected_revision ||
      published_snapshot_->revision != expected_revision ||
      initial_route_revision_ != expected_revision ||
      (active != expected_revision && !initial_pending) ||
      (active != 0 && publisher_.has_pending()) || preparing_source_ || pending_native_revision_ ||
      pending_manual_)
    return Receipt::failure({ErrorCode::conflict, "Prepared route tail revision is not stable"});
  if (!active_vst3_automation_.empty())
    return Receipt::failure(
        {ErrorCode::unsupported, "Real-time VST3 export cannot qualify automated plugin tails"});

  PreparedRouteTailReceipt receipt{
      initial_route_plan_->latency, initial_route_plan_->master, expected_revision, {}};
  receipt.nodes.reserve(initial_route_plan_->nodes.size());
  for (const auto& node : initial_route_plan_->nodes) {
    std::optional<std::uint64_t> total = 0;
    std::optional<project::TrackId> track_id;
    std::vector<track_chain::SavedStage> saved;
    if (node.id == receipt.master) {
      saved = track_chain::saved_chain(published_snapshot_->master_effects);
    } else {
      const auto owner = std::ranges::find(initial_route_plan_->track_nodes, node.id,
                                           &routing::ProjectRouteTrackNode::node);
      if (owner == initial_route_plan_->track_nodes.end())
        return Receipt::failure({ErrorCode::corrupt_data, "Prepared tail node has no track"});
      const auto track =
          std::ranges::find(published_snapshot_->tracks, owner->track, &project::Track::id);
      if (track == published_snapshot_->tracks.end())
        return Receipt::failure({ErrorCode::corrupt_data, "Prepared tail track is missing"});
      track_id = owner->track;
      saved = track_chain::saved_chain(*track);
      if (std::holds_alternative<project::Vst3PluginInstance>(track->instrument_source)) {
        // The instrument's release follows its exact MIDI cut.
        const auto source = std::ranges::find_if(vst3_stages_, [&](const Vst3Stage& stage) {
          return stage.track == owner->track && !stage.slot;
        });
        if (!export_end || *export_end == 0 || source == vst3_stages_.end() ||
            !source->prepared.prepared_export_end ||
            source->prepared.prepared_export_end() != export_end)
          return Receipt::failure(
              {ErrorCode::unsupported, "Prepared VST3 instrument release cut is unqualified"});
      }
      if (track->kind == project::TrackKind::instrument && !track->midi_clips.empty()) {
        if (!std::holds_alternative<project::NativeInstrumentPatch>(track->instrument_source) &&
            !std::holds_alternative<project::Vst3PluginInstance>(track->instrument_source))
          return Receipt::failure(
              {ErrorCode::unsupported, "Instrument note releases have no real-time export bound"});
      }
      if (std::holds_alternative<project::NativeInstrumentPatch>(track->instrument_source)) {
        const auto source_tail =
            live_source_ && export_end && *export_end > 0
                ? live_source_->prepared_synth_export_tail(owner->track, *export_end)
                : std::nullopt;
        if (!source_tail)
          return Receipt::failure(
              {ErrorCode::unsupported, "Prepared native synth release cut is unavailable"});
        total = sum_export_tail(total, source_tail);
      }
    }
    // The chain's stages ring out in series: a VST3 stage counts only with an
    // independently qualified fixed tail.
    const auto* chain = active_chain(track_id);
    if (chain ? !runs(chain->chain, saved) : !saved.empty())
      return Receipt::failure({ErrorCode::conflict, "Prepared chain tail has changed"});
    if (chain) {
      if (std::ranges::any_of(vst3_stages_, [&](const Vst3Stage& stage) {
            return stage.track == track_id && stage.prepared.process_failed &&
                   stage.prepared.process_failed();
          }))
        return Receipt::failure({ErrorCode::unavailable, "A prepared VST3 stage has failed"});
      total = sum_export_tail(total, chain->chain.export_tail_frames());
    }
    if (!total)
      return Receipt::failure({ErrorCode::unsupported, "Prepared route tail is unbounded"});
    receipt.nodes.push_back({node.id, total});
  }
  return Receipt::success(std::move(receipt));
}

Result<void> RoutedPlaybackControl::seek_after_quiesce(audio_io::TransportCursor& cursor,
                                                       std::uint64_t first_sample) {
  if (graph_handoff_in_progress())
    return rejected(ErrorCode::conflict,
                    "A seek requires a quiescent graph after the device callback joins");
  if (!cursor.seek_after_quiesce(first_sample))
    return rejected(ErrorCode::resource_exhausted, "Transport cannot seek to that sample");
  return Result<void>::success();
}

} // namespace oscillate::application
