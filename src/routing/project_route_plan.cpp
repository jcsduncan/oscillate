#include "oscillate/routing/project_route_plan.hpp"

#include "oscillate/foundation/trace.hpp"
#include "oscillate/project/insert_projection.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <utility>

namespace oscillate::routing {
namespace {
using Result = foundation::Result<ProjectRoutePlan>;

[[nodiscard]] Result fail(foundation::ErrorCode code, std::string message) {
  return Result::failure({code, std::move(message)});
}

[[nodiscard]] std::uint64_t stable_id(std::map<foundation::IdBits, std::uint64_t>& ids,
                                      foundation::IdBits key, std::uint64_t& next) {
  if (const auto found = ids.find(key); found != ids.end()) {
    return found->second;
  }
  const auto id = next++;
  ids.emplace(key, id);
  return id;
}
} // namespace

[[nodiscard]] bool has_mute_automation(const project::Track& track) noexcept {
  const auto mute_lane = [](const project::AutomationLane& lane) {
    const auto* control = std::get_if<project::TrackControlTarget>(&lane.target);
    return control && control->key == project::TrackControlTarget::Key::channel_mute;
  };
  return std::ranges::any_of(track.automation, mute_lane) ||
         std::ranges::any_of(track.clips, [&](const project::Clip& clip) {
           return std::ranges::any_of(clip.automation, mute_lane);
         });
}

std::vector<bool> effective_track_mutes(const project::Snapshot& snapshot,
                                        const ProjectRoutePlan& plan) {
  std::map<engine::RoutingNodeId, std::size_t> index_by_node;
  for (std::size_t index = 0; index < plan.track_nodes.size(); ++index) {
    index_by_node.emplace(plan.track_nodes[index].node, index);
  }
  std::vector<bool> explicitly_muted(plan.track_nodes.size());
  std::vector<bool> retained(plan.track_nodes.size());
  std::vector<std::size_t> solo_indices;
  for (std::size_t index = 0; index < plan.track_nodes.size(); ++index) {
    const auto track =
        std::ranges::find(snapshot.tracks, plan.track_nodes[index].track, &project::Track::id);
    if (track == snapshot.tracks.end())
      continue;
    // A mute lane drives the channel's mute through its node gain instead.
    explicitly_muted[index] = track->muted && !has_mute_automation(*track);
    if (track->soloed)
      solo_indices.push_back(index);
  }
  if (solo_indices.empty())
    return explicitly_muted;

  // Both traversals are bounded by the validated routing graph. Keeping
  // descendants lets a soloed source reach returns and buses; keeping
  // ancestors lets a soloed bus receive its inputs.
  for (const bool upstream : {true, false}) {
    std::vector<std::size_t> queue = solo_indices;
    std::vector<bool> visited(plan.track_nodes.size());
    for (const auto index : queue)
      visited[index] = true;
    for (std::size_t cursor = 0; cursor < queue.size(); ++cursor) {
      const auto node = plan.track_nodes[queue[cursor]].node;
      for (const auto& edge : plan.edges) {
        // A key is not audio: soloing the keyed track leaves its key's
        // source silent (the key still taps it before mute), and soloing
        // the source leaves the track it keys silent.
        if (edge.kind == engine::RoutingEdgeKind::sidechain ||
            (upstream ? edge.destination : edge.source) != node)
          continue;
        const auto found = index_by_node.find(upstream ? edge.source : edge.destination);
        if (found != index_by_node.end() && !visited[found->second]) {
          visited[found->second] = true;
          queue.push_back(found->second);
        }
      }
    }
    for (std::size_t index = 0; index < visited.size(); ++index)
      retained[index] = retained[index] || visited[index];
  }
  std::vector<bool> muted(plan.track_nodes.size());
  for (std::size_t index = 0; index < muted.size(); ++index)
    muted[index] = explicitly_muted[index] || !retained[index];
  return muted;
}

std::vector<bool> silent_track_nodes(const project::Snapshot& snapshot,
                                     const ProjectRoutePlan& plan) {
  auto silent = effective_track_mutes(snapshot, plan);
  std::map<engine::RoutingNodeId, std::size_t> index_by_node;
  for (std::size_t index = 0; index < plan.track_nodes.size(); ++index)
    index_by_node.emplace(plan.track_nodes[index].node, index);
  std::vector<bool> keys(plan.track_nodes.size());
  for (const auto& edge : plan.edges) {
    if (edge.kind != engine::RoutingEdgeKind::sidechain)
      continue;
    if (const auto found = index_by_node.find(edge.source); found != index_by_node.end())
      keys[found->second] = true;
  }
  for (std::size_t index = 0; index < silent.size(); ++index)
    silent[index] = silent[index] && !keys[index];
  // A track feeding only silent tracks is silent too. Each pass silences at
  // least one more track or stops, so this ends within track_nodes.size().
  for (bool changed = true; changed;) {
    changed = false;
    for (std::size_t index = 0; index < silent.size(); ++index) {
      if (silent[index] || keys[index])
        continue;
      bool feeds = false;
      bool heard = false;
      for (const auto& edge : plan.edges) {
        if (edge.kind == engine::RoutingEdgeKind::sidechain ||
            edge.source != plan.track_nodes[index].node)
          continue;
        feeds = true;
        const auto found = index_by_node.find(edge.destination);
        heard = heard || found == index_by_node.end() || !silent[found->second];
      }
      if (feeds && !heard) {
        silent[index] = true;
        changed = true;
      }
    }
  }
  return silent;
}

TrackStereoGain channel_stereo_gain(double level, double pan) noexcept {
  const auto gain = static_cast<float>(level);
  const auto side = static_cast<float>(pan);
  return {gain * (side > 0.0F ? 1.0F - side : 1.0F), gain * (side < 0.0F ? 1.0F + side : 1.0F)};
}

TrackStereoGain track_stereo_gain(const project::Track& track) noexcept {
  return channel_stereo_gain(track.level, track.pan);
}

foundation::Result<std::vector<engine::RoutedNodeControl>>
project_node_controls(const project::Snapshot& snapshot, const ProjectRoutePlan& plan) {
  using Controls = std::vector<engine::RoutedNodeControl>;
  if (snapshot.tracks.size() != plan.track_nodes.size())
    return foundation::Result<Controls>::failure(
        {foundation::ErrorCode::conflict, "route control track set changed"});
  const auto mutes = effective_track_mutes(snapshot, plan);
  Controls result;
  result.reserve(plan.latency.nodes.size());
  for (const auto& node : plan.latency.nodes) {
    const auto mapped = std::ranges::find(plan.track_nodes, node.id, &ProjectRouteTrackNode::node);
    if (mapped == plan.track_nodes.end()) {
      if (node.id != plan.master)
        return foundation::Result<Controls>::failure(
            {foundation::ErrorCode::conflict, "route control node has no saved track"});
      const auto level = static_cast<float>(snapshot.master_level);
      result.push_back({node.id, false, {level, level}});
      continue;
    }
    const auto track = std::ranges::find(snapshot.tracks, mapped->track, &project::Track::id);
    if (track == snapshot.tracks.end())
      return foundation::Result<Controls>::failure(
          {foundation::ErrorCode::conflict, "route control track is missing"});
    const auto index = static_cast<std::size_t>(mapped - plan.track_nodes.begin());
    const auto gain = track_stereo_gain(*track);
    result.push_back({node.id, mutes[index], {gain.left, gain.right}});
  }
  return foundation::Result<Controls>::success(std::move(result));
}

foundation::Result<std::vector<engine::RoutedEdgeControl>>
project_edge_controls(const project::Snapshot& snapshot, const ProjectRoutePlan& plan) {
  using Controls = std::vector<engine::RoutedEdgeControl>;
  Controls result;
  result.reserve(plan.latency.edges.size());
  for (const auto& edge : plan.latency.edges)
    result.push_back({edge.id, edge.gain});
  for (const auto& mapped : plan.sends) {
    const auto track = std::ranges::find(snapshot.tracks, mapped.track, &project::Track::id);
    if (track == snapshot.tracks.end())
      return foundation::Result<Controls>::failure(
          {foundation::ErrorCode::conflict, "send overlay track is missing"});
    const auto send = std::ranges::find(track->sends, mapped.send, &project::SendRoute::id);
    const auto edge = std::ranges::find(result, mapped.edge, &engine::RoutedEdgeControl::id);
    if (send == track->sends.end() || edge == result.end())
      return foundation::Result<Controls>::failure(
          {foundation::ErrorCode::conflict, "send overlay edge is missing"});
    if (!std::isfinite(send->gain) || send->gain < 0.0 || send->gain > 4.0)
      return foundation::Result<Controls>::failure(
          {foundation::ErrorCode::invalid_argument, "send overlay gain is invalid"});
    edge->gain = static_cast<float>(send->gain);
  }
  return foundation::Result<Controls>::success(std::move(result));
}

namespace {
// A key is live only while the effect it keys is in the chain and switched
// on; a switched-off effect keeps its key saved for when it returns.
[[nodiscard]] bool live_sidechain(const project::Snapshot& snapshot,
                                  const project::SendRoute& send) {
  if (send.purpose != project::SendPurpose::sidechain || !send.target_insert)
    return false;
  const auto destination =
      std::ranges::find(snapshot.tracks, send.destination, &project::Track::id);
  if (destination == snapshot.tracks.end())
    return false;
  const auto effect =
      std::ranges::find(destination->effects, *send.target_insert, &project::EffectInsert::id);
  return effect != destination->effects.end() && effect->enabled;
}
} // namespace

std::optional<project::InsertSlotId> sidechain_target(const project::Snapshot& snapshot,
                                                      project::TrackId destination) {
  for (const auto& track : snapshot.tracks) {
    for (const auto& send : track.sends) {
      if (send.destination == destination && live_sidechain(snapshot, send))
        return send.target_insert;
    }
  }
  return std::nullopt;
}

namespace {
// The chain a saved track runs, as receipts would describe it: its VST3
// instrument, then each enabled effect in saved order.
[[nodiscard]] std::vector<BoundStage> expected_stages(const project::Track& track) {
  std::vector<BoundStage> result;
  if (const auto* source = std::get_if<project::Vst3PluginInstance>(&track.instrument_source))
    result.push_back({track.id, std::nullopt, StageProcessor::vst3, source->id});
  for (const auto& effect : track.effects) {
    if (!effect.enabled)
      continue;
    if (const auto* plugin = std::get_if<project::Vst3PluginInstance>(&effect.processor))
      result.push_back({track.id, effect.id, StageProcessor::vst3, plugin->id});
    else if (const auto* patch = std::get_if<project::NativeEffectPatch>(&effect.processor))
      result.push_back(
          {track.id, effect.id, StageProcessor::native_effect, std::nullopt, patch->generation});
    else if (const auto* convolution = std::get_if<project::NativeConvolution>(&effect.processor))
      result.push_back({track.id, effect.id, StageProcessor::convolution, std::nullopt,
                        convolution->generation});
  }
  return result;
}

[[nodiscard]] bool same_stage(const BoundStage& expected, const BoundStage& bound) noexcept {
  return expected.track == bound.track && expected.slot == bound.slot &&
         expected.processor == bound.processor && expected.plugin == bound.plugin &&
         (expected.processor == StageProcessor::vst3 || expected.generation == bound.generation);
}
} // namespace

Result ProjectRouteIdRegistry::compile(const project::Snapshot& snapshot,
                                       std::uint32_t max_compensation_frames,
                                       std::span<const BoundStage> stages,
                                       bool convolution_runtime_available) {
  using foundation::ErrorCode;
  if (project::has_convolution_insert(snapshot) && !convolution_runtime_available)
    return fail(ErrorCode::unsupported,
                "Convolution insert is saved but has no prepared routing runtime");
  // The engine's bounded latency planner accepts 1024 nodes and 4096 edges.
  // Reserve one node for master and one edge per track's main output.
  if (snapshot.tracks.size() > 1023) {
    return fail(ErrorCode::resource_exhausted, "routing plan exceeds 1023 track nodes");
  }

  std::map<project::TrackId, const project::Track*> ordered_tracks;
  std::set<project::SendId> seen_sends;
  std::size_t edge_count = snapshot.tracks.size();
  for (const auto& track : snapshot.tracks) {
    if (!ordered_tracks.emplace(track.id, &track).second) {
      return fail(ErrorCode::conflict, "duplicate routing track " + track.id.to_string());
    }
    if (track.sends.size() > project::max_sends_per_track ||
        track.sends.size() > 4096 - edge_count) {
      return fail(ErrorCode::resource_exhausted, "routing plan exceeds send or edge limit");
    }
    edge_count += track.sends.size();
    for (const auto& send : track.sends) {
      if (!seen_sends.insert(send.id).second) {
        return fail(ErrorCode::conflict, "duplicate send route " + send.id.to_string());
      }
      if (!std::isfinite(send.gain) || send.gain < 0.0 || send.gain > 4.0) {
        return fail(ErrorCode::invalid_argument,
                    "send gain must be finite and 0..4: " + send.id.to_string());
      }
    }
  }

  // Every track's chain must be exactly its receipts, in order.
  std::map<project::TrackId, std::uint64_t> chain_latency;
  std::size_t bound = 0;
  for (const auto& track : snapshot.tracks) {
    if (track.kind != project::TrackKind::instrument &&
        !std::holds_alternative<std::monostate>(track.instrument_source))
      return fail(ErrorCode::unsupported, "non-instrument path contains an instrument source");
    const auto expected = expected_stages(track);
    if (expected.size() > max_stages_per_path)
      return fail(ErrorCode::unsupported, "track " + track.id.to_string() + " exceeds its " +
                                              std::to_string(max_stages_per_path) + "-stage chain");
    auto& latency = chain_latency[track.id];
    for (const auto& stage : expected) {
      if (bound >= stages.size() || !same_stage(stage, stages[bound]))
        return fail(ErrorCode::unsupported, "saved processor on track " + track.id.to_string() +
                                                " has no matching ordered prepared stage");
      latency += stages[bound++].latency_frames;
    }
    if (latency > std::numeric_limits<std::uint32_t>::max())
      return fail(ErrorCode::resource_exhausted,
                  "track " + track.id.to_string() + " chain latency exceeds 32-bit frame range");
  }
  if (bound != stages.size())
    return fail(ErrorCode::invalid_argument, "bound stage is absent from saved routing");

  // Preserve route-specific diagnostics before asking the project model to
  // validate and project its complete saved processing intent.
  for (const auto& [track_id, track] : ordered_tracks) {
    if (track->main_output) {
      const auto found = ordered_tracks.find(*track->main_output);
      if (found == ordered_tracks.end()) {
        return fail(ErrorCode::not_found,
                    "main routing destination is missing for " + track_id.to_string());
      }
      if (found->second->kind != project::TrackKind::group_bus) {
        return fail(ErrorCode::invalid_argument,
                    "main routing destination must be a group bus: " + track_id.to_string());
      }
    }
    for (const auto& send : track->sends) {
      const auto found = ordered_tracks.find(send.destination);
      if (found == ordered_tracks.end()) {
        return fail(ErrorCode::not_found, "send destination is missing: " + send.id.to_string());
      }
      if (send.purpose == project::SendPurpose::audible &&
          found->second->kind != project::TrackKind::group_bus &&
          found->second->kind != project::TrackKind::return_bus) {
        return fail(ErrorCode::invalid_argument,
                    "send destination must be a group or return bus: " + send.id.to_string());
      }
      if (send.purpose != project::SendPurpose::sidechain)
        continue;
      const auto effect = send.target_insert
                              ? std::ranges::find(found->second->effects, *send.target_insert,
                                                  &project::EffectInsert::id)
                              : found->second->effects.end();
      if (effect == found->second->effects.end())
        return fail(ErrorCode::not_found,
                    "sidechain target slot is missing: " + send.id.to_string());
      if (!effect->enabled)
        continue; // Saved for when the effect is switched back on.
      // Whether a stage reads a detector is known only once it is prepared.
      if (std::ranges::none_of(stages, [&](const BoundStage& stage) {
            return stage.track == send.destination && stage.slot == send.target_insert &&
                   stage.detector;
          }))
        return fail(ErrorCode::unsupported,
                    "sidechain target needs an effect that reads a detector: " +
                        send.id.to_string());
    }
  }

  auto projected = project::project_insert_chains(snapshot);
  if (!projected)
    return fail(projected.error().code, "saved routing projection: " + projected.error().message);
  // Conservative bound: even if every entity is new, the ID allocator cannot
  // wrap during this transaction. Reused identities consume no IDs.
  // The upper half of the ID space is reserved for verified plugin processor
  // identities. Route and plugin IDs must never alias during a session.
  constexpr auto plugin_id_base = std::uint64_t{1} << 63U;
  if (next_id_ >= plugin_id_base ||
      ordered_tracks.size() + edge_count >= plugin_id_base - next_id_) {
    return fail(ErrorCode::resource_exhausted, "session routing ID space exhausted");
  }

  auto tracks = tracks_;
  auto main_edges = main_edges_;
  auto send_edges = send_edges_;
  auto next = next_id_;
  ProjectRoutePlan plan;
  plan.master = 1;
  plan.nodes.reserve(ordered_tracks.size() + 1);
  plan.edges.reserve(edge_count);
  plan.sends.reserve(seen_sends.size());
  plan.track_nodes.reserve(ordered_tracks.size());
  plan.nodes.push_back({plan.master, 0});
  foundation::trace::label("graph.node", plan.master, "Master");
  std::map<engine::RoutingNodeId, project::TrackId> source_tracks;
  for (const auto& [track_id, track] : ordered_tracks) {
    const auto id = stable_id(tracks, track_id.bits(), next);
    plan.track_nodes.push_back({track_id, id});
    // Trace names, only while recording: a node's span by its id, a source's by its track.
    if (foundation::trace::enabled()) {
      const auto name =
          track->name.empty() ? "Track " + std::to_string(plan.track_nodes.size()) : track->name;
      foundation::trace::label("graph.node", id, name);
      foundation::trace::label("graph.source", track_id.bits().low, name + " (source)");
    }
    const auto latency = chain_latency.at(track_id);
    plan.nodes.push_back({id, static_cast<std::uint32_t>(latency)});
    if (track->kind == project::TrackKind::audio || track->kind == project::TrackKind::instrument) {
      source_tracks.emplace(id, track_id);
    }
  }

  for (const auto& [track_id, track] : ordered_tracks) {
    const auto source = tracks.at(track_id.bits());
    engine::RoutingNodeId destination = plan.master;
    if (track->main_output) {
      destination = tracks.at(track->main_output->bits());
    }
    const auto main_id = stable_id(main_edges, track_id.bits(), next);
    plan.edges.push_back({main_id, source, destination, 1.0F});

    std::map<project::SendId, const project::SendRoute*> ordered_sends;
    for (const auto& send : track->sends) {
      ordered_sends.emplace(send.id, &send);
    }
    for (const auto& [send_id, send] : ordered_sends) {
      if (send->purpose == project::SendPurpose::sidechain && !live_sidechain(snapshot, *send))
        continue;
      const auto send_edge_id = stable_id(send_edges, send_id.bits(), next);
      plan.edges.push_back({send_edge_id, source, tracks.at(send->destination.bits()),
                            static_cast<float>(send->gain),
                            send->purpose == project::SendPurpose::sidechain
                                ? engine::RoutingEdgeKind::sidechain
                                : engine::RoutingEdgeKind::main});
      plan.sends.push_back({track_id, send_id, send_edge_id});
    }
  }

  auto latency = engine::prepare_routing_latency(plan.nodes, plan.edges,
                                                 {plan.master, max_compensation_frames});
  if (!latency) {
    return fail(latency.error().code, "project routing revision " +
                                          std::to_string(snapshot.revision) + ": " +
                                          latency.error().message);
  }
  plan.latency = std::move(latency).value();
  plan.sources.reserve(source_tracks.size());
  for (std::size_t index = 0; index < plan.latency.topological_order.size(); ++index) {
    const auto node = plan.latency.topological_order[index];
    if (const auto found = source_tracks.find(node); found != source_tracks.end()) {
      plan.sources.push_back({found->second, node, index});
    }
  }
  tracks_ = std::move(tracks);
  main_edges_ = std::move(main_edges);
  send_edges_ = std::move(send_edges);
  next_id_ = next;
  return Result::success(std::move(plan));
}

} // namespace oscillate::routing
