#include "oscillate/render_graph/offline_graph.hpp"

#include "oscillate/native_inserts/instrument_binding.hpp"
#include "oscillate/native_inserts/native_stage.hpp"
#include "oscillate/native_inserts/synth_player.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/render_graph/insert_preflight.hpp"
#include "oscillate/render_graph/rebalance.hpp"
#include "oscillate/routing/project_route_plan.hpp"
#include "oscillate/source_prep/synth_media.hpp"
#include "oscillate/track_chain/track_chain.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <variant>

namespace oscillate::render_graph {
namespace {

// Delay compensation an offline route with a latent stage may apply.
constexpr std::uint32_t latent_compensation_frames = 8192;
// Delay history an offline route may hold.
constexpr std::uint64_t history_budget_bytes = 64ULL * 1024 * 1024;

struct NativeRouteRuntime {
  project::TrackId track;
  track_chain::PreparedChain chain;
};

[[nodiscard]] std::optional<project::Snapshot>
native_export_view(const project::Snapshot& snapshot) {
  // Route compilation and automation preparation require every referenced
  // VST3 effect to have a runtime. Bypassed effects have none. Remove those
  // effects and their inactive lanes only from this private render view.
  if (std::ranges::none_of(snapshot.tracks, [](const auto& track) {
        return std::ranges::any_of(track.effects, [](const auto& slot) {
          return !slot.enabled &&
                 std::holds_alternative<project::Vst3PluginInstance>(slot.processor);
        });
      }))
    return std::nullopt;
  auto view = snapshot;
  for (auto& track : view.tracks) {
    std::vector<project::PluginInstanceId> bypassed_plugins;
    for (const auto& slot : track.effects)
      if (!slot.enabled)
        if (const auto* plugin = std::get_if<project::Vst3PluginInstance>(&slot.processor))
          bypassed_plugins.push_back(plugin->id);
    if (bypassed_plugins.empty())
      continue;
    std::erase_if(track.effects, [](const auto& slot) {
      return !slot.enabled && std::holds_alternative<project::Vst3PluginInstance>(slot.processor);
    });
    const auto bypassed_lane = [&](const project::AutomationLane& lane) {
      const auto* target = std::get_if<project::Vst3ParameterTarget>(&lane.target);
      return target &&
             std::ranges::find(bypassed_plugins, target->plugin) != bypassed_plugins.end();
    };
    std::erase_if(track.automation, bypassed_lane);
    for (auto& clip : track.clips)
      std::erase_if(clip.automation, bypassed_lane);
  }
  return view;
}

[[nodiscard]] foundation::Result<OfflineRoute>
prepare_offline_routing(const project::Snapshot& snapshot, const OfflineGraphRequest& request,
                        std::span<const routing::BoundStage> native_chain = {},
                        std::span<const NativeRouteRuntime> native_runtimes = {}) {
  const auto block_frames = request.max_frames;
  routing::ProjectRouteIdRegistry ids;
  // Each native chain runs after its own incoming main/send contributions.
  // Lookahead belongs to that node and parallel paths receive bounded PDC.
  const bool latent_stage = std::ranges::any_of(
      native_chain, [](const auto& receipt) { return receipt.latency_frames != 0; });
  const std::uint32_t compensation_limit = latent_stage ? latent_compensation_frames : 0;
  const auto route_view = native_export_view(snapshot);
  auto plan = ids.compile(route_view ? *route_view : snapshot, compensation_limit, native_chain);
  if (!plan) {
    return foundation::Result<OfflineRoute>::failure(plan.error());
  }
  if (native_chain.empty() != native_runtimes.empty())
    return foundation::Result<OfflineRoute>::failure(
        {foundation::ErrorCode::corrupt_data, "Native route processor count is inconsistent"});
  for (const auto& receipt : native_chain) {
    const auto owner =
        std::ranges::find(native_runtimes, receipt.track, &NativeRouteRuntime::track);
    if (owner == native_runtimes.end() || !owner->chain.runtime)
      return foundation::Result<OfflineRoute>::failure(
          {foundation::ErrorCode::corrupt_data, "Native route has no prepared processor"});
  }
  std::map<engine::RoutingNodeId, std::uint64_t> own_tails;
  for (const auto& runtime : native_runtimes) {
    const auto owner = std::ranges::find(plan.value().track_nodes, runtime.track,
                                         &routing::ProjectRouteTrackNode::track);
    const auto tail = runtime.chain.export_tail_frames();
    if (owner == plan.value().track_nodes.end() || !runtime.chain.runtime || !tail ||
        !own_tails.emplace(owner->node, *tail).second)
      return foundation::Result<OfflineRoute>::failure(
          {foundation::ErrorCode::corrupt_data, "Native route owner is absent or duplicated"});
  }
  // Each track's rebalance stage, and the ring-out its EQ adds to its node.
  RebalanceStages rebalance;
  if (auto updated = rebalance.update(snapshot); !updated)
    return foundation::Result<OfflineRoute>::failure(updated.error());
  for (const auto& owner : plan.value().track_nodes)
    own_tails[owner.node] += rebalance.tail_frames(owner.track);
  std::map<engine::RoutingNodeId, std::vector<engine::RoutingNodeId>> outgoing;
  for (const auto& edge : plan.value().edges)
    outgoing[edge.source].push_back(edge.destination);
  std::map<engine::RoutingNodeId, std::uint64_t> path_tails;
  for (const auto node : plan.value().latency.topological_order) {
    auto& extent = path_tails[node];
    const auto own = own_tails.find(node);
    if (own != own_tails.end()) {
      if (own->second > std::numeric_limits<std::uint64_t>::max() - extent)
        return foundation::Result<OfflineRoute>::failure(
            {foundation::ErrorCode::resource_exhausted, "Native route tail exceeds frame range"});
      extent += own->second;
    }
    for (const auto destination : outgoing[node])
      path_tails[destination] = std::max(path_tails[destination], extent);
  }
  const auto tail = path_tails.find(plan.value().master);
  if (tail == path_tails.end())
    return foundation::Result<OfflineRoute>::failure(
        {foundation::ErrorCode::corrupt_data, "Native route has no master tail path"});
  // The same nodes live playback builds, except that the master chain runs
  // after the mix and a track nobody can hear is muted at its node, which
  // then skips its work.
  auto graph_nodes = routing::graph_nodes(
      snapshot, plan.value(),
      {.track_processor = [&](project::TrackId track) -> std::shared_ptr<engine::ProcessorRuntime> {
         const auto native = std::ranges::find(native_runtimes, track, &NativeRouteRuntime::track);
         return native == native_runtimes.end() ? nullptr : native->chain.runtime;
       },
       .track_output_stage = [&](project::TrackId track)
           -> std::shared_ptr<engine::ProcessorRuntime> { return rebalance.stage(track); },
       .mute_silent_tracks = true});
  if (!graph_nodes)
    return foundation::Result<OfflineRoute>::failure(graph_nodes.error());
  const auto& nodes = graph_nodes.value().nodes;
  const std::set<engine::RoutingNodeId> silent_nodes(graph_nodes.value().silent.begin(),
                                                     graph_nodes.value().silent.end());
  // A keyed node has a detector buffer; a key's source keeps its pre-fader block.
  std::set<engine::RoutingNodeId> sidechain_destinations;
  std::set<engine::RoutingNodeId> sidechain_sources;
  for (const auto& edge : plan.value().edges) {
    if (edge.kind == engine::RoutingEdgeKind::sidechain) {
      sidechain_destinations.insert(edge.destination);
      sidechain_sources.insert(edge.source);
    }
  }
  const auto buffer_bytes = (static_cast<std::uint64_t>(nodes.size()) + 1 +
                             sidechain_destinations.size() + sidechain_sources.size()) *
                            block_frames * sizeof(engine::StereoFrame);
  auto mixer = engine::prepare_routed_mixer(nodes, plan.value().edges,
                                            {.latency = {plan.value().master, compensation_limit},
                                             .max_frames = block_frames,
                                             .max_buffer_bytes = buffer_bytes,
                                             .max_history_bytes = history_budget_bytes,
                                             .skip_muted_nodes = true});
  if (!mixer) {
    return foundation::Result<OfflineRoute>::failure(mixer.error());
  }
  auto gains = routing::PreparedRouteGainBlocks::prepare(snapshot, plan.value(), request.automation,
                                                         block_frames);
  if (!gains)
    return foundation::Result<OfflineRoute>::failure(gains.error());
  OfflineRoute route;
  route.mixer = std::move(mixer).value();
  route.gains = std::move(gains).value();
  route.track_slots.resize(snapshot.tracks.size());
  route.inputs.resize(nodes.size(), std::vector<engine::StereoFrame>(block_frames));
  route.views.resize(nodes.size());
  route.silent_gain.resize(block_frames);
  // Inputs are in topological order, as the plan's sources index them.
  route.silent_inputs.resize(nodes.size());
  for (std::size_t index = 0; index < plan.value().latency.topological_order.size(); ++index)
    route.silent_inputs[index] =
        silent_nodes.contains(plan.value().latency.topological_order[index]);
  if (request.solo_track) {
    std::set<engine::RoutingNodeId> other_sources;
    std::set<engine::RoutingNodeId> detector_nodes;
    for (const auto& source : plan.value().sources) {
      if (source.track != *request.solo_track)
        other_sources.insert(source.node);
    }
    for (const auto& owner : plan.value().track_nodes) {
      if (std::ranges::find(request.detector_buses, owner.track) != request.detector_buses.end())
        detector_nodes.insert(owner.node);
    }
    for (std::size_t index = 0; index < plan.value().latency.edges.size(); ++index) {
      const auto& edge = plan.value().latency.edges[index];
      if (edge.kind != engine::RoutingEdgeKind::sidechain &&
          (detector_nodes.contains(edge.source) ||
           (other_sources.contains(edge.source) && !detector_nodes.contains(edge.destination))))
        route.isolated_edges.push_back(index);
    }
    route.isolated_gains.resize(plan.value().latency.edges.size());
  }
  route.output_latency_frames = plan.value().latency.master_output_latency_frames;
  route.native_tail_frames = tail->second;
  for (const auto& source : plan.value().sources) {
    const auto track = std::ranges::find_if(
        snapshot.tracks, [&](const auto& item) { return item.id == source.track; });
    if (track == snapshot.tracks.end() || source.topological_index >= route.inputs.size()) {
      return foundation::Result<OfflineRoute>::failure(
          {foundation::ErrorCode::corrupt_data, "Offline route source mapping is invalid"});
    }
    route.track_slots[static_cast<std::size_t>(track - snapshot.tracks.begin())] =
        source.topological_index;
    route.sources.push_back(
        {source.track, source.topological_index,
         plan.value().latency.nodes[source.topological_index].output_latency_frames});
  }
  // Snapshot order, as the taps report them.
  std::ranges::sort(route.sources, {}, [&](const RoutedSource& source) {
    return std::ranges::find(snapshot.tracks, source.track, &project::Track::id) -
           snapshot.tracks.begin();
  });
  return foundation::Result<OfflineRoute>::success(std::move(route));
}

} // namespace

foundation::Result<std::vector<automation_project::PreparedTargetAutomation>>
prepare_offline_automation(const project::Snapshot& snapshot) {
  const auto view = native_export_view(snapshot);
  return automation_project::prepare_project_automation(view ? *view : snapshot, snapshot.revision);
}

bool OfflineStage::faulted() const noexcept {
  if (failed)
    return failed();
  return held_non_finite(*runtime);
}

bool OfflineGraph::chain_faulted() const noexcept {
  return std::ranges::any_of(chain_stages, &OfflineStage::faulted);
}

bool held_non_finite(const engine::ProcessorRuntime& stage) noexcept {
  if (const auto frames = native_inserts::guarded_frames_after_quiesce(stage))
    return *frames != 0;
  return true;
}

foundation::Result<OfflineGraph> prepare_offline_graph(const project::Snapshot& snapshot,
                                                       const persistence::ProjectStore& store,
                                                       const OfflineGraphRequest& request) {
  using R = foundation::Result<OfflineGraph>;
  OfflineGraph graph;
  const bool vst3 = static_cast<bool>(request.vst3);
  const bool has_track_chain = std::ranges::any_of(snapshot.tracks, [vst3](const auto& track) {
    return (vst3 && std::holds_alternative<project::Vst3PluginInstance>(track.instrument_source)) ||
           std::ranges::any_of(track.effects, [](const auto& slot) {
             return slot.enabled ||
                    !std::holds_alternative<project::Vst3PluginInstance>(slot.processor);
           });
  });
  if (project::requires_routed_rendering(snapshot) && !has_track_chain) {
    auto prepared = prepare_offline_routing(snapshot, request);
    if (!prepared)
      return R::failure(prepared.error());
    graph.route.emplace(std::move(prepared).value());
  }
  if (!timeline::supported_sample_rate(snapshot.sample_rate)) {
    return R::failure(
        {foundation::ErrorCode::unsupported, "Master export sample rate is unsupported"});
  }
  auto inserts = preflight_inserts(snapshot, vst3 ? ExportSource::vst3 : ExportSource::native);
  if (!inserts)
    return R::failure(inserts.error());
  native_inserts::StageContext stage_context{snapshot.sample_rate};
  stage_context.snapshot = &snapshot;
  stage_context.automation = request.automation;
  stage_context.max_frames = request.max_frames;
  // A built-in effect's managed media (Cast's impulse response).
  stage_context.media = [&](const project::NativeSynthDocument& document) {
    return source_prep::load_synth_media(store, snapshot, document);
  };
  std::vector<NativeRouteRuntime> track_chains;
  std::vector<routing::BoundStage> receipts;
  for (const auto& saved_chain : inserts.value().track_chains) {
    const auto owner = std::ranges::find(snapshot.tracks, saved_chain.owner, &project::Track::id);
    if (owner == snapshot.tracks.end())
      return R::failure({foundation::ErrorCode::corrupt_data, "Offline chain has no saved track"});
    track_chain::ChainContext context{stage_context};
    context.detector_slot = routing::sidechain_target(snapshot, owner->id);
    if (vst3)
      context.vst3 = [&](const track_chain::SavedStage& stage) {
        return request.vst3(snapshot, stage, request.max_frames);
      };
    const auto saved = track_chain::saved_chain(*owner);
    auto prepared = track_chain::prepare_chain(saved, context);
    if (!prepared)
      return R::failure(prepared.error());
    // Offline renders need every tail bounded.
    for (const auto& stage : prepared.value().stages) {
      if (!stage.export_tail_frames)
        return R::failure({foundation::ErrorCode::unsupported, "Offline effect tail is unbounded"});
    }
    if (!prepared.value().runtime)
      continue;
    auto stages = track_chain::receipts(owner->id, prepared.value());
    receipts.insert(receipts.end(), stages.begin(), stages.end());
    for (const auto& stage : prepared.value().stages)
      graph.chain_stages.push_back({stage.runtime, stage.failed});
    track_chains.push_back({owner->id, std::move(prepared).value()});
  }
  if (!inserts.value().track_chains.empty()) {
    auto prepared = prepare_offline_routing(snapshot, request, receipts, track_chains);
    if (!prepared)
      return R::failure(prepared.error());
    graph.route.emplace(std::move(prepared).value());
  }
  // Taps read each track's node of the routed mixer, so a tapped render is
  // routed even with nothing on its tracks.
  if (request.always_routed && !graph.route) {
    auto prepared = prepare_offline_routing(snapshot, request);
    if (!prepared)
      return R::failure(prepared.error());
    graph.route.emplace(std::move(prepared).value());
  }
  // The master chain in saved order. Tails ring out after the mix; latency
  // drains internally and is trimmed at the writer.
  for (const auto* slot : inserts.value().master_chain) {
    if (std::holds_alternative<project::Vst3PluginInstance>(slot->processor)) {
      auto prepared = request.vst3(snapshot, {slot->id, slot->processor}, request.max_frames);
      if (!prepared)
        return R::failure(prepared.error());
      auto& stage = prepared.value();
      const auto tail = stage.export_tail_frames;
      if (!tail)
        return R::failure({foundation::ErrorCode::unsupported, "Offline effect tail is unbounded"});
      if (*tail > std::numeric_limits<std::uint64_t>::max() - graph.master_tail_frames)
        return R::failure(
            {foundation::ErrorCode::resource_exhausted, "Master effect tail exceeds render range"});
      graph.master_tail_frames += *tail;
      graph.master_latency_frames += stage.latency_frames;
      graph.master_stages.push_back({std::move(stage.runtime), std::move(stage.failed)});
      continue;
    }
    auto prepared = native_inserts::prepare_native_stage(*slot, stage_context);
    if (!prepared)
      return R::failure(prepared.error());
    auto& stage = prepared.value();
    // Offline renders need every tail bounded.
    if (!stage.export_tail_frames)
      return R::failure(stage.tail_error ? *stage.tail_error
                                         : foundation::Error{foundation::ErrorCode::unsupported,
                                                             "Offline effect tail is unbounded"});
    if (*stage.export_tail_frames >
        std::numeric_limits<std::uint64_t>::max() - graph.master_tail_frames)
      return R::failure(
          {foundation::ErrorCode::resource_exhausted, "Master effect tail exceeds render range"});
    graph.master_tail_frames += *stage.export_tail_frames;
    graph.master_latency_frames += stage.latency_frames;
    graph.master_stages.push_back({std::move(stage.runtime)});
  }
  return R::success(std::move(graph));
}

struct OfflineInstrument::Player {
  native_inserts::NativeSynthPlayer player;
};

OfflineInstrument::OfflineInstrument(std::unique_ptr<Player> player) noexcept
    : player_(std::move(player)) {}
OfflineInstrument::OfflineInstrument(OfflineInstrument&&) noexcept = default;
OfflineInstrument& OfflineInstrument::operator=(OfflineInstrument&&) noexcept = default;
OfflineInstrument::~OfflineInstrument() = default;

foundation::Result<OfflineInstrument>
OfflineInstrument::prepare(const project::Snapshot& snapshot,
                           const persistence::ProjectStore& store, const project::Track& track,
                           const engine::InstrumentEvents& events,
                           std::span<const automation_project::PreparedTargetAutomation> automation,
                           std::size_t max_frames) {
  using R = foundation::Result<OfflineInstrument>;
  const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
  if (patch == nullptr)
    return R::failure(
        {foundation::ErrorCode::unsupported, "Offline instrument needs a built-in instrument"});
  auto media = source_prep::load_synth_media(store, snapshot, patch->document);
  if (!media)
    return R::failure(media.error());
  auto player = native_inserts::NativeSynthPlayer::prepare(
      snapshot, track, events, automation, max_frames, std::numeric_limits<std::uint64_t>::max(),
      std::move(media).value());
  if (!player)
    return R::failure(player.error());
  return R::success(OfflineInstrument(std::make_unique<Player>(Player{std::move(player).value()})));
}

void OfflineInstrument::await_content() noexcept { player_->player.await_content(); }

bool OfflineInstrument::render(std::span<engine::StereoFrame> output, std::uint64_t first,
                               engine::AudioWorkerPool* workers) noexcept {
  // Offline renders run one transport generation.
  constexpr std::uint64_t transport_generation = 1;
  return player_->player.render(output, first, transport_generation, workers);
}

std::uint64_t OfflineInstrument::tail_frames() const noexcept {
  return player_->player.tail_frames();
}

foundation::Result<std::uint64_t>
instrument_tail_frames(const project::NativeInstrumentPatch& patch, std::uint32_t sample_rate) {
  auto resolved = native_inserts::resolve_native_instrument(patch);
  if (!resolved)
    return foundation::Result<std::uint64_t>::failure(resolved.error());
  return foundation::Result<std::uint64_t>::success(resolved.value().engine->tail_frames(
      resolved.value().values, static_cast<double>(sample_rate)));
}

} // namespace oscillate::render_graph
