#include "oscillate/render_graph/offline_graph.hpp"

#include "oscillate/native_dsp/synth/player.hpp"
#include "oscillate/native_dsp/synth/project_binding.hpp"
#include "oscillate/native_inserts/native_stage.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/render_graph/insert_preflight.hpp"
#include "oscillate/routing/project_route_plan.hpp"
#include "oscillate/source_prep/synth_media.hpp"
#include "oscillate/track_chain/track_chain.hpp"
#if OSCILLATE_HAVE_CONVOLUTION
#include "oscillate/native_dsp/convolution_ir.hpp"
#include "oscillate/native_dsp/convolution_runtime.hpp"
#endif

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <variant>

#include <unistd.h>

namespace oscillate::render_graph {
namespace {

// Delay compensation an offline route with a latent stage may apply.
constexpr std::uint32_t latent_compensation_frames = 8192;
// Delay history an offline route may hold.
constexpr std::uint64_t history_budget_bytes = 64ULL * 1024 * 1024;
#if OSCILLATE_HAVE_CONVOLUTION
// Memory a saved impulse response may take while it is prepared.
constexpr std::uint64_t convolution_preparation_budget_bytes = 12ULL * 1024 * 1024;
#endif

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
  auto plan = ids.compile(route_view ? *route_view : snapshot, compensation_limit, native_chain,
                          OSCILLATE_HAVE_CONVOLUTION);
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
  std::vector<engine::RoutedNode> nodes;
  nodes.reserve(plan.value().nodes.size());
  const auto track_mutes = routing::effective_track_mutes(snapshot, plan.value());
  // A track nobody can hear is muted at its node, which then skips its work.
  const auto silent_tracks = routing::silent_track_nodes(snapshot, plan.value());
  std::set<engine::RoutingNodeId> silent_nodes;
  for (const auto node : plan.value().nodes) {
    std::shared_ptr<engine::ProcessorRuntime> processor;
    bool muted = false;
    engine::StereoFrame output_gain{1.0F, 1.0F};
    if (const auto owner = std::ranges::find(plan.value().track_nodes, node.id,
                                             &routing::ProjectRouteTrackNode::node);
        owner != plan.value().track_nodes.end()) {
      if (const auto native =
              std::ranges::find(native_runtimes, owner->track, &NativeRouteRuntime::track);
          native != native_runtimes.end())
        processor = native->chain.runtime;
      const auto track_index = static_cast<std::size_t>(owner - plan.value().track_nodes.begin());
      muted = track_mutes[track_index] || silent_tracks[track_index];
      if (silent_tracks[track_index])
        silent_nodes.insert(node.id);
      if (const auto track = std::ranges::find(snapshot.tracks, owner->track, &project::Track::id);
          track != snapshot.tracks.end()) {
        const auto gain = routing::track_stereo_gain(*track);
        output_gain = {gain.left, gain.right};
      }
    }
    nodes.push_back({node, std::move(processor), {}, muted, output_gain});
  }
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

#if OSCILLATE_HAVE_CONVOLUTION
[[nodiscard]] foundation::Result<native_dsp::PreparedConvolutionIr>
prepare_saved_convolution(const project::Snapshot& snapshot, const persistence::ProjectStore& store,
                          const project::NativeConvolution& effect) {
  const auto record =
      std::ranges::find(snapshot.sources, effect.ir.source, &project::SourceRecord::id);
  if (record == snapshot.sources.end() || record->external_link ||
      record->encoding != project::SourceEncoding::wav)
    return foundation::Result<native_dsp::PreparedConvolutionIr>::failure(
        {foundation::ErrorCode::corrupt_data, "Convolution IR has no managed WAV source"});
  auto descriptor = store.open_managed_source(*record);
  if (!descriptor)
    return foundation::Result<native_dsp::PreparedConvolutionIr>::failure(descriptor.error());
  auto prepared = native_dsp::prepare_convolution_ir_offline(
      descriptor.value(),
      {effect.ir.sha256, effect.ir.encoded_bytes, effect.ir.sample_rate,
       static_cast<std::uint16_t>(effect.ir.channels), effect.ir.frames},
      convolution_preparation_budget_bytes, snapshot.sample_rate, static_cast<float>(effect.wet));
  const int close_result = ::close(descriptor.value());
  if (!prepared)
    return foundation::Result<native_dsp::PreparedConvolutionIr>::failure(prepared.error());
  if (close_result != 0)
    return foundation::Result<native_dsp::PreparedConvolutionIr>::failure(
        {foundation::ErrorCode::io_failure, "Cannot close verified convolution IR"});
  return prepared;
}
#endif

} // namespace

foundation::Result<std::vector<automation_project::PreparedTargetAutomation>>
prepare_offline_automation(const project::Snapshot& snapshot) {
  const auto view = native_export_view(snapshot);
  return automation_project::prepare_project_automation(view ? *view : snapshot, snapshot.revision);
}

bool OfflineGraph::chain_faulted() const noexcept {
  return std::ranges::any_of(chain_stages,
                             [](const auto& stage) { return held_non_finite(*stage); });
}

bool held_non_finite(const engine::ProcessorRuntime& stage) noexcept {
  if (const auto frames = native_inserts::guarded_frames_after_quiesce(stage))
    return *frames != 0;
#if OSCILLATE_HAVE_CONVOLUTION
  if (const auto* convolution = dynamic_cast<const native_dsp::ConvolutionRuntime*>(&stage))
    return convolution->guarded_frames_after_quiesce() != 0;
#endif
  return true;
}

foundation::Result<OfflineGraph> prepare_offline_graph(const project::Snapshot& snapshot,
                                                       const persistence::ProjectStore& store,
                                                       const OfflineGraphRequest& request) {
  using R = foundation::Result<OfflineGraph>;
#if !OSCILLATE_HAVE_CONVOLUTION
  (void)store;
#endif
  OfflineGraph graph;
  const bool has_track_native = std::ranges::any_of(snapshot.tracks, [](const auto& track) {
    return std::ranges::any_of(track.effects, [](const auto& slot) {
      return slot.enabled || !std::holds_alternative<project::Vst3PluginInstance>(slot.processor);
    });
  });
  if (project::requires_routed_rendering(snapshot) && !has_track_native) {
    auto prepared = prepare_offline_routing(snapshot, request);
    if (!prepared)
      return R::failure(prepared.error());
    graph.route.emplace(std::move(prepared).value());
  }
  if (!timeline::supported_sample_rate(snapshot.sample_rate)) {
    return R::failure(
        {foundation::ErrorCode::unsupported, "Master export sample rate is unsupported"});
  }
  auto inserts =
      preflight_inserts(snapshot, ExportSource::native, false, OSCILLATE_HAVE_CONVOLUTION);
  if (!inserts)
    return R::failure(inserts.error());
  native_inserts::StageContext stage_context{snapshot.sample_rate};
  stage_context.snapshot = &snapshot;
  stage_context.automation = request.automation;
  stage_context.max_frames = request.max_frames;
#if OSCILLATE_HAVE_CONVOLUTION
  stage_context.convolution = [&](const project::NativeConvolution& saved)
      -> foundation::Result<native_inserts::PreparedStage> {
    auto prepared = prepare_saved_convolution(snapshot, store, saved);
    if (!prepared)
      return foundation::Result<native_inserts::PreparedStage>::failure(prepared.error());
    native_inserts::PreparedStage stage;
    stage.kind = native_inserts::StageKind::convolution;
    stage.generation = saved.generation;
    const auto tail = prepared.value().processor->tail_frames();
    stage.export_tail_frames = saved.wet == 0.0 ? 0 : tail;
    stage.transition_tail_frames = saved.wet == 0.0 ? 1 : std::max<std::uint64_t>(1, tail);
    stage.transition_rule = native_inserts::TransitionRule::override;
    stage.runtime =
        std::make_shared<native_dsp::ConvolutionRuntime>(std::move(prepared.value().processor));
    return foundation::Result<native_inserts::PreparedStage>::success(std::move(stage));
  };
#endif
  std::vector<NativeRouteRuntime> track_chains;
  std::vector<routing::BoundStage> receipts;
  for (const auto& saved_chain : inserts.value().native_track_chains) {
    const auto owner = std::ranges::find(snapshot.tracks, saved_chain.owner, &project::Track::id);
    if (owner == snapshot.tracks.end())
      return R::failure({foundation::ErrorCode::corrupt_data, "Offline chain has no saved track"});
    track_chain::ChainContext context{stage_context};
    context.detector_slot = routing::sidechain_target(snapshot, owner->id);
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
      graph.chain_stages.push_back(stage.runtime);
    track_chains.push_back({owner->id, std::move(prepared).value()});
  }
  if (!inserts.value().native_track_chains.empty()) {
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
    graph.master_stages.push_back(std::move(stage.runtime));
  }
  return R::success(std::move(graph));
}

struct OfflineInstrument::Player {
  native_dsp::synth::NativeSynthPlayer player;
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
  auto player = native_dsp::synth::NativeSynthPlayer::prepare(
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
  auto resolved = native_dsp::synth::resolve_native_instrument(patch);
  if (!resolved)
    return foundation::Result<std::uint64_t>::failure(resolved.error());
  return foundation::Result<std::uint64_t>::success(resolved.value().engine->tail_frames(
      resolved.value().values, static_cast<double>(sample_rate)));
}

} // namespace oscillate::render_graph
