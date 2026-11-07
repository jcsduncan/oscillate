#include "oscillate/test/check.hpp"

#include <oscillate/routing/project_route_plan.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
namespace routing = oscillate::routing;
namespace engine = oscillate::engine;
namespace foundation = oscillate::foundation;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;

using oscillate::test::require;

using oscillate::test::take;

void require_failure(const foundation::Result<routing::ProjectRoutePlan>& result,
                     foundation::ErrorCode code, std::string_view message) {
  require(!result && result.error().code == code, message);
}

void require_failure(const foundation::Result<routing::ProjectRoutePlan>& result,
                     foundation::ErrorCode code, std::string_view fragment,
                     std::string_view message) {
  require(!result && result.error().code == code &&
              result.error().message.find(fragment) != std::string::npos,
          message);
}

template <typename Id> Id id(std::uint64_t low, std::uint64_t high = 0) {
  return take(Id::from_bits({high, low}));
}

project::Vst3PluginInstance saved_plugin(project::PluginInstanceId instance,
                                         std::string name = "effect") {
  return {instance,
          std::string(32, 'a'),
          "vendor",
          std::move(name),
          "1",
          "/fixture.vst3",
          std::string(64, 'b'),
          {},
          {},
          false};
}

project::EffectInsert saved_effect(project::Vst3PluginInstance plugin) {
  const auto slot = id<project::InsertSlotId>(plugin.id.bits().low + 1'000);
  return {slot, true, std::move(plugin)};
}

// A built-in effect (ADR-0040) at its engine defaults.
project::NativeEffectPatch effect_patch(std::string engine, std::uint64_t generation = 1) {
  return {std::move(engine), 1, {}, generation};
}

// Receipts for one prepared stage of a track's chain (ADR-0050).
routing::BoundStage instrument_stage(project::TrackId track, project::PluginInstanceId plugin,
                                     std::uint32_t latency = 0) {
  return {.track = track,
          .slot = std::nullopt,
          .processor = routing::StageProcessor::vst3,
          .plugin = plugin,
          .generation = 0,
          .latency_frames = latency,
          .detector = false};
}

routing::BoundStage vst3_stage(project::TrackId track, project::InsertSlotId slot,
                               project::PluginInstanceId plugin, std::uint32_t latency = 0,
                               bool detector = false) {
  return {.track = track,
          .slot = slot,
          .processor = routing::StageProcessor::vst3,
          .plugin = plugin,
          .generation = 0,
          .latency_frames = latency,
          .detector = detector};
}

routing::BoundStage native_stage(project::TrackId track, project::InsertSlotId slot,
                                 std::uint64_t generation, std::uint32_t latency = 0,
                                 bool detector = false) {
  return {.track = track,
          .slot = slot,
          .processor = routing::StageProcessor::native_effect,
          .plugin = std::nullopt,
          .generation = generation,
          .latency_frames = latency,
          .detector = detector};
}

routing::BoundStage convolution_stage(project::TrackId track, project::InsertSlotId slot,
                                      std::uint64_t generation, std::uint32_t latency = 0) {
  return {.track = track,
          .slot = slot,
          .processor = routing::StageProcessor::convolution,
          .plugin = std::nullopt,
          .generation = generation,
          .latency_frames = latency,
          .detector = false};
}

project::Snapshot snapshot() {
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  return {id<project::ProjectId>(1),
          1,
          48'000,
          std::move(tempo),
          std::move(meter),
          {{id<project::TrackId>(2)},
           {id<project::TrackId>(3)},
           {id<project::TrackId>(4), {}, project::TrackKind::group_bus},
           {id<project::TrackId>(5), {}, project::TrackKind::return_bus}}};
}

const engine::RoutingEdge& main_edge(const routing::ProjectRoutePlan& plan,
                                     engine::RoutingNodeId source,
                                     engine::RoutingNodeId destination) {
  const auto found = std::ranges::find_if(plan.edges, [&](const engine::RoutingEdge& edge) {
    return edge.source == source && edge.destination == destination && edge.gain == 1.0F;
  });
  REQUIRE_NE(found, plan.edges.end(), "main routing edge missing");
  return *found;
}

engine::RoutingNodeId track_node(const routing::ProjectRoutePlan& plan, project::TrackId track) {
  const auto found =
      std::ranges::find(plan.track_nodes, track, &routing::ProjectRouteTrackNode::track);
  REQUIRE_NE(found, plan.track_nodes.end(), "routed track node missing");
  return found->node;
}

std::uint32_t node_latency(const routing::ProjectRoutePlan& plan, project::TrackId track) {
  const auto node =
      std::ranges::find(plan.nodes, track_node(plan, track), &engine::RoutingNode::id);
  REQUIRE_NE(node, plan.nodes.end(), "routing node missing");
  return node->processor_latency_frames;
}

std::uint32_t master_node_latency(const routing::ProjectRoutePlan& plan) {
  const auto node = std::ranges::find(plan.nodes, plan.master, &engine::RoutingNode::id);
  REQUIRE_NE(node, plan.nodes.end(), "master routing node missing");
  return node->processor_latency_frames;
}

void stable_ids_and_order() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  state.tracks[0].main_output = state.tracks[2].id;
  state.tracks[0].sends.push_back({id<project::SendId>(10), state.tracks[3].id, 0.25});
  auto first = take(registry.compile(state, 128));
  REQUIRE_EQ(first.nodes.size(), 5, "main and send topology missing");
  REQUIRE_EQ(first.edges.size(), 5, "main and send topology missing");
  REQUIRE_EQ(first.master, 1, "routing master or zero-latency plan wrong");
  REQUIRE_EQ(first.latency.master_output_latency_frames, 0,
             "routing master or zero-latency plan wrong");
  REQUIRE_EQ(first.sources.size(), 2, "source bindings must follow stable topological order");
  REQUIRE_EQ(first.sources[0].track, state.tracks[0].id,
             "source bindings must follow stable topological order");
  REQUIRE_EQ(first.sources[1].track, state.tracks[1].id,
             "source bindings must follow stable topological order");
  const auto first_audio_node = first.sources[0].node;
  const auto first_main = main_edge(first, first_audio_node, first.nodes[3].id).id;
  const auto first_send = std::ranges::find_if(
      first.edges, [](const engine::RoutingEdge& edge) { return edge.gain == 0.25F; });
  REQUIRE_NE(first_send, first.edges.end(), "send gain not carried into plan");
  const auto send_id = first_send->id;
  REQUIRE_NE(send_id, first_main, "main and send edges must have distinct identities");
  REQUIRE_EQ(first.sends.size(), 1, "stable send identity was not exposed for automation binding");
  REQUIRE_EQ(first.sends[0].send, id<project::SendId>(10),
             "stable send identity was not exposed for automation binding");
  REQUIRE_EQ(first.sends[0].track, state.tracks[0].id,
             "stable send identity was not exposed for automation binding");
  REQUIRE_EQ(first.sends[0].edge, send_id,
             "stable send identity was not exposed for automation binding");

  state.tracks[0].sends.front().gain = 0.75;
  auto changed_gain = take(registry.compile(state, 128));
  require(std::ranges::any_of(changed_gain.edges,
                              [send_id](const engine::RoutingEdge& edge) {
                                return edge.id == send_id && edge.gain == 0.75F;
                              }),
          "gain edit changed send identity or failed to update gain");
  state.tracks[0].sends.front().gain = 0.25;

  std::ranges::reverse(state.tracks);
  auto reordered = take(registry.compile(state, 64));
  REQUIRE_EQ(reordered.sources[0].node, first_audio_node,
             "reordering changed stable node or main edge ID");
  REQUIRE_EQ(main_edge(reordered, first_audio_node, first.nodes[3].id).id, first_main,
             "reordering changed stable node or main edge ID");
  REQUIRE_EQ(reordered.edges.size(), first.edges.size(),
             "unchanged routing should compile deterministically");
  REQUIRE_TRUE(
      std::ranges::equal(reordered.edges, first.edges,
                         [](const engine::RoutingEdge& left, const engine::RoutingEdge& right) {
                           return left.id == right.id && left.source == right.source &&
                                  left.destination == right.destination && left.gain == right.gain;
                         }),
      "unchanged routing should compile deterministically");
  REQUIRE_EQ(reordered.sources[0].topological_index, first.sources[0].topological_index,
             "unchanged routing should compile deterministically");

  const auto audio_it = std::ranges::find_if(state.tracks, [](const project::Track& track) {
    return track.id == id<project::TrackId>(2);
  });
  REQUIRE_NE(audio_it, state.tracks.end(), "audio track missing");
  audio_it->sends.clear();
  auto removed_send = take(registry.compile(state, 128));
  REQUIRE_EQ(removed_send.edges.size(), 4, "removed send still compiled");
  audio_it->sends.push_back({id<project::SendId>(10), id<project::TrackId>(5), 0.25});
  auto restored = take(registry.compile(state, 128));
  require(std::ranges::any_of(
              restored.edges,
              [send_id](const engine::RoutingEdge& edge) { return edge.id == send_id; }),
          "re-added same send lost its session identity");

  const auto added_track = id<project::TrackId>(90);
  state.tracks.emplace_back(added_track);
  auto with_track = take(registry.compile(state, 128));
  const auto added_binding = std::ranges::find_if(
      with_track.sources, [added_track](const routing::ProjectRouteSource& binding) {
        return binding.track == added_track;
      });
  REQUIRE_NE(added_binding, with_track.sources.end(), "added track source binding missing");
  const auto added_node = added_binding->node;
  state.tracks.pop_back();
  REQUIRE_OK(registry.compile(state, 128));
  state.tracks.emplace(state.tracks.begin(), added_track);
  auto readded = take(registry.compile(state, 128));
  require(std::ranges::any_of(readded.sources,
                              [&](const routing::ProjectRouteSource& binding) {
                                return binding.track == added_track && binding.node == added_node;
                              }),
          "re-added track lost its session node identity");
}

void full_project_ids_and_parallel_edges() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  state.tracks[1].id = id<project::TrackId>(2, 1); // Same low half as first track.
  state.tracks[0].main_output = state.tracks[2].id;
  state.tracks[0].sends.push_back({id<project::SendId>(20), state.tracks[2].id, 1.0});
  state.tracks[0].sends.push_back({id<project::SendId>(21), state.tracks[2].id, 0.0});
  auto plan = take(registry.compile(state, 0));
  REQUIRE_EQ(plan.sources.size(), 2, "128-bit track IDs collided when mapped to engine IDs");
  REQUIRE_NE(plan.sources[0].node, plan.sources[1].node,
             "128-bit track IDs collided when mapped to engine IDs");
  const auto source = plan.sources[0].node;
  const auto parallel =
      std::ranges::count_if(plan.edges, [source](const engine::RoutingEdge& edge) {
        return edge.source == source && edge.destination != 1;
      });
  REQUIRE_EQ(parallel, 3, "distinct main/send parallel routes collapsed");
  require(std::ranges::any_of(plan.edges,
                              [](const engine::RoutingEdge& edge) { return edge.gain == 0.0F; }),
          "zero gain send should retain its identity for later edits");
}

void errors_do_not_consume_ids() {
  auto state = snapshot();
  routing::ProjectRouteIdRegistry registry;
  auto expected = take(registry.compile(state, 0));
  state.tracks[0].sends.push_back({id<project::SendId>(30), id<project::TrackId>(99), 1.0});
  const auto missing = registry.compile(state, 0);
  REQUIRE_TRUE(!missing, "missing endpoint should fail with not-found");
  REQUIRE_EQ(missing.error().code, foundation::ErrorCode::not_found,
             "missing endpoint should fail with not-found");
  state.tracks[0].sends.clear();
  state.tracks[0].sends.push_back({id<project::SendId>(31), state.tracks[3].id, 0.5});
  auto valid = take(registry.compile(state, 0));
  const auto new_send = std::ranges::find_if(
      valid.edges, [](const engine::RoutingEdge& edge) { return edge.gain == 0.5F; });
  REQUIRE_NE(new_send, valid.edges.end(), "failed build consumed a session edge ID");
  REQUIRE_EQ(new_send->id, expected.edges.back().id + 1, "failed build consumed a session edge ID");

  state.tracks[0].sends.push_back(state.tracks[0].sends.front());
  REQUIRE_EQ(registry.compile(state, 0).error().code, foundation::ErrorCode::conflict,
             "duplicate send ID accepted");
  state.tracks[0].sends.pop_back();
  state.tracks[0].sends.front().gain = 5.0;
  REQUIRE_EQ(registry.compile(state, 0).error().code, foundation::ErrorCode::invalid_argument,
             "out-of-range gain accepted");
  state.tracks[0].sends.front().gain = 0.5;
  state.tracks[0].sends.front().destination = state.tracks[1].id;
  REQUIRE_EQ(registry.compile(state, 0).error().code, foundation::ErrorCode::invalid_argument,
             "send to ordinary audio track accepted");
  state.tracks[0].sends.front().destination = state.tracks[3].id;
  state.tracks[2].main_output = state.tracks[2].id;
  REQUIRE_EQ(registry.compile(state, 0).error().code, foundation::ErrorCode::conflict,
             "routing cycle accepted");
  state.tracks[2].main_output.reset();
  state.tracks[0].effects.push_back(saved_effect(saved_plugin(id<project::PluginInstanceId>(50))));
  auto unsupported = registry.compile(state, 0);
  require(!unsupported && unsupported.error().code == foundation::ErrorCode::unsupported,
          !unsupported ? unsupported.error().message
                       : "unrepresented track plugin composition accepted");
}

void structural_limits_and_main_destinations() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  state.tracks.push_back(state.tracks.front());
  REQUIRE_EQ(registry.compile(state, 0).error().code, foundation::ErrorCode::conflict,
             "duplicate track ID accepted");
  state.tracks.pop_back();

  state.tracks[0].main_output = id<project::TrackId>(99);
  REQUIRE_EQ(registry.compile(state, 0).error().code, foundation::ErrorCode::not_found,
             "missing main destination accepted");
  state.tracks[0].main_output = state.tracks[1].id;
  REQUIRE_EQ(registry.compile(state, 0).error().code, foundation::ErrorCode::invalid_argument,
             "ordinary audio track accepted as main destination");
  state.tracks[0].main_output.reset();

  for (std::uint64_t number = 1; number <= project::max_sends_per_track + 1; ++number) {
    state.tracks[0].sends.push_back({id<project::SendId>(number), state.tracks[3].id, 1.0});
  }
  REQUIRE_EQ(registry.compile(state, 0).error().code, foundation::ErrorCode::resource_exhausted,
             "per-track send limit ignored");
  state.tracks[0].sends.clear();

  for (std::uint64_t number = 100; number < 1124; ++number) {
    state.tracks.emplace_back(id<project::TrackId>(number));
  }
  REQUIRE_EQ(registry.compile(state, 0).error().code, foundation::ErrorCode::resource_exhausted,
             "engine node limit ignored");
}

void verified_insert_latency_and_fail_closed_binding() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  const auto track = state.tracks[0].id;
  const auto plugin = id<project::PluginInstanceId>(40);
  state.tracks[0].effects.push_back(saved_effect(saved_plugin(plugin)));
  const auto slot = state.tracks[0].effects.front().id;
  REQUIRE_EQ(registry.compile(state, 64).error().code, foundation::ErrorCode::unsupported,
             "saved insert compiled without a verified binding");
  REQUIRE_EQ(registry
                 .compile(state, 64,
                          std::array{vst3_stage(track, slot, id<project::PluginInstanceId>(41), 7)})
                 .error()
                 .code,
             foundation::ErrorCode::unsupported, "wrong plugin receipt was accepted");
  const std::array bound{vst3_stage(track, slot, plugin, 7)};
  auto routed = take(registry.compile(state, 64, bound));
  const auto first_node = routed.sources[0].node;
  const auto second_node = routed.sources[1].node;
  REQUIRE_NE(std::ranges::find_if(routed.nodes,
                                  [first_node](const auto& node) {
                                    return node.id == first_node &&
                                           node.processor_latency_frames == 7;
                                  }),
             routed.nodes.end(), "verified insert latency was not assigned to its own track");
  REQUIRE_NE(std::ranges::find_if(routed.latency.edges,
                                  [second_node](const auto& edge) {
                                    return edge.source == second_node &&
                                           edge.compensation_frames == 7;
                                  }),
             routed.latency.edges.end(), "unprocessed parallel track was not compensated");
  REQUIRE_EQ(registry.compile(state, 6, bound).error().code,
             foundation::ErrorCode::resource_exhausted, "over-budget insert latency was published");
  state.tracks[1].effects.push_back(saved_effect(saved_plugin(id<project::PluginInstanceId>(42))));
  REQUIRE_EQ(registry.compile(state, 64, bound).error().code, foundation::ErrorCode::unsupported,
             "second insert was silently skipped");
}

void two_verified_audio_effect_receipts_are_independent() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  const auto first_track = state.tracks[0].id;
  const auto second_track = state.tracks[1].id;
  const auto first_plugin = id<project::PluginInstanceId>(40);
  const auto second_plugin = id<project::PluginInstanceId>(41);
  state.tracks[0].effects.push_back(saved_effect(saved_plugin(first_plugin, "effect A")));
  state.tracks[1].effects.push_back(saved_effect(saved_plugin(second_plugin, "effect B")));
  const std::array bound{
      vst3_stage(first_track, state.tracks[0].effects.front().id, first_plugin, 7),
      vst3_stage(second_track, state.tracks[1].effects.front().id, second_plugin, 11)};
  REQUIRE_EQ(registry.compile(state, 64, std::span(bound).first(1)).error().code,
             foundation::ErrorCode::unsupported,
             "unbound saved second effect was silently skipped");
  auto plan = take(registry.compile(state, 64, bound));
  const auto first_source =
      std::ranges::find(plan.sources, first_track, &routing::ProjectRouteSource::track);
  const auto second_source =
      std::ranges::find(plan.sources, second_track, &routing::ProjectRouteSource::track);
  REQUIRE_NE(first_source, plan.sources.end(),
             "distinct saved effects collapsed onto one source node");
  REQUIRE_NE(second_source, plan.sources.end(),
             "distinct saved effects collapsed onto one source node");
  REQUIRE_NE(first_source->node, second_source->node,
             "distinct saved effects collapsed onto one source node");
  const auto first_node = first_source->node;
  const auto second_node = second_source->node;
  const auto latency_of = [&](engine::RoutingNodeId node) {
    return std::ranges::find(plan.nodes, node, &engine::RoutingNode::id)->processor_latency_frames;
  };
  REQUIRE_EQ(latency_of(first_node), 7,
             "reported effect latencies were not bound to their own tracks");
  REQUIRE_EQ(latency_of(second_node), 11,
             "reported effect latencies were not bound to their own tracks");
  const auto compensation_of = [&](engine::RoutingNodeId node) {
    return std::ranges::find_if(plan.latency.edges,
                                [&](const auto& edge) {
                                  return edge.source == node && edge.destination == plan.master;
                                })
        ->compensation_frames;
  };
  // Independent longest-path oracle: arrivals at 7 and 11 frames require
  // four extra frames on the first track, and none on the second.
  require(compensation_of(first_node) == 4 && compensation_of(second_node) == 0,
          "two-effect parallel paths were not aligned to frame 11");
  const auto first_main = main_edge(plan, first_node, plan.master).id;
  const auto second_main = main_edge(plan, second_node, plan.master).id;
  REQUIRE_NE(first_main, second_main, "parallel effect route IDs aliased");

  REQUIRE_EQ(registry.compile(state, 3, bound).error().code,
             foundation::ErrorCode::resource_exhausted,
             "four-frame compensation exceeded the caller's three-frame budget");
  std::ranges::reverse(state.tracks);
  // Receipts follow the saved track order, so reordering tracks reorders them.
  REQUIRE_EQ(registry.compile(state, 64, bound).error().code, foundation::ErrorCode::unsupported,
             "receipts out of saved track order were accepted");
  auto reversed = bound;
  std::ranges::reverse(reversed);
  auto reordered = take(registry.compile(state, 64, reversed));
  REQUIRE_EQ(main_edge(reordered, first_node, reordered.master).id, first_main,
             "failed plan or manifest reordering changed stable route identity");
  REQUIRE_EQ(main_edge(reordered, second_node, reordered.master).id, second_main,
             "failed plan or manifest reordering changed stable route identity");

  auto wrong = reversed;
  wrong[0].plugin = id<project::PluginInstanceId>(99);
  REQUIRE_EQ(registry.compile(state, 64, wrong).error().code, foundation::ErrorCode::unsupported,
             "mismatched effect receipt was accepted");
  auto duplicate_track = reversed;
  duplicate_track[0].track = duplicate_track[1].track;
  REQUIRE_EQ(registry.compile(state, 64, duplicate_track).error().code,
             foundation::ErrorCode::unsupported,
             "receipt bound to the wrong saved track was accepted");
  auto duplicate_plugin = reversed;
  duplicate_plugin[0].plugin = duplicate_plugin[1].plugin;
  REQUIRE_EQ(registry.compile(state, 64, duplicate_plugin).error().code,
             foundation::ErrorCode::unsupported,
             "receipt naming another track's plugin was accepted");
  auto extra = reversed;
  extra[0].track = id<project::TrackId>(5);
  REQUIRE_EQ(registry.compile(state, 64, extra).error().code, foundation::ErrorCode::unsupported,
             "receipt for another track was accepted in place of a saved effect");
  auto as_instrument = reversed;
  as_instrument[0].slot.reset();
  REQUIRE_EQ(registry.compile(state, 64, as_instrument).error().code,
             foundation::ErrorCode::unsupported,
             "effect receipt was accepted as an instrument stage");
  auto empty = snapshot();
  REQUIRE_EQ(registry.compile(empty, 64, bound).error().code,
             foundation::ErrorCode::invalid_argument,
             "orphan verified receipts were accepted without saved inserts");
  auto third = state;
  const auto third_track = id<project::TrackId>(6);
  const auto third_plugin = id<project::PluginInstanceId>(42);
  third.tracks.emplace_back(third_track);
  third.tracks.back().effects.push_back(saved_effect(saved_plugin(third_plugin, "effect C")));
  const std::array three_bound{
      reversed[0], reversed[1],
      vst3_stage(third_track, third.tracks.back().effects.front().id, third_plugin, 1)};
  const auto three = take(registry.compile(third, 64, three_bound));
  const auto third_node = track_node(three, third_track);
  REQUIRE_NE(third_node, first_node,
             "independent third effect was accidentally serialized into another path");
  REQUIRE_NE(third_node, second_node,
             "independent third effect was accidentally serialized into another path");
  REQUIRE_EQ(node_latency(three, third_track), 1,
             "independent third effect was accidentally serialized into another path");
  REQUIRE_EQ(node_latency(three, first_track), 7,
             "independent third effect was accidentally serialized into another path");
  REQUIRE_EQ(node_latency(three, second_track), 11,
             "independent third effect was accidentally serialized into another path");
}

void saved_projection_shapes_fail_closed() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  const auto track = state.tracks[0].id;
  const auto effect = id<project::PluginInstanceId>(70);
  state.tracks[0].effects.push_back(saved_effect(saved_plugin(effect)));
  const std::array bound{vst3_stage(track, state.tracks[0].effects.front().id, effect, 5)};
  auto& saved = std::get<project::Vst3PluginInstance>(state.tracks[0].effects.front().processor);
  saved.missing = true;
  REQUIRE_OK(registry.compile(state, 64, bound),
             "fresh verified effect binding did not recover a stale saved missing flag");
  REQUIRE_EQ(registry.compile(state, 64).error().code, foundation::ErrorCode::unsupported,
             "unbound missing saved effect was silently skipped");
  saved.missing = false;

  saved.binary_sha256.clear();
  REQUIRE_EQ(registry.compile(state, 64, bound).error().code,
             foundation::ErrorCode::invalid_argument,
             "malformed saved plugin identity bypassed snapshot projection validation");
  saved.binary_sha256 = std::string(64, 'b');
  state.master_effects.push_back({id<project::InsertSlotId>(270), true, effect_patch("press", 1)});
  const auto with_master = take(registry.compile(state, 64, bound));
  REQUIRE_EQ(master_node_latency(with_master), 0,
             "master effects changed route latency; the caller owns the master chain");
  REQUIRE_EQ(node_latency(with_master, track), 5,
             "master effects changed route latency; the caller owns the master chain");
  state.master_effects.front().enabled = false;
  REQUIRE_OK(registry.compile(state, 64, bound),
             "bypassed native master slot rejected the saved route");
  state.master_effects.front().enabled = true;
  state.master_effects.push_back({id<project::InsertSlotId>(271), true, effect_patch("shape", 1)});
  REQUIRE_OK(registry.compile(state, 64, bound),
             "reordered native master chain rejected the saved route");
  state.master_effects.push_back({id<project::InsertSlotId>(272), true, effect_patch("shape", 1)});
  REQUIRE_OK(registry.compile(state, 64, bound), "a repeated built-in master effect was rejected");
  state.master_effects.pop_back();
  state.master_effects.pop_back();
  const auto master_plugin = id<project::PluginInstanceId>(74);
  const auto master_slot = id<project::InsertSlotId>(273);
  state.master_effects.push_back({master_slot, false, saved_plugin(master_plugin)});
  REQUIRE_OK(registry.compile(state, 64, bound),
             "bypassed VST3 master slot rejected a routed render");
  state.master_effects.back().enabled = true;
  const auto vst3_master = take(registry.compile(state, 64, bound));
  REQUIRE_EQ(master_node_latency(vst3_master), 0,
             "an enabled VST3 master insert was rejected or given route latency");
  require_failure(
      registry.compile(state, 64,
                       std::array{bound[0], vst3_stage(track, master_slot, master_plugin, 3)}),
      foundation::ErrorCode::invalid_argument,
      "a master effect receipt was accepted as a track stage");
  state.master_effects.pop_back();

  state.tracks[0].effects.front().enabled = false;
  auto bypassed = take(registry.compile(state, 64));
  REQUIRE_EQ(node_latency(bypassed, track), 0,
             "single bypassed VST3 slot retained processor latency");
  REQUIRE_EQ(bypassed.latency.master_output_latency_frames, 0,
             "single bypassed VST3 slot retained processor latency");
  REQUIRE_EQ(registry.compile(state, 64, bound).error().code,
             foundation::ErrorCode::invalid_argument, "bypassed slot accepted a runtime binding");
  state.tracks[0].effects.front().enabled = true;

  state.tracks[0].effects.push_back(saved_effect(saved_plugin(id<project::PluginInstanceId>(71))));
  REQUIRE_EQ(registry.compile(state, 64, bound).error().code, foundation::ErrorCode::unsupported,
             "two saved effects on one track were partially routed without both receipts");
  state.tracks[0].effects.pop_back();

  state.tracks[0].kind = project::TrackKind::group_bus;
  REQUIRE_OK(registry.compile(state, 64, bound),
             "verified bus effect was rejected by the shared route path");
  state.tracks[0].kind = project::TrackKind::instrument;
  state.tracks[0].effects.clear();
  const std::array instrument_bound{instrument_stage(track, effect, 5)};
  state.tracks[0].instrument_source = saved_plugin(effect);
  std::get<project::Vst3PluginInstance>(state.tracks[0].instrument_source).missing = true;
  REQUIRE_OK(registry.compile(state, 64, instrument_bound),
             "fresh verified instrument binding did not recover a stale saved missing flag");
  REQUIRE_EQ(registry.compile(state, 64).error().code, foundation::ErrorCode::unsupported,
             "unbound missing saved instrument was silently skipped");
  for (const auto kind :
       {project::TrackKind::audio, project::TrackKind::group_bus, project::TrackKind::return_bus}) {
    state.tracks[0].kind = kind;
    require_failure(registry.compile(state, 64, instrument_bound),
                    foundation::ErrorCode::unsupported, "instrument source",
                    "a non-instrument track ran an instrument source");
  }
  state.tracks[0].instrument_source = project::NativeInstrumentPatch{"prism", 1, {}};
  require_failure(registry.compile(state, 64), foundation::ErrorCode::unsupported,
                  "instrument source", "a bus ran a built-in instrument source");
}

void bypassed_native_track_eq_needs_no_runtime_receipt() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  state.tracks[0].effects.push_back(
      {id<project::InsertSlotId>(300), false, effect_patch("shape", 1)});
  REQUIRE_OK(registry.compile(state, 64),
             "bypassed native track EQ required an active runtime receipt");
  state.tracks[0].effects.front().enabled = true;
  REQUIRE_EQ(registry.compile(state, 64).error().code, foundation::ErrorCode::unsupported,
             "enabled native track EQ was silently skipped without a prepared receipt");
}

void bypassed_missing_vst3_between_native_stages_is_inert() {
  for (const std::size_t owner_index : {0U, 2U}) {
    routing::ProjectRouteIdRegistry registry;
    auto state = snapshot();
    auto& owner = state.tracks[owner_index];
    const auto eq_slot = id<project::InsertSlotId>(900);
    const auto bypass_slot = id<project::InsertSlotId>(901);
    const auto compressor_slot = id<project::InsertSlotId>(902);
    auto missing = saved_plugin(id<project::PluginInstanceId>(903));
    missing.missing = true;
    missing.bundle_path = "/missing/Broken.vst3";
    owner.effects = {{eq_slot, true, effect_patch("shape", 4)},
                     {bypass_slot, false, missing},
                     {compressor_slot, true, effect_patch("press", 8)}};
    const std::array native{native_stage(owner.id, eq_slot, 4),
                            native_stage(owner.id, compressor_slot, 8)};
    auto prepared = take(registry.compile(state, 64, native));
    REQUIRE_EQ(node_latency(prepared, owner.id), 0,
               "bypassed missing VST3 changed native stage order, identity or latency");
    REQUIRE_EQ(owner.effects[0].id, eq_slot,
               "bypassed missing VST3 changed native stage order, identity or latency");
    REQUIRE_EQ(owner.effects[1].id, bypass_slot,
               "bypassed missing VST3 changed native stage order, identity or latency");
    REQUIRE_EQ(owner.effects[2].id, compressor_slot,
               "bypassed missing VST3 changed native stage order, identity or latency");
    REQUIRE_EQ(std::get<project::Vst3PluginInstance>(owner.effects[1].processor), missing,
               "bypassed missing VST3 changed native stage order, identity or latency");
    owner.effects[1].enabled = true;
    REQUIRE_EQ(registry.compile(state, 64, native).error().code, foundation::ErrorCode::unsupported,
               "enabled missing VST3 effect was silently omitted");
  }
}

void ordered_native_route_receipts_require_exact_slots_and_generations() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  const auto bus = state.tracks[2].id;
  const auto eq_slot = id<project::InsertSlotId>(330);
  const auto compressor_slot = id<project::InsertSlotId>(331);
  state.tracks[2].effects = {{eq_slot, true, effect_patch("shape", 4)},
                             {compressor_slot, true, effect_patch("press", 8)}};
  const std::array receipts{native_stage(bus, eq_slot, 4), native_stage(bus, compressor_slot, 8)};
  require(!registry.compile(state, 64), "native bus chain published without receipts");
  auto plan = take(registry.compile(state, 64, receipts));
  REQUIRE_EQ(node_latency(plan, bus), 0, "zero-latency native bus chain changed route latency");
  auto reversed = receipts;
  std::ranges::reverse(reversed);
  require(!registry.compile(state, 64, reversed), "native route accepted reversed processor order");
  auto stale = receipts;
  stale[1].generation = 9;
  require(!registry.compile(state, 64, stale), "native route accepted stale compressor generation");
  state.tracks[2].effects[0].enabled = false;
  const std::array enabled_only{receipts[1]};
  REQUIRE_OK(registry.compile(state, 64, enabled_only),
             "bypassed first stage required a processor receipt");
  state.tracks[2].effects[0].enabled = true;
  state.tracks[2].effects[1].processor = effect_patch("expanse");
  const std::array reverb_receipts{receipts[0], native_stage(bus, compressor_slot, 1)};
  REQUIRE_OK(registry.compile(state, 64, reverb_receipts),
             "zero-latency native bus reverb rejected a prepared ordered receipt");
  require(!registry.compile(state, 64, receipts),
          "route accepted a compressor receipt for a saved reverb");
  state.tracks[2].effects[1].processor = effect_patch("ceiling");
  require(!registry.compile(state, 64, receipts),
          "route silently accepted native limiter without an exact prepared receipt");
  const std::array limiter_receipts{receipts[0], native_stage(bus, compressor_slot, 1, 48)};
  const auto limited = take(registry.compile(state, 48, limiter_receipts));
  REQUIRE_EQ(node_latency(limited, bus), 48,
             "native limiter lookahead was not assigned to its routed node");
  REQUIRE_EQ(limited.latency.master_output_latency_frames, 48,
             "native limiter lookahead was not assigned to its routed node");
  REQUIRE_EQ(registry.compile(state, 47, limiter_receipts).error().code,
             foundation::ErrorCode::resource_exhausted,
             "native limiter parallel path exceeded its PDC budget without rejection");
  auto zero_latency = limiter_receipts;
  zero_latency[1].latency_frames = 0;
  REQUIRE_EQ(take(registry.compile(state, 48, zero_latency)).latency.master_output_latency_frames,
             0, "a zero-latency built-in effect required a delayed receipt");
}

void native_bus_eq_requires_exact_receipt_and_maps_bus_node() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  const auto bus = state.tracks[3].id;
  const auto slot = id<project::InsertSlotId>(305);
  state.tracks[3].effects.push_back({slot, true, effect_patch("shape", 7)});
  REQUIRE_EQ(registry.compile(state, 64).error().code, foundation::ErrorCode::unsupported,
             "enabled bus EQ published without a prepared runtime");
  auto plan = take(registry.compile(state, 64, std::array{native_stage(bus, slot, 7)}));
  const auto owner =
      std::ranges::find(plan.track_nodes, bus, &routing::ProjectRouteTrackNode::track);
  REQUIRE_NE(owner, plan.track_nodes.end(), "bus EQ lost its routed node or became a source slot");
  REQUIRE_NE(owner->node, plan.master, "bus EQ lost its routed node or became a source slot");
  REQUIRE_TRUE(
      std::ranges::none_of(plan.sources, [bus](const auto& source) { return source.track == bus; }),
      "bus EQ lost its routed node or became a source slot");
  REQUIRE_EQ(registry.compile(state, 64, std::array{native_stage(bus, slot, 8)}).error().code,
             foundation::ErrorCode::unsupported, "bus EQ accepted a stale settings generation");
  state.tracks[3].effects.front().enabled = false;
  REQUIRE_OK(registry.compile(state, 64), "bypassed bus EQ required a runtime receipt");
}

void mixed_eq_vst3_receipts_accept_both_saved_orders() {
  routing::ProjectRouteIdRegistry registry;
  for (const std::string_view engine : {"shape", "press", "expanse"}) {
    auto state = snapshot();
    auto& track = state.tracks.front();
    const auto plugin = saved_plugin(id<project::PluginInstanceId>(400));
    const auto vst3 = saved_effect(plugin);
    const project::EffectInsert native_slot{id<project::InsertSlotId>(401), true,
                                            effect_patch(std::string(engine), 3)};
    track.effects = {vst3, native_slot};
    const auto vst3_receipt = vst3_stage(track.id, vst3.id, plugin.id, 7);
    const auto native_receipt = native_stage(track.id, native_slot.id, 3);
    const std::array vst3_first{vst3_receipt, native_receipt};
    const std::array native_first{native_receipt, vst3_receipt};
    const auto reverse = take(registry.compile(state, 7, vst3_first));
    REQUIRE_EQ(reverse.latency.master_output_latency_frames, 7,
               "VST3-before-native route did not retain plugin latency");
    require(!registry.compile(state, 7, native_first),
            "VST3-before-native route accepted receipts in the other order");
    std::ranges::swap(track.effects[0], track.effects[1]);
    const auto forward = take(registry.compile(state, 7, native_first));
    REQUIRE_EQ(forward.latency.master_output_latency_frames, 7,
               "native-before-VST3 route changed plugin latency");
    require(!registry.compile(state, 7, vst3_first),
            "native-before-VST3 route accepted receipts in the other order");
    auto stale = native_first;
    stale[0].generation = 4;
    require(!registry.compile(state, 7, stale),
            "mixed route accepted stale native settings generation");
    auto wrong_kind = native_first;
    wrong_kind[0].processor = routing::StageProcessor::convolution;
    require(!registry.compile(state, 7, wrong_kind),
            "mixed route accepted a native receipt for the wrong processor kind");
    auto wrong_slot = native_first;
    wrong_slot[0].slot = id<project::InsertSlotId>(9'999);
    require(!registry.compile(state, 7, wrong_slot),
            "mixed route accepted a native receipt for another slot");
    const auto third_slot = id<project::InsertSlotId>(402);
    track.effects.push_back({third_slot, true, effect_patch("ceiling")});
    require(!registry.compile(state, 7, native_first),
            "mixed route silently accepted a third processor");
    const std::array three{native_receipt, vst3_receipt, native_stage(track.id, third_slot, 1, 5)};
    REQUIRE_EQ(take(registry.compile(state, 12, three)).latency.master_output_latency_frames, 12,
               "mixed route rejected a prepared third processor or lost its latency");
  }
}

void mixed_limiter_vst3_receipts_sum_latency_and_bind_exact_slot() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  auto& track = state.tracks.front();
  const auto plugin = saved_plugin(id<project::PluginInstanceId>(410));
  const auto vst3 = saved_effect(plugin);
  const project::EffectInsert limiter{id<project::InsertSlotId>(411), true,
                                      effect_patch("ceiling", 3)};
  const auto vst3_receipt = vst3_stage(track.id, vst3.id, plugin.id, 7);
  const auto limiter_receipt = native_stage(track.id, limiter.id, 3, 12);
  for (const bool native_first : {false, true}) {
    track.effects = native_first ? std::vector<project::EffectInsert>{limiter, vst3}
                                 : std::vector<project::EffectInsert>{vst3, limiter};
    const auto receipts = native_first ? std::array{limiter_receipt, vst3_receipt}
                                       : std::array{vst3_receipt, limiter_receipt};
    const auto route = take(registry.compile(state, 19, receipts));
    REQUIRE_EQ(route.latency.master_output_latency_frames, 19,
               "limiter/VST3 route did not sum lookahead and plugin PDC");
    require(!registry.compile(state, 18, receipts),
            "limiter/VST3 route exceeded compensation budget without rejection");
  }
  const std::array receipts{limiter_receipt, vst3_receipt};
  auto wrong = receipts;
  wrong[0].generation = 4;
  require(!registry.compile(state, 19, wrong),
          "limiter/VST3 route accepted stale limiter settings generation");
  wrong = receipts;
  wrong[0].slot = id<project::InsertSlotId>(9'999);
  require(!registry.compile(state, 19, wrong), "limiter/VST3 route accepted wrong native slot");
  wrong = receipts;
  wrong[0].processor = routing::StageProcessor::convolution;
  require(!registry.compile(state, 19, wrong), "limiter/VST3 route accepted wrong native kind");
  track.effects.push_back({id<project::InsertSlotId>(412), true, effect_patch("shape")});
  require(!registry.compile(state, 19, receipts),
          "limiter/VST3 route silently omitted a third stage");
}

void mixed_bus_receipts_preserve_order_and_parallel_pdc() {
  for (const auto kind : {project::TrackKind::group_bus, project::TrackKind::return_bus}) {
    for (const bool native_first : {false, true}) {
      routing::ProjectRouteIdRegistry registry;
      auto state = snapshot();
      auto& bus = state.tracks[2];
      bus.kind = kind;
      if (kind == project::TrackKind::group_bus)
        state.tracks[0].main_output = bus.id;
      else
        state.tracks[0].sends.push_back({id<project::SendId>(421), bus.id, 1.0});
      const auto plugin = saved_plugin(id<project::PluginInstanceId>(422));
      const auto vst3 = saved_effect(plugin);
      const project::EffectInsert limiter{id<project::InsertSlotId>(423), true,
                                          effect_patch("ceiling", 4)};
      bus.effects = native_first ? std::vector<project::EffectInsert>{limiter, vst3}
                                 : std::vector<project::EffectInsert>{vst3, limiter};
      const auto vst3_receipt = vst3_stage(bus.id, vst3.id, plugin.id, 7);
      const auto limiter_receipt = native_stage(bus.id, limiter.id, 4, 12);
      const auto receipts = native_first ? std::array{limiter_receipt, vst3_receipt}
                                         : std::array{vst3_receipt, limiter_receipt};
      const auto native_index = native_first ? 0U : 1U;
      const auto route = take(registry.compile(state, 19, receipts));
      REQUIRE_EQ(node_latency(route, bus.id), 19, "mixed bus omitted VST3 or limiter latency");
      REQUIRE_EQ(route.latency.master_output_latency_frames, 19,
                 "mixed bus omitted VST3 or limiter latency");
      const auto dry_node =
          std::ranges::find(route.sources, state.tracks[1].id, &routing::ProjectRouteSource::track)
              ->node;
      const auto dry_edge = std::ranges::find_if(route.latency.edges, [&](const auto& edge) {
        return edge.source == dry_node && edge.destination == route.master;
      });
      REQUIRE_NE(dry_edge, route.latency.edges.end(),
                 "mixed bus failed to align parallel dry audio");
      REQUIRE_EQ(dry_edge->compensation_frames, 19, "mixed bus failed to align parallel dry audio");
      require(!registry.compile(state, 18, receipts),
              "mixed bus exceeded compensation limit without rejection");
      auto stale = receipts;
      stale[native_index].generation = 5;
      require(!registry.compile(state, 19, stale), "mixed bus accepted stale native generation");
      stale = receipts;
      stale[native_index].slot = id<project::InsertSlotId>(9'999);
      require(!registry.compile(state, 19, stale), "mixed bus accepted wrong native slot");
      stale = receipts;
      stale[native_index].processor = routing::StageProcessor::convolution;
      require(!registry.compile(state, 19, stale), "mixed bus accepted wrong native kind");
      const auto third_slot = id<project::InsertSlotId>(424);
      bus.effects.push_back({third_slot, true, effect_patch("shape")});
      require(!registry.compile(state, 19, receipts), "mixed bus silently omitted a third stage");
      const std::array three{receipts[0], receipts[1], native_stage(bus.id, third_slot, 1)};
      REQUIRE_OK(registry.compile(state, 19, three), "mixed bus rejected a prepared third stage");
      bus.effects = {vst3};
      REQUIRE_OK(registry.compile(state, 19, std::array{vst3_receipt}),
                 "standalone VST3 bus insert was rejected after removing the native stage");
    }
  }
}

void mixed_bus_accepts_each_zero_latency_native_kind() {
  const std::array native_processors{project::EffectProcessor{effect_patch("shape", 1)},
                                     project::EffectProcessor{effect_patch("press")},
                                     project::EffectProcessor{effect_patch("expanse")}};
  for (const auto bus_kind : {project::TrackKind::group_bus, project::TrackKind::return_bus}) {
    for (std::size_t index = 0; index < native_processors.size(); ++index) {
      for (const bool native_first : {false, true}) {
        routing::ProjectRouteIdRegistry registry;
        auto state = snapshot();
        auto& bus = state.tracks[2];
        bus.kind = bus_kind;
        if (bus_kind == project::TrackKind::group_bus)
          state.tracks[0].main_output = bus.id;
        else
          state.tracks[0].sends.push_back({id<project::SendId>(430), bus.id, 1.0});
        const auto plugin = saved_plugin(id<project::PluginInstanceId>(431));
        const auto vst3 = saved_effect(plugin);
        const project::EffectInsert native{id<project::InsertSlotId>(432), true,
                                           native_processors[index]};
        bus.effects = native_first ? std::vector<project::EffectInsert>{native, vst3}
                                   : std::vector<project::EffectInsert>{vst3, native};
        const auto vst3_receipt = vst3_stage(bus.id, vst3.id, plugin.id, 7);
        const auto native_receipt = native_stage(bus.id, native.id, 1);
        const auto receipts = native_first ? std::array{native_receipt, vst3_receipt}
                                           : std::array{vst3_receipt, native_receipt};
        const auto route = take(registry.compile(state, 7, receipts));
        REQUIRE_EQ(node_latency(route, bus.id), 7, "mixed bus native kind omitted plugin latency");
        REQUIRE_EQ(route.latency.master_output_latency_frames, 7,
                   "mixed bus native kind omitted plugin latency");
      }
    }
  }
}

void two_serial_effects_sum_latency_and_bind_slots() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  const auto wet = state.tracks[0].id;
  const auto dry = state.tracks[1].id;
  const auto first = id<project::PluginInstanceId>(80);
  const auto second = id<project::PluginInstanceId>(81);
  state.tracks[0].effects.push_back(saved_effect(saved_plugin(first)));
  state.tracks[0].effects.push_back(saved_effect(saved_plugin(second)));
  const auto first_slot = state.tracks[0].effects[0].id;
  const auto second_slot = state.tracks[0].effects[1].id;
  std::array bound{vst3_stage(wet, first_slot, first, 7), vst3_stage(wet, second_slot, second, 11)};
  auto swapped = bound;
  std::ranges::reverse(swapped);
  REQUIRE_EQ(registry.compile(state, 18, swapped).error().code, foundation::ErrorCode::unsupported,
             "serial receipts were accepted out of saved order");
  auto plan = take(registry.compile(state, 18, bound));
  const auto wet_node =
      std::ranges::find(plan.sources, wet, &routing::ProjectRouteSource::track)->node;
  const auto dry_node =
      std::ranges::find(plan.sources, dry, &routing::ProjectRouteSource::track)->node;
  REQUIRE_EQ(node_latency(plan, wet), 18, "serial effects did not advertise their summed latency");
  REQUIRE_NE(main_edge(plan, dry_node, plan.master).id, 0,
             "serial effects did not advertise their summed latency");
  const auto dry_edge = std::ranges::find_if(plan.latency.edges, [&](const auto& edge) {
    return edge.source == dry_node && edge.destination == plan.master;
  });
  REQUIRE_NE(dry_edge, plan.latency.edges.end(),
             "parallel dry path was not aligned to the two-stage wet path");
  REQUIRE_EQ(dry_edge->compensation_frames, 18,
             "parallel dry path was not aligned to the two-stage wet path");
  REQUIRE_EQ(registry.compile(state, 17, bound).error().code,
             foundation::ErrorCode::resource_exhausted,
             "serial latency exceeded the PDC budget without rejection");
  const auto third = id<project::PluginInstanceId>(82);
  state.tracks[0].effects.push_back(saved_effect(saved_plugin(third)));
  const auto third_slot = state.tracks[0].effects.back().id;
  const std::array three_bound{bound[0], bound[1], vst3_stage(wet, third_slot, third, 5)};
  auto three = take(registry.compile(state, 23, three_bound));
  REQUIRE_EQ(
      std::ranges::find(three.nodes, wet_node, &engine::RoutingNode::id)->processor_latency_frames,
      23, "three serial effect latencies were not summed");
  REQUIRE_EQ(registry.compile(state, 22, three_bound).error().code,
             foundation::ErrorCode::resource_exhausted,
             "three-stage PDC budget overflow was not rejected");
  auto bus = state;
  bus.tracks[0].kind = project::TrackKind::group_bus;
  REQUIRE_EQ(node_latency(take(registry.compile(bus, 23, three_bound)), wet), 23,
             "a three-stage VST3 chain on a group bus was rejected or lost latency");
  state.tracks[0].effects.pop_back();
  state.tracks[0].effects[1].enabled = false;
  REQUIRE_EQ(registry.compile(state, 18, bound).error().code,
             foundation::ErrorCode::invalid_argument,
             "bypassed second serial effect accepted an active runtime receipt");
  state.tracks[0].effects[1].enabled = true;
  bound[1].slot = first_slot;
  REQUIRE_EQ(registry.compile(state, 18, bound).error().code, foundation::ErrorCode::unsupported,
             "duplicate bound slot IDs were accepted");
  bound[1].slot = id<project::InsertSlotId>(9'999);
  REQUIRE_EQ(registry.compile(state, 18, bound).error().code, foundation::ErrorCode::unsupported,
             "receipt for a different saved slot was accepted");
  bound[1].slot = second_slot;
  bound[0].latency_frames = std::numeric_limits<std::uint32_t>::max();
  REQUIRE_EQ(registry.compile(state, std::numeric_limits<std::uint32_t>::max(), bound).error().code,
             foundation::ErrorCode::resource_exhausted, "serial latency overflow was truncated");
}

void separate_native_limiters_align_parallel_tracks() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  const auto left = state.tracks[0].id;
  const auto right = state.tracks[1].id;
  const auto left_slot = id<project::InsertSlotId>(1'701);
  const auto right_slot = id<project::InsertSlotId>(1'702);
  state.tracks[0].effects.push_back({left_slot, true, effect_patch("ceiling", 1)});
  state.tracks[1].effects.push_back({right_slot, true, effect_patch("ceiling", 1)});
  const std::array native{native_stage(left, left_slot, 1, 7),
                          native_stage(right, right_slot, 1, 11)};
  const auto plan = take(registry.compile(state, 11, native));
  const auto left_node =
      std::ranges::find(plan.sources, left, &routing::ProjectRouteSource::track)->node;
  const auto right_node =
      std::ranges::find(plan.sources, right, &routing::ProjectRouteSource::track)->node;
  const auto left_edge = std::ranges::find_if(plan.latency.edges, [&](const auto& edge) {
    return edge.source == left_node && edge.destination == plan.master;
  });
  const auto right_edge = std::ranges::find_if(plan.latency.edges, [&](const auto& edge) {
    return edge.source == right_node && edge.destination == plan.master;
  });
  REQUIRE_NE(left_edge, plan.latency.edges.end(),
             "separate native limiter paths were not aligned by their own latencies");
  REQUIRE_NE(right_edge, plan.latency.edges.end(),
             "separate native limiter paths were not aligned by their own latencies");
  REQUIRE_EQ(left_edge->compensation_frames, 4,
             "separate native limiter paths were not aligned by their own latencies");
  REQUIRE_EQ(right_edge->compensation_frames, 0,
             "separate native limiter paths were not aligned by their own latencies");
  REQUIRE_EQ(plan.latency.master_output_latency_frames, 11,
             "separate native limiter paths were not aligned by their own latencies");
  require(!registry.compile(state, 10, native),
          "parallel native limiter path exceeded the compensation budget without rejection");
}

void instrument_and_separate_effect_bind_exact_roles() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  const auto audio = state.tracks[0].id;
  const auto instrument = state.tracks[1].id;
  const auto effect = saved_plugin(id<project::PluginInstanceId>(90));
  const auto source = saved_plugin(id<project::PluginInstanceId>(91));
  state.tracks[0].effects.push_back(saved_effect(effect));
  state.tracks[1].kind = project::TrackKind::instrument;
  state.tracks[1].instrument_source = source;
  const auto effect_slot = state.tracks[0].effects.front().id;
  const std::array receipts{vst3_stage(audio, effect_slot, effect.id, 7),
                            instrument_stage(instrument, source.id, 0)};
  const auto plan = take(registry.compile(state, 7, receipts));
  const auto instrument_node =
      std::ranges::find(plan.sources, instrument, &routing::ProjectRouteSource::track)->node;
  const auto instrument_edge = std::ranges::find_if(plan.latency.edges, [&](const auto& edge) {
    return edge.source == instrument_node && edge.destination == plan.master;
  });
  REQUIRE_NE(instrument_edge, plan.latency.edges.end(),
             "mixed instrument/effect graph did not compensate the parallel MIDI path");
  REQUIRE_EQ(instrument_edge->compensation_frames, 7,
             "mixed instrument/effect graph did not compensate the parallel MIDI path");
  REQUIRE_EQ(plan.latency.master_output_latency_frames, 7,
             "mixed instrument/effect graph did not compensate the parallel MIDI path");
  auto wrong_role = receipts;
  wrong_role[1].slot = effect_slot;
  require(!registry.compile(state, 7, wrong_role),
          "effect receipt was accepted for an instrument source");
  auto second_instrument = state;
  second_instrument.tracks[0].effects.clear();
  second_instrument.tracks[0].kind = project::TrackKind::instrument;
  second_instrument.tracks[0].instrument_source = effect;
  const std::array two_sources{instrument_stage(audio, effect.id, 7), receipts[1]};
  REQUIRE_OK(registry.compile(second_instrument, 7, two_sources),
             "two exact bare VST3 instrument sources should compile independently");
  auto unbound = receipts;
  unbound[0].slot = id<project::InsertSlotId>(999);
  require(!registry.compile(state, 7, unbound),
          "effect receipt with the wrong saved slot was accepted");
}

void two_bare_vst3_sources_bind_distinct_tracks_and_fail_closed() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  const auto first_track = state.tracks[0].id;
  const auto second_track = state.tracks[1].id;
  const auto first = saved_plugin(id<project::PluginInstanceId>(410), "instrument A");
  const auto second = saved_plugin(id<project::PluginInstanceId>(411), "instrument B");
  state.tracks[0].kind = project::TrackKind::instrument;
  state.tracks[0].instrument_source = first;
  state.tracks[1].kind = project::TrackKind::instrument;
  state.tracks[1].instrument_source = second;
  const std::array receipts{instrument_stage(first_track, first.id, 7),
                            instrument_stage(second_track, second.id, 11)};
  const auto plan = take(registry.compile(state, 11, receipts));
  const auto first_node = track_node(plan, first_track);
  const auto second_node = track_node(plan, second_track);
  REQUIRE_NE(first_node, second_node,
             "two instrument sources lost distinct nodes or were serialized to latency 18");
  REQUIRE_EQ(
      std::ranges::find(plan.sources, first_track, &routing::ProjectRouteSource::track)->node,
      first_node, "two instrument sources lost distinct nodes or were serialized to latency 18");
  REQUIRE_EQ(
      std::ranges::find(plan.sources, second_track, &routing::ProjectRouteSource::track)->node,
      second_node, "two instrument sources lost distinct nodes or were serialized to latency 18");
  REQUIRE_EQ(node_latency(plan, first_track), 7,
             "two instrument sources lost distinct nodes or were serialized to latency 18");
  REQUIRE_EQ(node_latency(plan, second_track), 11,
             "two instrument sources lost distinct nodes or were serialized to latency 18");
  REQUIRE_EQ(plan.latency.master_output_latency_frames, 11,
             "two instrument sources lost distinct nodes or were serialized to latency 18");
  const auto first_edge = std::ranges::find_if(plan.latency.edges, [&](const auto& edge) {
    return edge.source == first_node && edge.destination == plan.master;
  });
  const auto second_edge = std::ranges::find_if(plan.latency.edges, [&](const auto& edge) {
    return edge.source == second_node && edge.destination == plan.master;
  });
  REQUIRE_NE(first_edge, plan.latency.edges.end(),
             "independent instrument arrivals did not align at frame 11");
  REQUIRE_NE(second_edge, plan.latency.edges.end(),
             "independent instrument arrivals did not align at frame 11");
  REQUIRE_EQ(first_edge->compensation_frames, 4,
             "independent instrument arrivals did not align at frame 11");
  REQUIRE_EQ(second_edge->compensation_frames, 0,
             "independent instrument arrivals did not align at frame 11");
  REQUIRE_EQ(registry.compile(state, 10, receipts).error().code,
             foundation::ErrorCode::resource_exhausted,
             "parallel instrument latency exceeded the total PDC budget without rejection");
  auto reversed = state;
  std::ranges::reverse(reversed.tracks);
  auto reversed_receipts = receipts;
  std::ranges::reverse(reversed_receipts);
  const auto rebuilt = take(registry.compile(reversed, 11, reversed_receipts));
  REQUIRE_EQ(track_node(rebuilt, first_track), first_node,
             "manifest order changed stable instrument source nodes");
  REQUIRE_EQ(track_node(rebuilt, second_track), second_node,
             "manifest order changed stable instrument source nodes");

  REQUIRE_EQ(registry.compile(state, 11, std::span(receipts).first(1)).error().code,
             foundation::ErrorCode::unsupported,
             "missing instrument source receipt was silently skipped");
  auto wrong_plugin = receipts;
  wrong_plugin[0].plugin = id<project::PluginInstanceId>(412);
  REQUIRE_EQ(registry.compile(state, 11, wrong_plugin).error().code,
             foundation::ErrorCode::unsupported, "wrong saved instrument identity was accepted");
  auto wrong_track = receipts;
  wrong_track[0].track = state.tracks[2].id;
  REQUIRE_EQ(registry.compile(state, 11, wrong_track).error().code,
             foundation::ErrorCode::unsupported,
             "source receipt bound to a bus instead of its owning track was accepted");
  auto duplicate_plugin = receipts;
  duplicate_plugin[1].plugin = duplicate_plugin[0].plugin;
  REQUIRE_EQ(registry.compile(state, 11, duplicate_plugin).error().code,
             foundation::ErrorCode::unsupported,
             "two source receipts reused one plugin instance identity");
  auto duplicate_source_track = receipts;
  duplicate_source_track[1].track = duplicate_source_track[0].track;
  REQUIRE_EQ(registry.compile(state, 11, duplicate_source_track).error().code,
             foundation::ErrorCode::unsupported,
             "two source receipts claimed one instrument track");
  auto source_with_slot = receipts;
  source_with_slot[0].slot = id<project::InsertSlotId>(999);
  REQUIRE_EQ(registry.compile(state, 11, source_with_slot).error().code,
             foundation::ErrorCode::unsupported,
             "instrument source receipt was accepted with an effect slot");

  auto with_effect = state;
  const auto effect = saved_plugin(id<project::PluginInstanceId>(413));
  with_effect.tracks[0].effects.push_back(saved_effect(effect));
  const std::array three_receipts{
      receipts[0], vst3_stage(first_track, with_effect.tracks[0].effects.front().id, effect.id, 0),
      receipts[1]};
  REQUIRE_OK(registry.compile(with_effect, 11, three_receipts),
             "a second instrument track rejected an instrument with its own effect");
  auto paired_effects = with_effect;
  const auto second_effect = saved_plugin(id<project::PluginInstanceId>(415));
  paired_effects.tracks[1].effects.push_back(saved_effect(second_effect));
  const std::array paired_receipts{
      receipts[0],
      vst3_stage(first_track, paired_effects.tracks[0].effects.front().id, effect.id, 7),
      receipts[1],
      vst3_stage(second_track, paired_effects.tracks[1].effects.front().id, second_effect.id, 5)};
  const auto paired = take(registry.compile(paired_effects, 32, paired_receipts));
  REQUIRE_EQ(paired.latency.master_output_latency_frames, 16,
             "two independent source/effect paths lost serial latency or were joined together");
  REQUIRE_EQ(node_latency(paired, first_track), 14,
             "two independent source/effect paths lost serial latency or were joined together");
  REQUIRE_EQ(node_latency(paired, second_track), 16,
             "two independent source/effect paths lost serial latency or were joined together");
  const auto paired_first_edge = std::ranges::find_if(paired.latency.edges, [&](const auto& edge) {
    return edge.source == first_node && edge.destination == paired.master;
  });
  REQUIRE_NE(paired_first_edge, paired.latency.edges.end(),
             "shorter source/effect path did not receive independent PDC");
  REQUIRE_EQ(paired_first_edge->compensation_frames, 2,
             "shorter source/effect path did not receive independent PDC");
  REQUIRE_EQ(registry.compile(paired_effects, 15, paired_receipts).error().code,
             foundation::ErrorCode::resource_exhausted,
             "paired source/effect PDC exceeded its budget without refusal");
  auto wrong_effect_slot = paired_receipts;
  wrong_effect_slot[3].slot = paired_effects.tracks[0].effects.front().id;
  REQUIRE_EQ(registry.compile(paired_effects, 32, wrong_effect_slot).error().code,
             foundation::ErrorCode::unsupported, "paired source effects reused one slot identity");
  auto missing_effect = paired_receipts;
  missing_effect[3].plugin = id<project::PluginInstanceId>(416);
  REQUIRE_EQ(registry.compile(paired_effects, 32, missing_effect).error().code,
             foundation::ErrorCode::unsupported,
             "paired source path accepted a mismatched effect identity");
  auto third_source = state;
  const auto third = saved_plugin(id<project::PluginInstanceId>(414), "instrument C");
  third_source.tracks[2].kind = project::TrackKind::instrument;
  third_source.tracks[2].instrument_source = third;
  const std::array third_receipts{receipts[0], receipts[1],
                                  instrument_stage(third_source.tracks[2].id, third.id, 0)};
  const auto three_sources = take(registry.compile(third_source, 11, third_receipts));
  REQUIRE_EQ(three_sources.sources.size(), 3,
             "a third VST3 instrument track was rejected or lost its source");
  REQUIRE_EQ(node_latency(three_sources, third_source.tracks[2].id), 0,
             "a third VST3 instrument track was rejected or lost its source");
}

void instrument_and_own_effect_are_one_serial_path() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  auto& instrument = state.tracks[1];
  instrument.kind = project::TrackKind::instrument;
  const auto source = saved_plugin(id<project::PluginInstanceId>(191), "instrument");
  const auto effect = saved_plugin(id<project::PluginInstanceId>(192));
  instrument.instrument_source = source;
  instrument.effects.push_back(saved_effect(effect));
  const std::array receipts{
      instrument_stage(instrument.id, source.id, 11),
      vst3_stage(instrument.id, instrument.effects.front().id, effect.id, 29)};
  const auto plan = take(registry.compile(state, 64, receipts));
  REQUIRE_EQ(node_latency(plan, instrument.id), 40,
             "instrument/effect serial route did not sum processor latency");
  REQUIRE_EQ(plan.latency.master_output_latency_frames, 40,
             "instrument/effect serial route did not sum processor latency");
  auto wrong_slot = receipts;
  wrong_slot[1].slot = id<project::InsertSlotId>(999);
  require(!registry.compile(state, 64, wrong_slot),
          "instrument effect accepted an unrelated slot receipt");
  auto wrong_role = receipts;
  wrong_role[1].slot.reset();
  require(!registry.compile(state, 64, wrong_role), "instrument effect accepted a source receipt");
  auto effect_first = receipts;
  std::ranges::reverse(effect_first);
  require(!registry.compile(state, 64, effect_first),
          "instrument effect receipt was accepted ahead of its instrument");
}

void instrument_and_two_own_effects_are_one_serial_path() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  auto& instrument = state.tracks[1];
  instrument.kind = project::TrackKind::instrument;
  const auto source = saved_plugin(id<project::PluginInstanceId>(301), "instrument");
  const auto first = saved_plugin(id<project::PluginInstanceId>(302));
  const auto second = saved_plugin(id<project::PluginInstanceId>(303));
  instrument.instrument_source = source;
  instrument.effects = {saved_effect(first), saved_effect(second)};
  const std::array receipts{instrument_stage(instrument.id, source.id, 7),
                            vst3_stage(instrument.id, instrument.effects[0].id, first.id, 11),
                            vst3_stage(instrument.id, instrument.effects[1].id, second.id, 13)};
  const auto plan = take(registry.compile(state, 64, receipts));
  REQUIRE_EQ(node_latency(plan, instrument.id), 31,
             "instrument and two serial effects did not sum latency");
  REQUIRE_EQ(plan.latency.master_output_latency_frames, 31,
             "instrument and two serial effects did not sum latency");
  auto wrong_slot = receipts;
  wrong_slot[2].slot = id<project::InsertSlotId>(999);
  require(!registry.compile(state, 64, wrong_slot),
          "second instrument effect accepted an unrelated slot receipt");
  auto unrelated = state;
  unrelated.tracks[1].effects.pop_back();
  unrelated.tracks[0].effects.push_back(saved_effect(second));
  require(!registry.compile(unrelated, 64, receipts),
          "second instrument effect escaped to a separate path");
  auto third = state;
  const auto third_plugin = id<project::PluginInstanceId>(304);
  third.tracks[1].effects.push_back(saved_effect(saved_plugin(third_plugin)));
  require(!registry.compile(third, 64, receipts),
          "instrument accepted a third VST3 effect without a bound runtime");
  const std::array four_receipts{
      receipts[0], receipts[1], receipts[2],
      vst3_stage(instrument.id, third.tracks[1].effects.back().id, third_plugin, 3)};
  const auto four = take(registry.compile(third, 64, four_receipts));
  REQUIRE_EQ(node_latency(four, instrument.id), 34,
             "a VST3 instrument with three VST3 effects was rejected or lost latency");
  auto bypassed = state;
  bypassed.tracks[1].effects[1].enabled = false;
  require(!registry.compile(bypassed, 64, receipts),
          "instrument accepted a bypassed stage in the bounded chain");
}

// Five or more VST3 hosts in one project, four VST3 effects on one track, VST3
// effects on group and return buses, and a VST3 instrument with its own
// effects all compile together: the route has no host-count shape limits.
void many_vst3_instances_compile_in_one_project() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  for (std::uint64_t number = 630; number < 634; ++number)
    state.tracks.emplace_back(id<project::TrackId>(number));
  std::vector<routing::BoundStage> receipts;
  const auto add_effect = [&](std::size_t track_index, std::uint64_t plugin_number,
                              std::uint32_t latency) {
    auto& track = state.tracks[track_index];
    const auto plugin = id<project::PluginInstanceId>(plugin_number);
    track.effects.push_back(saved_effect(saved_plugin(plugin)));
    receipts.push_back(vst3_stage(track.id, track.effects.back().id, plugin, latency));
  };
  // Audio track: four VST3 effects into the group bus.
  state.tracks[0].main_output = state.tracks[2].id;
  for (std::uint32_t index = 0; index < 4; ++index)
    add_effect(0, 600 + index, index + 1);
  // VST3 instrument with two VST3 effects, also sent to the return bus.
  const auto source = id<project::PluginInstanceId>(610);
  state.tracks[1].kind = project::TrackKind::instrument;
  state.tracks[1].instrument_source = saved_plugin(source, "instrument");
  state.tracks[1].sends.push_back({id<project::SendId>(620), state.tracks[3].id, 0.5});
  receipts.push_back(instrument_stage(state.tracks[1].id, source, 5));
  add_effect(1, 611, 6);
  add_effect(1, 612, 7);
  add_effect(2, 613, 2);
  add_effect(3, 614, 3);
  for (std::size_t index = 4; index < state.tracks.size(); ++index)
    add_effect(index, 640 + index, 0);
  REQUIRE_EQ(receipts.size(), 13, "fixture lost a VST3 stage");
  const auto plan = take(registry.compile(state, 21, receipts));
  REQUIRE_EQ(node_latency(plan, state.tracks[0].id), 10,
             "many VST3 stages lost their per-track latency sums");
  REQUIRE_EQ(node_latency(plan, state.tracks[1].id), 18,
             "many VST3 stages lost their per-track latency sums");
  REQUIRE_EQ(node_latency(plan, state.tracks[2].id), 2,
             "many VST3 stages lost their per-track latency sums");
  REQUIRE_EQ(node_latency(plan, state.tracks[3].id), 3,
             "many VST3 stages lost their per-track latency sums");
  REQUIRE_EQ(node_latency(plan, state.tracks[4].id), 0,
             "many VST3 stages lost their per-track latency sums");
  REQUIRE_EQ(master_node_latency(plan), 0, "many VST3 stages lost their per-track latency sums");
  // Longest path: instrument (18) through the return bus (3).
  require(plan.latency.master_output_latency_frames == 21 && plan.sources.size() == 6,
          "many VST3 stages lost the longest path or a source");
  require_failure(registry.compile(state, 20, receipts), foundation::ErrorCode::resource_exhausted,
                  "many-VST3 route exceeded its compensation budget without refusal");
}

// Built-in and VST3 effects interleave freely: every saved order compiles
// when the receipts follow it, and the reversed receipts never do.
void native_and_vst3_effects_interleave_in_any_saved_order() {
  for (const std::size_t owner_index : {0U, 2U, 3U}) {
    const auto owner = snapshot().tracks[owner_index].id;
    const auto first_vst3 = saved_effect(saved_plugin(id<project::PluginInstanceId>(700)));
    const auto second_vst3 = saved_effect(saved_plugin(id<project::PluginInstanceId>(702)));
    const std::array<project::EffectInsert, 4> effects{
        first_vst3,
        project::EffectInsert{id<project::InsertSlotId>(701), true, effect_patch("shape", 3)},
        second_vst3,
        project::EffectInsert{id<project::InsertSlotId>(703), true, effect_patch("ceiling", 5)}};
    const std::array<routing::BoundStage, 4> stages{
        vst3_stage(owner, first_vst3.id, id<project::PluginInstanceId>(700), 2),
        native_stage(owner, id<project::InsertSlotId>(701), 3),
        vst3_stage(owner, second_vst3.id, id<project::PluginInstanceId>(702), 3),
        native_stage(owner, id<project::InsertSlotId>(703), 5, 11)};
    std::array<std::size_t, 4> order{0, 1, 2, 3};
    std::size_t orders = 0;
    do {
      routing::ProjectRouteIdRegistry registry;
      auto state = snapshot();
      if (owner_index == 2)
        state.tracks[0].main_output = owner;
      if (owner_index == 3)
        state.tracks[0].sends.push_back({id<project::SendId>(704), owner, 1.0});
      std::vector<routing::BoundStage> receipts;
      for (const auto index : order) {
        state.tracks[owner_index].effects.push_back(effects[index]);
        receipts.push_back(stages[index]);
      }
      const auto plan = take(registry.compile(state, 16, receipts));
      REQUIRE_EQ(node_latency(plan, owner), 16,
                 "an interleaved native/VST3 chain lost a stage's latency");
      REQUIRE_EQ(plan.latency.master_output_latency_frames, 16,
                 "an interleaved native/VST3 chain lost a stage's latency");
      auto reversed = receipts;
      std::ranges::reverse(reversed);
      require_failure(registry.compile(state, 16, reversed), foundation::ErrorCode::unsupported,
                      "interleaved chain accepted receipts in reverse order");
      ++orders;
    } while (std::next_permutation(order.begin(), order.end()));
    REQUIRE_EQ(orders, 24, "not every interleaving was compiled");
  }
}

// Two or more VST3 instrument tracks, each with its own chain, compile beside
// synth, audio and bus tracks that run only built-in effects.
void vst3_instruments_with_arbitrary_chains_beside_native_tracks() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  for (std::uint64_t number = 6; number <= 8; ++number)
    state.tracks.emplace_back(id<project::TrackId>(number));
  auto& keys = state.tracks[0];
  auto& pad = state.tracks[1];
  auto& group = state.tracks[2];
  auto& returns = state.tracks[3];
  auto& bare = state.tracks[4];
  auto& synth = state.tracks[5];
  auto& audio = state.tracks[6];
  const auto plugin = [](std::uint64_t number) { return id<project::PluginInstanceId>(number); };
  const auto slot = [](std::uint64_t number) { return id<project::InsertSlotId>(number); };

  keys.kind = project::TrackKind::instrument;
  keys.instrument_source = saved_plugin(plugin(800), "keys");
  keys.effects = {{slot(801), true, effect_patch("shape", 2)},
                  saved_effect(saved_plugin(plugin(802))),
                  {slot(803), true, effect_patch("ceiling", 1)}};
  keys.main_output = group.id;
  pad.kind = project::TrackKind::instrument;
  pad.instrument_source = saved_plugin(plugin(810), "pad");
  pad.effects = {saved_effect(saved_plugin(plugin(811))), saved_effect(saved_plugin(plugin(812)))};
  pad.sends.push_back({id<project::SendId>(813), returns.id, 0.5});
  group.effects = {{slot(840), true, effect_patch("shape", 1)}};
  returns.effects = {{slot(850), true, effect_patch("expanse", 1)},
                     saved_effect(saved_plugin(plugin(851)))};
  bare.kind = project::TrackKind::instrument;
  bare.instrument_source = saved_plugin(plugin(820), "bare");
  synth.kind = project::TrackKind::instrument;
  synth.instrument_source = project::NativeInstrumentPatch{"prism", 1, {}};
  synth.effects = {{slot(830), true, effect_patch("ceiling", 4)}};
  audio.effects = {{slot(860), true, effect_patch("press", 1)}};

  const std::vector<routing::BoundStage> receipts{
      instrument_stage(keys.id, plugin(800), 4),
      native_stage(keys.id, slot(801), 2),
      vst3_stage(keys.id, keys.effects[1].id, plugin(802), 6),
      native_stage(keys.id, slot(803), 1, 8),
      instrument_stage(pad.id, plugin(810), 3),
      vst3_stage(pad.id, pad.effects[0].id, plugin(811), 5),
      vst3_stage(pad.id, pad.effects[1].id, plugin(812), 1),
      native_stage(group.id, slot(840), 1),
      native_stage(returns.id, slot(850), 1),
      vst3_stage(returns.id, returns.effects[1].id, plugin(851), 4),
      instrument_stage(bare.id, plugin(820), 2),
      native_stage(synth.id, slot(830), 4, 7),
      native_stage(audio.id, slot(860), 1)};
  const auto plan = take(registry.compile(state, 18, receipts));
  REQUIRE_EQ(node_latency(plan, keys.id), 18,
             "mixed instrument and native chains lost a per-track latency sum");
  REQUIRE_EQ(node_latency(plan, pad.id), 9,
             "mixed instrument and native chains lost a per-track latency sum");
  REQUIRE_EQ(node_latency(plan, group.id), 0,
             "mixed instrument and native chains lost a per-track latency sum");
  REQUIRE_EQ(node_latency(plan, returns.id), 4,
             "mixed instrument and native chains lost a per-track latency sum");
  REQUIRE_EQ(node_latency(plan, bare.id), 2,
             "mixed instrument and native chains lost a per-track latency sum");
  REQUIRE_EQ(node_latency(plan, synth.id), 7,
             "mixed instrument and native chains lost a per-track latency sum");
  REQUIRE_EQ(node_latency(plan, audio.id), 0,
             "mixed instrument and native chains lost a per-track latency sum");
  REQUIRE_EQ(plan.latency.master_output_latency_frames, 18,
             "mixed instrument and native chains lost the longest path or a source");
  REQUIRE_EQ(plan.sources.size(), 5,
             "mixed instrument and native chains lost the longest path or a source");
  require_failure(registry.compile(state, 17, receipts), foundation::ErrorCode::resource_exhausted,
                  "mixed instrument route exceeded its compensation budget without refusal");
  auto bare_missing = receipts;
  bare_missing.erase(bare_missing.begin() + 10);
  require_failure(registry.compile(state, 18, bare_missing), foundation::ErrorCode::unsupported,
                  "a bare VST3 instrument was silently skipped");
}

void bus_vst3_effect_preserves_parallel_alignment() {
  for (const auto bus_index : {2U, 3U}) {
    routing::ProjectRouteIdRegistry registry;
    auto state = snapshot();
    const auto bus = state.tracks[bus_index].id;
    if (bus_index == 2)
      state.tracks[0].main_output = bus;
    else
      state.tracks[0].sends.push_back({id<project::SendId>(210), bus, 0.5});
    const auto effect = saved_plugin(id<project::PluginInstanceId>(200 + bus_index));
    state.tracks[bus_index].effects.push_back(saved_effect(effect));
    const std::array receipts{
        vst3_stage(bus, state.tracks[bus_index].effects.front().id, effect.id, 7)};
    const auto plan = take(registry.compile(state, 64, receipts));
    REQUIRE_EQ(node_latency(plan, bus), 7,
               "bus VST3 effect latency was not included in the routed path");
    REQUIRE_EQ(plan.latency.master_output_latency_frames, 7,
               "bus VST3 effect latency was not included in the routed path");
    auto wrong = receipts;
    wrong[0].track = state.tracks[0].id;
    require(!registry.compile(state, 64, wrong), "bus VST3 effect accepted an audio-track receipt");
  }
}

void instrument_and_separate_native_chain_keep_latency_and_roles() {
  for (const auto owner_index : {0U, 2U, 3U}) {
    routing::ProjectRouteIdRegistry registry;
    auto state = snapshot();
    const auto instrument = state.tracks[1].id;
    const auto owner = state.tracks[owner_index].id;
    const auto source = saved_plugin(id<project::PluginInstanceId>(120));
    state.tracks[1].kind = project::TrackKind::instrument;
    state.tracks[1].instrument_source = source;
    if (owner_index == 2)
      state.tracks[0].main_output = owner;
    if (owner_index == 3)
      state.tracks[0].sends.push_back({id<project::SendId>(121), owner, 1.0});
    const auto eq = id<project::InsertSlotId>(122);
    const auto limiter = id<project::InsertSlotId>(123);
    state.tracks[owner_index].effects = {{eq, true, effect_patch("shape", 3)},
                                         {limiter, true, effect_patch("ceiling", 4)}};
    // Receipts follow saved track order: the owner's chain sits before or
    // after the instrument track's source.
    const auto ordered = [&](const routing::BoundStage& source_stage,
                             std::span<const routing::BoundStage> chain) {
      std::vector<routing::BoundStage> result;
      if (owner_index != 0)
        result.push_back(source_stage);
      result.insert(result.end(), chain.begin(), chain.end());
      if (owner_index == 0)
        result.push_back(source_stage);
      return result;
    };
    const auto source_receipt = instrument_stage(instrument, source.id, 0);
    const std::array native{native_stage(owner, eq, 3), native_stage(owner, limiter, 4, 48)};
    const auto plan = take(registry.compile(state, 48, ordered(source_receipt, native)));
    const auto instrument_node =
        std::ranges::find(plan.sources, instrument, &routing::ProjectRouteSource::track)->node;
    const auto instrument_edge = std::ranges::find_if(plan.latency.edges, [&](const auto& edge) {
      return edge.source == instrument_node && edge.destination == plan.master;
    });
    REQUIRE_NE(instrument_edge, plan.latency.edges.end(),
               "separate native chain did not align the instrument source");
    REQUIRE_EQ(instrument_edge->compensation_frames, 48,
               "separate native chain did not align the instrument source");
    REQUIRE_EQ(plan.latency.master_output_latency_frames, 48,
               "separate native chain did not align the instrument source");
    auto stale = native;
    stale[1].generation = 5;
    require(!registry.compile(state, 48, ordered(source_receipt, stale)),
            "instrument/native route accepted a stale native receipt");
    auto wrong_role = source_receipt;
    wrong_role.slot = eq;
    require(!registry.compile(state, 48, ordered(wrong_role, native)),
            "instrument/native route accepted a source bound to an effect slot");
  }
}

void vst3_instrument_native_chain_is_one_serial_path() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  auto& instrument = state.tracks[1];
  instrument.kind = project::TrackKind::instrument;
  const auto source = saved_plugin(id<project::PluginInstanceId>(140), "instrument");
  instrument.instrument_source = source;
  const auto eq = id<project::InsertSlotId>(141);
  const auto limiter = id<project::InsertSlotId>(142);
  instrument.effects = {{eq, true, effect_patch("shape", 3)},
                        {limiter, true, effect_patch("ceiling", 4)}};
  const std::array receipts{instrument_stage(instrument.id, source.id, 16),
                            native_stage(instrument.id, eq, 3),
                            native_stage(instrument.id, limiter, 4, 32)};
  const auto plan = take(registry.compile(state, 48, receipts));
  const auto node = track_node(plan, instrument.id);
  REQUIRE_EQ(node_latency(plan, instrument.id), 48,
             "VST3 instrument and native inserts lost their serial latency or source identity");
  REQUIRE_EQ(
      std::ranges::find(plan.sources, instrument.id, &routing::ProjectRouteSource::track)->node,
      node, "VST3 instrument and native inserts lost their serial latency or source identity");
  REQUIRE_EQ(plan.latency.master_output_latency_frames, 48,
             "VST3 instrument and native inserts lost their serial latency or source identity");
  auto stale = receipts;
  stale[2].generation = 5;
  require(!registry.compile(state, 48, stale),
          "VST3 instrument path accepted a stale native processor");
  auto wrong_path = receipts;
  wrong_path[2].track = state.tracks[0].id;
  require(!registry.compile(state, 48, wrong_path),
          "VST3 instrument path accepted a processor belonging to another track");
}

void synth_native_chain_has_one_serial_latency_path() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  auto& instrument = state.tracks[1];
  instrument.kind = project::TrackKind::instrument;
  instrument.instrument_source = project::NativeInstrumentPatch{"prism", 1, {}};
  const auto eq = id<project::InsertSlotId>(131);
  const auto limiter = id<project::InsertSlotId>(132);
  instrument.effects = {{eq, true, effect_patch("shape", 3)},
                        {limiter, true, effect_patch("ceiling", 4)}};
  const std::array native{native_stage(instrument.id, eq, 3),
                          native_stage(instrument.id, limiter, 4, 48)};
  const auto plan = take(registry.compile(state, 48, native));
  REQUIRE_EQ(plan.latency.master_output_latency_frames, 48,
             "synth native chain lost its instrument source or serial latency");
  REQUIRE_NE(std::ranges::find(plan.sources, instrument.id, &routing::ProjectRouteSource::track),
             plan.sources.end(), "synth native chain lost its instrument source or serial latency");
  auto stale = native;
  stale[1].generation = 5;
  require(!registry.compile(state, 48, stale),
          "synth native chain accepted stale processor settings");
  instrument.effects.push_back({id<project::InsertSlotId>(133), true, effect_patch("shape", 5)});
  std::vector<routing::BoundStage> expanded(native.begin(), native.end());
  expanded.push_back(native_stage(instrument.id, id<project::InsertSlotId>(133), 5));
  REQUIRE_OK(registry.compile(state, 48, expanded),
             "synth native chain rejected a third ordered stage");
  for (std::uint64_t index = 4; index <= routing::ProjectRouteIdRegistry::max_stages_per_path;
       ++index) {
    const auto slot = id<project::InsertSlotId>(2'000 + index);
    instrument.effects.push_back({slot, true, effect_patch("shape", index + 2)});
    expanded.push_back(native_stage(instrument.id, slot, index + 2));
  }
  REQUIRE_EQ(expanded.size(), routing::ProjectRouteIdRegistry::max_stages_per_path,
             "synth native chain rejected its bounded maximum");
  REQUIRE_OK(registry.compile(state, 48, expanded),
             "synth native chain rejected its bounded maximum");
  const auto overflow = id<project::InsertSlotId>(3'000);
  instrument.effects.push_back({overflow, true, effect_patch("shape", 11)});
  require(!registry.compile(state, 48, expanded),
          "synth native chain silently skipped a stage past its bound");
  expanded.push_back(native_stage(instrument.id, overflow, 11));
  require_failure(registry.compile(state, 48, expanded), foundation::ErrorCode::unsupported,
                  "-stage chain", "synth native chain exceeded its bounded stage count");
}

// The 64-stage bound counts a VST3 instrument as a stage of its track.
void stage_bound_counts_the_vst3_instrument() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  auto& instrument = state.tracks[1];
  instrument.kind = project::TrackKind::instrument;
  const auto source = id<project::PluginInstanceId>(150);
  instrument.instrument_source = saved_plugin(source, "instrument");
  std::vector<routing::BoundStage> receipts{instrument_stage(instrument.id, source, 1)};
  for (std::uint64_t index = 1; index < routing::ProjectRouteIdRegistry::max_stages_per_path;
       ++index) {
    const auto slot = id<project::InsertSlotId>(4'000 + index);
    instrument.effects.push_back({slot, true, effect_patch("press", index)});
    receipts.push_back(native_stage(instrument.id, slot, index));
  }
  const auto full = take(registry.compile(state, 1, receipts));
  REQUIRE_EQ(receipts.size(), routing::ProjectRouteIdRegistry::max_stages_per_path,
             "a VST3 instrument chain at the stage bound was rejected");
  REQUIRE_EQ(node_latency(full, instrument.id), 1,
             "a VST3 instrument chain at the stage bound was rejected");
  const auto overflow = id<project::InsertSlotId>(4'999);
  instrument.effects.push_back({overflow, true, effect_patch("press", 1)});
  receipts.push_back(native_stage(instrument.id, overflow, 1));
  REQUIRE_EQ(instrument.effects.size(), project::max_effect_slots_per_path,
             "fixture exceeded the saved effect-slot bound");
  require_failure(registry.compile(state, 1, receipts), foundation::ErrorCode::unsupported,
                  "-stage chain", "a VST3 instrument and 64 effects escaped the stage bound");
}

// Every receipt mismatch is refused: order, absence, surplus, identity,
// processor kind and built-in generation.
void receipt_mismatches_fail_closed() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  const auto first = state.tracks[0].id;
  const auto second = state.tracks[1].id;
  const auto native_slot = id<project::InsertSlotId>(960);
  const auto plugin = id<project::PluginInstanceId>(961);
  const auto second_slot = id<project::InsertSlotId>(962);
  state.tracks[0].effects = {{native_slot, true, effect_patch("shape", 3)},
                             saved_effect(saved_plugin(plugin))};
  state.tracks[1].effects = {{second_slot, true, effect_patch("press", 2)}};
  const auto vst3_slot = state.tracks[0].effects[1].id;
  const std::array good{native_stage(first, native_slot, 3, 1),
                        vst3_stage(first, vst3_slot, plugin, 4),
                        native_stage(second, second_slot, 2)};
  const auto plan = take(registry.compile(state, 5, good));
  REQUIRE_EQ(node_latency(plan, first), 5, "exact receipts did not compile");
  REQUIRE_EQ(node_latency(plan, second), 0, "exact receipts did not compile");
  constexpr std::string_view mismatch = "no matching ordered prepared stage";
  constexpr std::string_view surplus = "absent from saved routing";
  const auto rejects = [&](std::span<const routing::BoundStage> stages, foundation::ErrorCode code,
                           std::string_view fragment, std::string_view message) {
    require_failure(registry.compile(state, 5, stages), code, fragment, message);
  };

  const std::array swapped{good[1], good[0], good[2]};
  rejects(swapped, foundation::ErrorCode::unsupported, mismatch,
          "receipts out of order within a track were accepted");
  const std::array across{good[2], good[0], good[1]};
  rejects(across, foundation::ErrorCode::unsupported, mismatch,
          "receipts out of saved track order were accepted");
  rejects(std::span(good).first(2), foundation::ErrorCode::unsupported, mismatch,
          "a missing last receipt was silently skipped");
  const std::array missing_middle{good[0], good[2]};
  rejects(missing_middle, foundation::ErrorCode::unsupported, mismatch,
          "a missing middle receipt was silently skipped");
  rejects({}, foundation::ErrorCode::unsupported, mismatch, "a chain compiled with no receipts");

  const std::array extra_slot{good[0], good[1], good[2],
                              native_stage(second, id<project::InsertSlotId>(963), 1)};
  rejects(extra_slot, foundation::ErrorCode::invalid_argument, surplus,
          "a receipt for an unsaved slot was accepted");
  const std::array extra_track{good[0], good[1], good[2],
                               native_stage(state.tracks[2].id, id<project::InsertSlotId>(970), 1)};
  rejects(extra_track, foundation::ErrorCode::invalid_argument, surplus,
          "a receipt for a track with no chain was accepted");
  const std::array duplicate{good[0], good[1], good[2], good[2]};
  rejects(duplicate, foundation::ErrorCode::invalid_argument, surplus,
          "a duplicated receipt was accepted");

  auto wrong = good;
  wrong[1].plugin = id<project::PluginInstanceId>(999);
  rejects(wrong, foundation::ErrorCode::unsupported, mismatch,
          "a receipt for another plugin instance was accepted");
  wrong = good;
  wrong[1].plugin.reset();
  rejects(wrong, foundation::ErrorCode::unsupported, mismatch,
          "a VST3 receipt without its plugin instance was accepted");
  wrong = good;
  wrong[1].processor = routing::StageProcessor::native_effect;
  rejects(wrong, foundation::ErrorCode::unsupported, mismatch,
          "a built-in receipt was accepted for a VST3 slot");
  wrong = good;
  wrong[0].processor = routing::StageProcessor::vst3;
  rejects(wrong, foundation::ErrorCode::unsupported, mismatch,
          "a VST3 receipt was accepted for a built-in slot");
  wrong = good;
  wrong[0].plugin = plugin;
  rejects(wrong, foundation::ErrorCode::unsupported, mismatch,
          "a built-in receipt naming a plugin instance was accepted");
  wrong = good;
  wrong[0].generation = 4;
  rejects(wrong, foundation::ErrorCode::unsupported, mismatch,
          "a built-in receipt for a stale generation was accepted");
  wrong = good;
  wrong[2].generation = 1;
  rejects(wrong, foundation::ErrorCode::unsupported, mismatch,
          "a built-in receipt for an older generation was accepted");
  wrong = good;
  wrong[2].track = first;
  rejects(wrong, foundation::ErrorCode::unsupported, mismatch,
          "a receipt naming another track was accepted");
  wrong = good;
  wrong[0].slot = second_slot;
  rejects(wrong, foundation::ErrorCode::unsupported, mismatch,
          "a receipt naming another track's slot was accepted");

  // A VST3 stage has no saved generation; its receipt's generation is unused.
  wrong = good;
  wrong[1].generation = 77;
  REQUIRE_OK(registry.compile(state, 5, wrong),
             "a VST3 receipt was refused over an unused generation");
  // Failed compiles consumed no IDs and left the session plan reproducible.
  const auto again = take(registry.compile(state, 5, good));
  require(std::ranges::equal(again.track_nodes, plan.track_nodes,
                             [](const auto& left, const auto& right) {
                               return left.track == right.track && left.node == right.node;
                             }),
          "refused receipts changed the session's route identities");
}

void solo_projection_retains_routes_and_respects_explicit_mute() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  const auto audio = state.tracks[0].id;
  const auto other = state.tracks[1].id;
  const auto group = state.tracks[2].id;
  const auto returns = state.tracks[3].id;
  state.tracks[0].main_output = group;
  state.tracks[0].sends.push_back({id<project::SendId>(80), returns, 0.5});
  const auto plan = take(registry.compile(state, 0));
  const auto muted = [&](project::TrackId track) {
    const auto mask = routing::effective_track_mutes(state, plan);
    const auto found =
        std::ranges::find(plan.track_nodes, track, &routing::ProjectRouteTrackNode::track);
    REQUIRE_NE(found, plan.track_nodes.end(), "solo fixture track missing");
    return mask[static_cast<std::size_t>(found - plan.track_nodes.begin())];
  };
  REQUIRE_TRUE(!muted(audio), "ordinary graph unexpectedly silenced a track");
  REQUIRE_TRUE(!muted(other), "ordinary graph unexpectedly silenced a track");
  REQUIRE_TRUE(!muted(group), "ordinary graph unexpectedly silenced a track");
  REQUIRE_TRUE(!muted(returns), "ordinary graph unexpectedly silenced a track");
  state.tracks[0].soloed = true;
  REQUIRE_TRUE(!muted(audio), "source solo lost its group/return route or leaked another source");
  REQUIRE_TRUE(muted(other), "source solo lost its group/return route or leaked another source");
  REQUIRE_TRUE(!muted(group), "source solo lost its group/return route or leaked another source");
  REQUIRE_TRUE(!muted(returns), "source solo lost its group/return route or leaked another source");
  state.tracks[0].muted = true;
  REQUIRE_TRUE(muted(audio), "explicit mute did not override a source solo");
  REQUIRE_TRUE(muted(other), "explicit mute did not override a source solo");
  REQUIRE_TRUE(!muted(group), "explicit mute did not override a source solo");
  REQUIRE_TRUE(!muted(returns), "explicit mute did not override a source solo");
  state.tracks[0].muted = false;
  state.tracks[0].soloed = false;
  state.tracks[2].soloed = true;
  REQUIRE_TRUE(!muted(audio), "bus solo did not retain its input or excluded the wrong branch");
  REQUIRE_TRUE(muted(other), "bus solo did not retain its input or excluded the wrong branch");
  REQUIRE_TRUE(!muted(group), "bus solo did not retain its input or excluded the wrong branch");
  REQUIRE_TRUE(muted(returns), "bus solo did not retain its input or excluded the wrong branch");
  state.tracks[1].soloed = true;
  REQUIRE_TRUE(!muted(audio), "multiple solos did not combine their audible paths");
  REQUIRE_TRUE(!muted(other), "multiple solos did not combine their audible paths");
  REQUIRE_TRUE(!muted(group), "multiple solos did not combine their audible paths");
  REQUIRE_TRUE(muted(returns), "multiple solos did not combine their audible paths");
  state.tracks[0].level = 0.5;
  state.tracks[0].pan = 0.25;
  state.master_level = 0.75;
  const auto controls = take(routing::project_node_controls(state, plan));
  REQUIRE_EQ(controls.size(), plan.latency.nodes.size(),
             "live controls did not cover the compiled route");
  for (std::size_t index = 0; index < controls.size(); ++index)
    REQUIRE_EQ(controls[index].id, plan.latency.nodes[index].id,
               "live controls lost topological node order");
  const auto mapped =
      std::ranges::find(plan.track_nodes, audio, &routing::ProjectRouteTrackNode::track);
  const auto active = std::ranges::find(controls, mapped->node, &engine::RoutedNodeControl::id);
  REQUIRE_NE(active, controls.end(),
             "live control projection lost solo retention or level/pan balance");
  REQUIRE_TRUE(!active->muted, "live control projection lost solo retention or level/pan balance");
  REQUIRE_EQ(active->output_gain.left, 0.375F,
             "live control projection lost solo retention or level/pan balance");
  REQUIRE_EQ(active->output_gain.right, 0.5F,
             "live control projection lost solo retention or level/pan balance");
  const auto master = std::ranges::find(controls, plan.master, &engine::RoutedNodeControl::id);
  REQUIRE_NE(master, controls.end(), "live control projection lost the master level");
  REQUIRE_EQ(master->output_gain.left, 0.75F, "live control projection lost the master level");
  REQUIRE_EQ(master->output_gain.right, 0.75F, "live control projection lost the master level");
  const auto excluded =
      std::ranges::find(plan.track_nodes, returns, &routing::ProjectRouteTrackNode::track);
  const auto silent = std::ranges::find(controls, excluded->node, &engine::RoutedNodeControl::id);
  REQUIRE_NE(silent, controls.end(), "live control projection lost solo-muted return route");
  REQUIRE_TRUE(silent->muted, "live control projection lost solo-muted return route");
  state.tracks.emplace_back(id<project::TrackId>(99));
  require(!routing::project_node_controls(state, plan),
          "live control projection accepted a changed track set");
}

void channel_balance_preserves_center_unity() {
  project::Track track{id<project::TrackId>(90)};
  auto gain = routing::track_stereo_gain(track);
  REQUIRE_EQ(gain.left, 1.0F, "center balance changed unity level");
  REQUIRE_EQ(gain.right, 1.0F, "center balance changed unity level");
  track.level = 0.5;
  track.pan = 0.25;
  gain = routing::track_stereo_gain(track);
  REQUIRE_EQ(gain.left, 0.375F, "right balance did not attenuate only the left channel");
  REQUIRE_EQ(gain.right, 0.5F, "right balance did not attenuate only the left channel");
  track.pan = -1.0;
  gain = routing::track_stereo_gain(track);
  REQUIRE_EQ(gain.left, 0.5F, "hard left balance did not silence the right channel");
  REQUIRE_EQ(gain.right, 0.0F, "hard left balance did not silence the right channel");
}

void silent_tracks_are_muted_or_feed_only_muted_tracks_or_silent_keys() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  const auto audio = state.tracks[0].id;
  const auto other = state.tracks[1].id;
  const auto group = state.tracks[2].id;
  const auto returns = state.tracks[3].id;
  state.tracks[0].main_output = group;
  const auto silent = [&](project::TrackId track, std::span<const routing::BoundStage> bound = {}) {
    const auto plan = take(registry.compile(state, 64, bound));
    const auto mask = routing::silent_track_nodes(state, plan);
    // Without a usable order the fallback passes must settle the same mask:
    // no order at all, and sources before destinations (the worst order).
    auto unordered = plan;
    unordered.latency.topological_order.clear();
    REQUIRE_TRUE(routing::silent_track_nodes(state, unordered) == mask,
                 "the fallback passes settled a different silence mask");
    auto backwards = plan;
    std::ranges::reverse(backwards.latency.topological_order);
    REQUIRE_TRUE(routing::silent_track_nodes(state, backwards) == mask,
                 "the fallback passes settled a different silence mask");
    const auto found =
        std::ranges::find(plan.track_nodes, track, &routing::ProjectRouteTrackNode::track);
    REQUIRE_NE(found, plan.track_nodes.end(), "silent fixture track missing");
    return mask[static_cast<std::size_t>(found - plan.track_nodes.begin())];
  };
  REQUIRE_TRUE(!silent(audio), "an ordinary graph had a silent track");
  REQUIRE_TRUE(!silent(other), "an ordinary graph had a silent track");
  REQUIRE_TRUE(!silent(group), "an ordinary graph had a silent track");
  REQUIRE_TRUE(!silent(returns), "an ordinary graph had a silent track");
  // A muted bus silences what only feeds it, not what also reaches a return.
  state.tracks[2].muted = true;
  REQUIRE_TRUE(silent(group), "a track feeding only a muted bus was rendered as heard");
  REQUIRE_TRUE(silent(audio), "a track feeding only a muted bus was rendered as heard");
  REQUIRE_TRUE(!silent(other), "a track feeding only a muted bus was rendered as heard");
  REQUIRE_TRUE(!silent(returns), "a track feeding only a muted bus was rendered as heard");
  state.tracks[0].sends.push_back({id<project::SendId>(80), returns, 0.5});
  REQUIRE_TRUE(silent(group), "a send to a heard return was treated as silent");
  REQUIRE_TRUE(!silent(audio), "a send to a heard return was treated as silent");
  // Solo silences the rest as mute does.
  state.tracks[2].muted = false;
  state.tracks[0].sends.clear();
  state.tracks[1].soloed = true;
  REQUIRE_TRUE(silent(audio), "solo left a silenced track heard");
  REQUIRE_TRUE(silent(group), "solo left a silenced track heard");
  REQUIRE_TRUE(!silent(other), "solo left a silenced track heard");
  // A track whose key sounds still renders: the key taps it before its
  // fader, so soloing the track it keys silences its audio, not its key.
  const auto slot = id<project::InsertSlotId>(81);
  state.tracks[1].effects.push_back({slot, true, effect_patch("press")});
  state.tracks[0].sends.push_back(
      {id<project::SendId>(82), other, 1.0, project::SendPurpose::sidechain, slot});
  const std::array bound{native_stage(other, slot, 1, 0, true)};
  require(!silent(audio, bound), "a sounding sidechain key was skipped");
  // A muted track keys nothing (issue #245), so nothing renders for it.
  state.tracks[1].soloed = false;
  state.tracks[0].muted = true;
  require(silent(audio, bound), "a muted track was rendered for its silent key");
}

// Issue #245: a key follows its source's mute and the mixer's solo, except
// that a solo keeping the keyed track keeps its key. A silent key carries
// no audio: its edge gain is zero, live and in a recompiled plan.
void a_key_sounds_only_from_a_track_you_can_hear() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  const auto kick = state.tracks[0].id;
  const auto bass = state.tracks[1].id;
  const auto slot = id<project::InsertSlotId>(91);
  const auto send = id<project::SendId>(92);
  state.tracks[1].effects.push_back({slot, true, effect_patch("press")});
  state.tracks[0].sends.push_back({send, bass, 1.0, project::SendPurpose::sidechain, slot});
  const std::array bound{native_stage(bass, slot, 1, 0, true)};
  const auto key_gain = [&] {
    const auto plan = take(registry.compile(state, 64, bound));
    const auto mapped = std::ranges::find(plan.sends, send, &routing::ProjectRouteSend::send);
    REQUIRE_NE(mapped, plan.sends.end(), "the key has an edge");
    const auto edge = std::ranges::find(plan.edges, mapped->edge, &engine::RoutingEdge::id);
    const auto controls = take(routing::project_edge_controls(state, plan));
    const auto control = std::ranges::find(controls, mapped->edge, &engine::RoutedEdgeControl::id);
    REQUIRE_EQ(control->gain, edge->gain, "live and compiled key gains agree");
    return edge->gain;
  };
  const auto silent = [&] { return routing::silent_key_sources(state).contains(kick); };
  REQUIRE_EQ(key_gain(), 1.0F, "a heard kick keys at its gain");
  REQUIRE_TRUE(!silent(), "a heard kick keys at its gain");

  state.tracks[0].muted = true;
  REQUIRE_EQ(key_gain(), 0.0F, "a muted kick keys nothing");
  REQUIRE_TRUE(silent(), "a muted kick keys nothing");
  state.tracks[0].muted = false;

  state.tracks[2].soloed = true;
  REQUIRE_EQ(key_gain(), 0.0F, "soloing another track silences the kick's key");
  REQUIRE_TRUE(silent(), "soloing another track silences the kick's key");
  state.tracks[2].soloed = false;

  state.tracks[1].soloed = true;
  REQUIRE_EQ(key_gain(), 1.0F, "a soloed bass still hears its kick");
  REQUIRE_TRUE(!silent(), "a soloed bass still hears its kick");
  state.tracks[1].soloed = false;

  state.tracks[0].sends.back().gain = 0.0;
  REQUIRE_TRUE(!routing::keys_now(state, routing::silent_key_sources(state), state.tracks[0],
                                  state.tracks[0].sends.back()),
               "a key at no gain keys nothing");
}

// A processor that does nothing, standing in for a prepared chain.
class Stage final : public engine::ProcessorRuntime {
public:
  void process(std::span<engine::StereoFrame>, engine::BlockContext) noexcept override {}
};

// Live playback and export build their mixer nodes the same way: each
// track's chain, mute and channel gain on its node; the master chain on the
// master node only when the mixer runs it.
void graph_nodes_carry_chains_mutes_and_the_master() {
  constexpr double master_level = 0.5;
  constexpr double track_level = 0.25;
  constexpr std::uint32_t master_latency = 32;
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  const auto audio = state.tracks[0].id;
  const auto group = state.tracks[2].id;
  state.tracks[0].main_output = group;
  state.tracks[0].level = track_level;
  state.tracks[2].muted = true;
  state.master_level = master_level;
  const auto plan = take(registry.compile(state, 64, {}));
  const auto chain = std::make_shared<Stage>();
  const auto master = std::make_shared<Stage>();
  const auto on_audio = [&](project::TrackId track) -> std::shared_ptr<engine::ProcessorRuntime> {
    return track == audio ? chain : nullptr;
  };
  const auto node_of = [&](const routing::GraphNodes& graph,
                           std::optional<project::TrackId> track) {
    const auto found = std::ranges::find(graph.owners, track);
    REQUIRE_NE(found, graph.owners.end(), "every track and the master has a node");
    return graph.nodes[static_cast<std::size_t>(found - graph.owners.begin())];
  };

  const auto live = take(routing::graph_nodes(state, plan,
                                              {.track_processor = on_audio,
                                               .master_at_node = true,
                                               .master_processor = master,
                                               .master_latency_frames = master_latency}));
  REQUIRE_EQ(live.nodes.size(), plan.nodes.size(), "one node per route node");
  REQUIRE_TRUE(node_of(live, audio).processor == chain, "the track's chain runs on its node");
  REQUIRE_EQ(node_of(live, audio).output_gain.left, static_cast<float>(track_level),
             "the track's level is its node's gain");
  REQUIRE_TRUE(node_of(live, group).muted, "the muted bus is muted at its node");
  REQUIRE_TRUE(!node_of(live, audio).muted, "only effective mutes apply live");
  REQUIRE_TRUE(live.silent.empty(), "live playback keeps unheard tracks running");
  REQUIRE_TRUE(node_of(live, std::nullopt).processor == master,
               "the master chain runs in the mixer");
  REQUIRE_EQ(node_of(live, std::nullopt).routing.processor_latency_frames, master_latency,
             "the master node reports its chain's latency");
  REQUIRE_EQ(node_of(live, std::nullopt).output_gain.left, static_cast<float>(master_level),
             "the master level is the master node's gain");

  const auto offline = take(
      routing::graph_nodes(state, plan, {.track_processor = on_audio, .mute_silent_tracks = true}));
  REQUIRE_TRUE(node_of(offline, audio).processor == chain, "the track's chain runs on its node");
  REQUIRE_TRUE(node_of(offline, audio).muted, "a track feeding only a muted bus skips its work");
  REQUIRE_EQ(offline.silent.size(), 2U, "the muted bus and what feeds only it are silent");
  REQUIRE_TRUE(node_of(offline, std::nullopt).processor == nullptr,
               "the master chain runs after the mix");
  REQUIRE_EQ(node_of(offline, std::nullopt).output_gain.left, 1.0F,
             "the master level is applied after the mix");
}

void sidechain_send_binds_a_separate_input() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  const auto destination = state.tracks[1].id;
  const auto slot = id<project::InsertSlotId>(80);
  const auto send = id<project::SendId>(81);
  state.tracks[1].effects.push_back({slot, true, effect_patch("press")});
  state.tracks[0].sends.push_back({send, destination, 0.75, project::SendPurpose::sidechain, slot});
  const std::array bound{native_stage(destination, slot, 1, 0, true)};
  const auto route = take(registry.compile(state, 64, bound));
  const auto mapping = std::ranges::find(route.sends, send, &routing::ProjectRouteSend::send);
  REQUIRE_NE(mapping, route.sends.end(), "sidechain send lost its stable route identity");
  const auto edge = std::ranges::find(route.edges, mapping->edge, &engine::RoutingEdge::id);
  REQUIRE_NE(edge, route.edges.end(),
             "sidechain route was mixed into the audible input or lost its target");
  REQUIRE_EQ(edge->kind, engine::RoutingEdgeKind::sidechain,
             "sidechain route was mixed into the audible input or lost its target");
  REQUIRE_EQ(edge->gain, 0.75F,
             "sidechain route was mixed into the audible input or lost its target");
  REQUIRE_EQ(routing::sidechain_target(state, destination), slot,
             "sidechain route was mixed into the audible input or lost its target");
  require(!registry.compile(state, 64),
          "sidechain route compiled without a prepared compressor receipt");
  require_failure(registry.compile(state, 64, std::array{native_stage(destination, slot, 1)}),
                  foundation::ErrorCode::unsupported, "detector",
                  "sidechain route keyed a stage that reads no detector");

  // The keyed stage may share its track with VST3 effects.
  const auto plugin = id<project::PluginInstanceId>(82);
  state.tracks[1].effects.push_back(saved_effect(saved_plugin(plugin)));
  const auto vst3_slot = state.tracks[1].effects.back().id;
  require(!registry.compile(state, 64, bound),
          "sidechain route silently skipped a VST3 effect on the keyed track");
  const std::array shared{bound[0], vst3_stage(destination, vst3_slot, plugin, 6)};
  const auto keyed = take(registry.compile(state, 64, shared));
  REQUIRE_EQ(node_latency(keyed, destination), 6,
             "keyed built-in stage beside a VST3 effect lost its key or latency");
  REQUIRE_NE(std::ranges::find(keyed.sends, send, &routing::ProjectRouteSend::send),
             keyed.sends.end(),
             "keyed built-in stage beside a VST3 effect lost its key or latency");

  // A switched-off target keeps its saved key but needs no detector receipt.
  state.tracks[1].effects.front().enabled = false;
  const auto dormant = take(registry.compile(state, 64, std::array{shared[1]}));
  REQUIRE_TRUE(std::ranges::none_of(dormant.sends,
                                    [send](const auto& mapped) { return mapped.send == send; }),
               "a switched-off sidechain target kept a live key or lost its saved key");
  REQUIRE_TRUE(std::ranges::none_of(dormant.edges,
                                    [](const engine::RoutingEdge& candidate) {
                                      return candidate.kind == engine::RoutingEdgeKind::sidechain;
                                    }),
               "a switched-off sidechain target kept a live key or lost its saved key");
  REQUIRE_TRUE(!routing::sidechain_target(state, destination),
               "a switched-off sidechain target kept a live key or lost its saved key");
  REQUIRE_EQ(state.tracks[0].sends.front().target_insert, slot,
             "a switched-off sidechain target kept a live key or lost its saved key");
}

void vst3_sidechain_needs_exact_auxiliary_bus_receipt() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  const auto destination = state.tracks[1].id;
  const auto plugin = saved_plugin(id<project::PluginInstanceId>(91));
  const auto slot = saved_effect(plugin);
  state.tracks[1].effects.push_back(slot);
  const auto send = id<project::SendId>(92);
  state.tracks[0].sends.push_back(
      {send, destination, 0.75, project::SendPurpose::sidechain, slot.id});
  const auto unverified = vst3_stage(destination, slot.id, plugin.id, 7);
  auto prior = state;
  prior.tracks[0].sends.clear();
  const auto active = take(registry.compile(prior, 64, std::span(&unverified, 1)));
  require_failure(registry.compile(state, 64, std::span(&unverified, 1)),
                  foundation::ErrorCode::unsupported, "detector",
                  "VST3 detector routed without an activated auxiliary-bus receipt");
  const auto still_active = take(registry.compile(prior, 64, std::span(&unverified, 1)));
  REQUIRE_TRUE(std::ranges::equal(still_active.track_nodes, active.track_nodes,
                                  [](const auto& left, const auto& right) {
                                    return left.track == right.track && left.node == right.node;
                                  }),
               "failed VST3 detector preflight changed the prior graph IDs or saved send");
  REQUIRE_EQ(state.tracks[0].sends.front().id, send,
             "failed VST3 detector preflight changed the prior graph IDs or saved send");
  const auto verified = vst3_stage(destination, slot.id, plugin.id, 7, true);
  const auto route = take(registry.compile(state, 64, std::span(&verified, 1)));
  const auto mapping = std::ranges::find(route.sends, send, &routing::ProjectRouteSend::send);
  REQUIRE_NE(mapping, route.sends.end(), "verified sidechain send lost its route identity");
  const auto edge = std::ranges::find(route.edges, mapping->edge, &engine::RoutingEdge::id);
  REQUIRE_NE(edge, route.edges.end(),
             "VST3 detector route lost its independent input, gain, or plug-in latency");
  REQUIRE_EQ(edge->kind, engine::RoutingEdgeKind::sidechain,
             "VST3 detector route lost its independent input, gain, or plug-in latency");
  REQUIRE_EQ(edge->gain, 0.75F,
             "VST3 detector route lost its independent input, gain, or plug-in latency");
  REQUIRE_EQ(route.latency.master_output_latency_frames, 7,
             "VST3 detector route lost its independent input, gain, or plug-in latency");
  auto mismatched = verified;
  mismatched.slot = id<project::InsertSlotId>(999);
  require(!registry.compile(state, 64, std::span(&mismatched, 1)),
          "a detector receipt for another effect slot was accepted");
}

// A sidechain keys a VST3 stage that reads a detector while built-in and
// other VST3 effects share its track; the key may move to a built-in stage.
void keyed_vst3_detector_shares_its_track() {
  routing::ProjectRouteIdRegistry registry;
  auto state = snapshot();
  const auto source = state.tracks[0].id;
  const auto destination = state.tracks[1].id;
  const auto keyed_plugin = id<project::PluginInstanceId>(901);
  const auto other_plugin = id<project::PluginInstanceId>(903);
  const auto eq = id<project::InsertSlotId>(900);
  const auto limiter = id<project::InsertSlotId>(902);
  state.tracks[1].effects = {{eq, true, effect_patch("shape", 1)},
                             saved_effect(saved_plugin(keyed_plugin)),
                             {limiter, true, effect_patch("ceiling", 1)},
                             saved_effect(saved_plugin(other_plugin))};
  const auto keyed_slot = state.tracks[1].effects[1].id;
  const auto other_slot = state.tracks[1].effects[3].id;
  const auto send = id<project::SendId>(904);
  state.tracks[0].sends.push_back(
      {send, destination, 0.5, project::SendPurpose::sidechain, keyed_slot});
  const std::array receipts{native_stage(destination, eq, 1),
                            vst3_stage(destination, keyed_slot, keyed_plugin, 3, true),
                            native_stage(destination, limiter, 1, 5),
                            vst3_stage(destination, other_slot, other_plugin, 2)};
  const auto plan = take(registry.compile(state, 10, receipts));
  const auto mapping = std::ranges::find(plan.sends, send, &routing::ProjectRouteSend::send);
  REQUIRE_NE(mapping, plan.sends.end(), "keyed VST3 stage lost its sidechain send");
  const auto edge = std::ranges::find(plan.edges, mapping->edge, &engine::RoutingEdge::id);
  REQUIRE_NE(edge, plan.edges.end(),
             "keyed VST3 stage among shared effects lost its key, target or latency");
  REQUIRE_EQ(edge->kind, engine::RoutingEdgeKind::sidechain,
             "keyed VST3 stage among shared effects lost its key, target or latency");
  REQUIRE_EQ(edge->source, track_node(plan, source),
             "keyed VST3 stage among shared effects lost its key, target or latency");
  REQUIRE_EQ(edge->destination, track_node(plan, destination),
             "keyed VST3 stage among shared effects lost its key, target or latency");
  REQUIRE_EQ(edge->gain, 0.5F,
             "keyed VST3 stage among shared effects lost its key, target or latency");
  REQUIRE_EQ(node_latency(plan, destination), 10,
             "keyed VST3 stage among shared effects lost its key, target or latency");
  REQUIRE_EQ(routing::sidechain_target(state, destination), keyed_slot,
             "keyed VST3 stage among shared effects lost its key, target or latency");

  auto no_detector = receipts;
  no_detector[1].detector = false;
  require_failure(registry.compile(state, 10, no_detector), foundation::ErrorCode::unsupported,
                  "detector", "a keyed VST3 stage without a detector was accepted");
  auto detector_elsewhere = no_detector;
  detector_elsewhere[3].detector = true;
  require_failure(registry.compile(state, 10, detector_elsewhere),
                  foundation::ErrorCode::unsupported, "detector",
                  "another stage's detector satisfied the keyed slot");

  state.tracks[0].sends.front().target_insert = limiter;
  auto native_keyed = no_detector;
  native_keyed[2].detector = true;
  REQUIRE_OK(registry.compile(state, 10, native_keyed),
             "a keyed built-in stage beside VST3 effects was rejected");
  require_failure(registry.compile(state, 10, receipts), foundation::ErrorCode::unsupported,
                  "detector", "the old keyed stage's detector satisfied the new key");
}

// A VST3 instrument track can be keyed: its keyed effect, VST3 or built-in,
// reads the sidechain after the instrument and beside other effects.
void keyed_stage_on_a_vst3_instrument_track() {
  for (const bool keyed_vst3 : {true, false}) {
    routing::ProjectRouteIdRegistry registry;
    auto state = snapshot();
    auto& instrument = state.tracks[1];
    instrument.kind = project::TrackKind::instrument;
    const auto source = id<project::PluginInstanceId>(950);
    const auto keyed_plugin = id<project::PluginInstanceId>(951);
    const auto other_plugin = id<project::PluginInstanceId>(953);
    instrument.instrument_source = saved_plugin(source, "instrument");
    instrument.effects = {keyed_vst3 ? saved_effect(saved_plugin(keyed_plugin))
                                     : project::EffectInsert{id<project::InsertSlotId>(952), true,
                                                             effect_patch("press", 1)},
                          saved_effect(saved_plugin(other_plugin))};
    const auto keyed_slot = instrument.effects[0].id;
    const auto send = id<project::SendId>(954);
    state.tracks[0].sends.push_back(
        {send, instrument.id, 1.0, project::SendPurpose::sidechain, keyed_slot});
    const auto keyed = keyed_vst3 ? vst3_stage(instrument.id, keyed_slot, keyed_plugin, 2, true)
                                  : native_stage(instrument.id, keyed_slot, 1, 2, true);
    const std::array receipts{instrument_stage(instrument.id, source, 4), keyed,
                              vst3_stage(instrument.id, instrument.effects[1].id, other_plugin, 1)};
    const auto plan = take(registry.compile(state, 7, receipts));
    const auto mapping = std::ranges::find(plan.sends, send, &routing::ProjectRouteSend::send);
    REQUIRE_NE(mapping, plan.sends.end(),
               "keyed stage on a VST3 instrument track lost its key, source or latency");
    REQUIRE_EQ(std::ranges::find(plan.edges, mapping->edge, &engine::RoutingEdge::id)->kind,
               engine::RoutingEdgeKind::sidechain,
               "keyed stage on a VST3 instrument track lost its key, source or latency");
    REQUIRE_EQ(node_latency(plan, instrument.id), 7,
               "keyed stage on a VST3 instrument track lost its key, source or latency");
    REQUIRE_NE(std::ranges::find(plan.sources, instrument.id, &routing::ProjectRouteSource::track),
               plan.sources.end(),
               "keyed stage on a VST3 instrument track lost its key, source or latency");
    auto no_detector = receipts;
    no_detector[1].detector = false;
    require_failure(registry.compile(state, 7, no_detector), foundation::ErrorCode::unsupported,
                    "detector", "a keyed stage on an instrument track read no detector");
    auto instrument_detector = no_detector;
    instrument_detector[0].detector = true;
    require_failure(registry.compile(state, 7, instrument_detector),
                    foundation::ErrorCode::unsupported, "detector",
                    "the instrument's receipt satisfied an effect slot's key");
  }
}

void saved_convolution_requires_a_prepared_runtime() {
  auto state = snapshot();
  const auto source = project::SourceRecord{id<project::SourceId>(90),
                                            project::SourceEncoding::wav,
                                            std::string(64, 'a'),
                                            192'044,
                                            48'000,
                                            1,
                                            96'000};
  state.sources.push_back(source);
  const project::ManagedAudioReference ir{source.id,          source.sha256,   source.byte_length,
                                          source.sample_rate, source.channels, source.frames};
  state.tracks[0].effects.push_back(
      {id<project::InsertSlotId>(91), false, project::NativeConvolution{ir, 0.4, 1}});
  routing::ProjectRouteIdRegistry registry;
  const auto result = registry.compile(state, 128);
  REQUIRE_TRUE(!result, "bypassed saved convolution was silently omitted from a route");
  REQUIRE_EQ(result.error().code, foundation::ErrorCode::unsupported,
             "bypassed saved convolution was silently omitted from a route");
  REQUIRE_NE(result.error().message.find("Convolution"), std::string::npos,
             "bypassed saved convolution was silently omitted from a route");
  REQUIRE_OK(registry.compile(state, 128, {}, true),
             "offline capability rejected a bypassed convolution slot");
  state.tracks[0].effects.front().enabled = true;
  require(!registry.compile(state, 128, {}, true),
          "enabled convolution accepted without an ordered runtime receipt");
  const std::array receipts{
      convolution_stage(state.tracks[0].id, state.tracks[0].effects.front().id, 1)};
  REQUIRE_OK(registry.compile(state, 128, receipts, true),
             "exact convolution runtime receipt did not bind");
  require(!registry.compile(state, 128, receipts),
          "convolution receipt bound without an available convolution runtime");
  auto stale = receipts;
  stale.front().generation = 2;
  require(!registry.compile(state, 128, stale, true),
          "stale convolution runtime receipt was accepted");
  auto wrong_kind = receipts;
  wrong_kind.front().processor = routing::StageProcessor::native_effect;
  require(!registry.compile(state, 128, wrong_kind, true),
          "a built-in effect receipt was accepted for a convolution slot");
}
} // namespace

int main() {
  try {
    stable_ids_and_order();
    full_project_ids_and_parallel_edges();
    errors_do_not_consume_ids();
    structural_limits_and_main_destinations();
    verified_insert_latency_and_fail_closed_binding();
    two_verified_audio_effect_receipts_are_independent();
    two_serial_effects_sum_latency_and_bind_slots();
    separate_native_limiters_align_parallel_tracks();
    instrument_and_separate_effect_bind_exact_roles();
    two_bare_vst3_sources_bind_distinct_tracks_and_fail_closed();
    instrument_and_own_effect_are_one_serial_path();
    instrument_and_two_own_effects_are_one_serial_path();
    many_vst3_instances_compile_in_one_project();
    native_and_vst3_effects_interleave_in_any_saved_order();
    vst3_instruments_with_arbitrary_chains_beside_native_tracks();
    bus_vst3_effect_preserves_parallel_alignment();
    instrument_and_separate_native_chain_keep_latency_and_roles();
    vst3_instrument_native_chain_is_one_serial_path();
    synth_native_chain_has_one_serial_latency_path();
    stage_bound_counts_the_vst3_instrument();
    receipt_mismatches_fail_closed();
    solo_projection_retains_routes_and_respects_explicit_mute();
    silent_tracks_are_muted_or_feed_only_muted_tracks_or_silent_keys();
    a_key_sounds_only_from_a_track_you_can_hear();
    channel_balance_preserves_center_unity();
    sidechain_send_binds_a_separate_input();
    vst3_sidechain_needs_exact_auxiliary_bus_receipt();
    keyed_vst3_detector_shares_its_track();
    keyed_stage_on_a_vst3_instrument_track();
    saved_convolution_requires_a_prepared_runtime();
    saved_projection_shapes_fail_closed();
    bypassed_native_track_eq_needs_no_runtime_receipt();
    bypassed_missing_vst3_between_native_stages_is_inert();
    ordered_native_route_receipts_require_exact_slots_and_generations();
    native_bus_eq_requires_exact_receipt_and_maps_bus_node();
    mixed_eq_vst3_receipts_accept_both_saved_orders();
    mixed_limiter_vst3_receipts_sum_latency_and_bind_exact_slot();
    mixed_bus_receipts_preserve_order_and_parallel_pdc();
    mixed_bus_accepts_each_zero_latency_native_kind();
    graph_nodes_carry_chains_mutes_and_the_master();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
