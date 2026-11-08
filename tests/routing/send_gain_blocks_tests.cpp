#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/routing/route_gain_blocks.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace automation_project = oscillate::automation_project;
namespace engine = oscillate::engine;
namespace foundation = oscillate::foundation;
namespace project = oscillate::project;
namespace routing = oscillate::routing;
namespace timeline = oscillate::timeline;

using oscillate::test::require;
using oscillate::test::take;
template <typename T> T id(std::uint64_t low) { return take(T::from_bits({0, low})); }
constexpr auto max_block_frames = routing::PreparedRouteGainBlocks::max_block_frames;

project::Snapshot snapshot_with_send(std::size_t sends = 1) {
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  project::Snapshot snapshot{id<project::ProjectId>(1), 1, 48'000, std::move(tempo),
                             std::move(meter),          {}};
  const auto bus = id<project::TrackId>(2);
  for (std::size_t index = 0; index < sends; ++index) {
    const auto track = id<project::TrackId>(10 + index);
    const auto send = id<project::SendId>(1'000 + index);
    project::Track source{track, {}, project::TrackKind::audio};
    source.sends.push_back({send, bus, 0.25});
    source.automation.push_back(
        {id<project::AutomationLaneId>(2'000 + index),
         project::SendGainTarget{send},
         {project::AutomationRepresentation::physical, "mixer.send_gain", "linear_gain", 0.0, 4.0,
          0.25, false, 0, project::AutomationScale::decibel},
         project::AutomationTimebase::elapsed,
         {{id<project::AutomationPointId>(3'000 + index), 0, 0.5}},
         1});
    snapshot.tracks.push_back(std::move(source));
  }
  snapshot.tracks.emplace_back(bus, std::vector<project::Clip>{}, project::TrackKind::return_bus);
  return snapshot;
}

void binding_and_bounds() {
  auto snapshot = snapshot_with_send();
  routing::ProjectRouteIdRegistry ids;
  auto route = take(ids.compile(snapshot, 64));
  auto lanes = take(automation_project::prepare_project_automation(snapshot, 1));
  auto adapter = take(routing::PreparedRouteGainBlocks::prepare(snapshot, route, lanes, 4));
  require(std::ranges::none_of(route.latency.nodes,
                               [&](const auto& node) { return adapter->binds_node_gain(node.id); }),
          "send-only automation falsely blocked channel control overlays");
  engine::RoutedGainSpans block;
  REQUIRE_TRUE(adapter->fill_block({0, 1}, 4, block),
               "valid send block was not bound to edge plan");
  REQUIRE_EQ(block.edges.size(), route.latency.edges.size(),
             "valid send block was not bound to edge plan");
  REQUIRE_TRUE(block.nodes.empty(), "valid send block was not bound to edge plan");
  const auto dynamic = std::ranges::find_if(
      block.edges, [](const engine::EdgeGainSpan& edge) { return !edge.values.empty(); });
  REQUIRE_NE(dynamic, block.edges.end(), "send gain values did not reach stable edge span");
  REQUIRE_EQ(dynamic->values.size(), 4, "send gain values did not reach stable edge span");
  REQUIRE_EQ(dynamic->values[0], 0.5F, "send gain values did not reach stable edge span");
  REQUIRE_EQ(dynamic->values[3], 0.5F, "send gain values did not reach stable edge span");
  REQUIRE_TRUE(adapter->binds_edge_gain(dynamic->id),
               "send automation overlay admission did not isolate its bound edge");
  REQUIRE_TRUE(std::ranges::any_of(block.edges,
                                   [&](const engine::EdgeGainSpan& edge) {
                                     return edge.id != dynamic->id &&
                                            !adapter->binds_edge_gain(edge.id);
                                   }),
               "send automation overlay admission did not isolate its bound edge");
  REQUIRE_TRUE(!adapter->fill_block({0, 0}, 4, block),
               "invalid callback span or position was accepted");
  REQUIRE_TRUE(block.edges.empty(), "invalid callback span or position was accepted");
  REQUIRE_TRUE(!adapter->fill_block({0, 1}, 5, block),
               "invalid callback span or position was accepted");
  REQUIRE_TRUE(block.edges.empty(), "invalid callback span or position was accepted");
  REQUIRE_TRUE(
      !adapter->fill_block(
          {static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()), 1}, 4, block),
      "invalid callback span or position was accepted");
  REQUIRE_TRUE(block.edges.empty(), "invalid callback span or position was accepted");
  REQUIRE_TRUE(!routing::PreparedRouteGainBlocks::prepare(snapshot, route, lanes, 0),
               "invalid prepared block size was accepted");
  REQUIRE_TRUE(!routing::PreparedRouteGainBlocks::prepare(snapshot, route, lanes, 8193),
               "invalid prepared block size was accepted");
  ++snapshot.revision;
  auto stale = routing::PreparedRouteGainBlocks::prepare(snapshot, route, lanes, 4);
  REQUIRE_TRUE(!stale, "stale prepared lane was accepted");
  REQUIRE_EQ(stale.error().code, foundation::ErrorCode::conflict,
             "stale prepared lane was accepted");
  --snapshot.revision;
  auto absent_send = route;
  absent_send.sends.clear();
  auto missing = routing::PreparedRouteGainBlocks::prepare(snapshot, absent_send, lanes, 4);
  REQUIRE_TRUE(!missing, "unrouted send lane was silently omitted");
  REQUIRE_EQ(missing.error().code, foundation::ErrorCode::unavailable,
             "unrouted send lane was silently omitted");
  auto absent_edge = route;
  absent_edge.latency.edges.clear();
  missing = routing::PreparedRouteGainBlocks::prepare(snapshot, absent_edge, lanes, 4);
  REQUIRE_TRUE(!missing, "missing mixer edge was silently omitted");
  REQUIRE_EQ(missing.error().code, foundation::ErrorCode::unavailable,
             "missing mixer edge was silently omitted");
  lanes.push_back(lanes.front());
  auto duplicate = routing::PreparedRouteGainBlocks::prepare(snapshot, route, lanes, 4);
  REQUIRE_TRUE(!duplicate, "duplicate binding to one send edge was accepted");
  REQUIRE_EQ(duplicate.error().code, foundation::ErrorCode::conflict,
             "duplicate binding to one send edge was accepted");
}

void memory_budget() {
  auto snapshot = snapshot_with_send(171);
  routing::ProjectRouteIdRegistry ids;
  auto route = take(ids.compile(snapshot, 64));
  auto lanes = take(automation_project::prepare_project_automation(snapshot, 1));
  auto oversized =
      routing::PreparedRouteGainBlocks::prepare(snapshot, route, lanes, max_block_frames);
  REQUIRE_TRUE(!oversized, "send gain scratch exceeded its explicit memory budget");
  REQUIRE_EQ(oversized.error().code, foundation::ErrorCode::resource_exhausted,
             "send gain scratch exceeded its explicit memory budget");

  snapshot = snapshot_with_send(100);
  for (std::size_t index = 0; index < 100; ++index) {
    auto& track = snapshot.tracks[index];
    track.automation.push_back(
        {id<project::AutomationLaneId>(6'000 + index),
         project::TrackControlTarget{track.id, project::TrackControlTarget::Key::channel_level},
         {project::AutomationRepresentation::physical, "mixer.channel_level", "linear_gain", 0.0,
          4.0, 1.0, false, 0, project::AutomationScale::decibel},
         project::AutomationTimebase::elapsed,
         {{id<project::AutomationPointId>(7'000 + index), 0, 0.5}},
         1});
  }
  route = take(ids.compile(snapshot, 64));
  lanes = take(automation_project::prepare_project_automation(snapshot, 1));
  oversized = routing::PreparedRouteGainBlocks::prepare(snapshot, route, lanes, max_block_frames);
  REQUIRE_TRUE(!oversized, "combined send and channel scratch exceeded its memory budget");
  REQUIRE_EQ(oversized.error().code, foundation::ErrorCode::resource_exhausted,
             "combined send and channel scratch exceeded its memory budget");
}

void dormant_sidechain_automation_stays_saved() {
  auto snapshot = snapshot_with_send();
  auto& bus = snapshot.tracks.back();
  const auto slot = id<project::InsertSlotId>(4'000);
  bus.effects.push_back({slot, false, project::NativeEffectPatch{"press", 1, {}, 1}});
  auto& send = snapshot.tracks.front().sends.front();
  send.purpose = project::SendPurpose::sidechain;
  send.target_insert = slot;
  routing::ProjectRouteIdRegistry ids;
  auto route = take(ids.compile(snapshot, 64));
  REQUIRE_TRUE(route.sends.empty(), "a switched-off sidechain target compiled a live key");
  auto lanes = take(automation_project::prepare_project_automation(snapshot, 1));
  auto adapter = take(routing::PreparedRouteGainBlocks::prepare(snapshot, route, lanes, 4));
  engine::RoutedGainSpans block;
  REQUIRE_TRUE(adapter->fill_block({0, 1}, 4, block),
               "a dormant sidechain send's automation blocked the project");
  REQUIRE_TRUE(
      std::ranges::none_of(block.edges,
                           [](const engine::EdgeGainSpan& edge) { return !edge.values.empty(); }),
      "a dormant sidechain send's automation drove a live edge");

  // A plain send that lost its route is still an error.
  send.purpose = project::SendPurpose::audible;
  send.target_insert.reset();
  auto unrouted = route;
  unrouted.sends.clear();
  lanes = take(automation_project::prepare_project_automation(snapshot, 1));
  require(!routing::PreparedRouteGainBlocks::prepare(snapshot, unrouted, lanes, 4),
          "an unrouted live send's automation was silently dropped");
}

void channel_level_binding_preserves_stereo_balance() {
  auto snapshot = snapshot_with_send();
  auto& track = snapshot.tracks.front();
  track.level = 0.8;
  track.pan = 0.25;
  track.automation.push_back(
      {id<project::AutomationLaneId>(5'000),
       project::TrackControlTarget{track.id, project::TrackControlTarget::Key::channel_level},
       {project::AutomationRepresentation::physical, "mixer.channel_level", "linear_gain", 0.0, 4.0,
        0.8, false, 0, project::AutomationScale::decibel},
       project::AutomationTimebase::elapsed,
       {{id<project::AutomationPointId>(5'001), 0, 0.5, project::AutomationShape::linear},
        {id<project::AutomationPointId>(5'002), 2, 1.0}},
       1});
  track.clips.emplace_back(
      id<project::ClipId>(5'010), id<project::SourceId>(5'011), project::SourceRange{{0}, {2}},
      project::ElapsedInterval{{1}, {3}}, 1,
      std::vector<project::AutomationLane>{
          {id<project::AutomationLaneId>(5'012),
           project::TrackControlTarget{track.id, project::TrackControlTarget::Key::channel_level},
           {project::AutomationRepresentation::physical, "mixer.channel_level", "linear_gain", 0.0,
            4.0, 0.8, false, 0, project::AutomationScale::decibel},
           project::AutomationTimebase::elapsed,
           {{id<project::AutomationPointId>(5'013), 0, 0.2}},
           1}});
  routing::ProjectRouteIdRegistry ids;
  auto route = take(ids.compile(snapshot, 64));
  auto lanes = take(automation_project::prepare_project_automation(snapshot, snapshot.revision));
  auto adapter = take(routing::PreparedRouteGainBlocks::prepare(snapshot, route, lanes, 4));
  engine::RoutedGainSpans block;
  REQUIRE_TRUE(adapter->fill_block({0, 1}, 4, block),
               "channel gain block was not bound to routed nodes");
  REQUIRE_EQ(block.nodes.size(), route.latency.nodes.size(),
             "channel gain block was not bound to routed nodes");
  const auto mapped =
      std::ranges::find(route.track_nodes, track.id, &routing::ProjectRouteTrackNode::track);
  REQUIRE_NE(mapped, route.track_nodes.end(), "channel route is absent");
  const auto node_id = mapped->node;
  REQUIRE_TRUE(adapter->binds_node_gain(node_id),
               "channel automation admission did not isolate its bound node");
  REQUIRE_TRUE(!adapter->binds_node_gain(route.master),
               "channel automation admission did not isolate its bound node");
  const auto node = std::ranges::find(block.nodes, node_id, &engine::NodeGainSpan::id);
  REQUIRE_NE(node, block.nodes.end(),
             "channel clip envelope did not take precedence then return to track curve");
  REQUIRE_EQ(node->values.size(), 4,
             "channel clip envelope did not take precedence then return to track curve");
  REQUIRE_EQ(node->values[0].left, 0.375F,
             "channel clip envelope did not take precedence then return to track curve");
  REQUIRE_EQ(node->values[0].right, 0.5F,
             "channel clip envelope did not take precedence then return to track curve");
  REQUIRE_EQ(node->values[1].left, 0.15F,
             "channel clip envelope did not take precedence then return to track curve");
  REQUIRE_EQ(node->values[1].right, 0.2F,
             "channel clip envelope did not take precedence then return to track curve");
  REQUIRE_EQ(node->values[2].left, 0.15F,
             "channel clip envelope did not take precedence then return to track curve");
  REQUIRE_EQ(node->values[2].right, 0.2F,
             "channel clip envelope did not take precedence then return to track curve");
  REQUIRE_EQ(node->values[3].left, 0.75F,
             "channel clip envelope did not take precedence then return to track curve");
  REQUIRE_EQ(node->values[3].right, 1.0F,
             "channel clip envelope did not take precedence then return to track curve");
  auto missing = route;
  missing.track_nodes.clear();
  require(!routing::PreparedRouteGainBlocks::prepare(snapshot, missing, lanes, 4),
          "unrouted channel automation was silently omitted");
}
// ADR-0097: a sidechain key from a source under a mute lane carries the
// source channel's mute gain, delayed as the edge delays the key, so it is
// silent on exactly the audio the channel's mute silences.
void a_mute_lane_gates_its_sources_key() {
  constexpr std::int64_t mute_from = 1'000;
  // Past the 5 ms mute glide at 48 kHz (240 frames).
  constexpr std::uint64_t past_the_glide = 400;
  constexpr std::uint32_t key_delay = 16;
  constexpr std::size_t frames = 4;
  constexpr float saved_gain = 0.25F;
  constexpr double lane_unmuted = 0.0;
  constexpr double lane_muted = 1.0;
  auto snapshot = snapshot_with_send();
  auto& bus = snapshot.tracks.back();
  const auto slot = id<project::InsertSlotId>(4'000);
  bus.effects.push_back({slot, true, project::NativeEffectPatch{"press", 1, {}, 1}});
  auto& source = snapshot.tracks.front();
  auto& send = source.sends.front();
  send.purpose = project::SendPurpose::sidechain;
  send.target_insert = slot;
  // The key's own gain is its saved gain; the source's mute is its lane's.
  source.automation.clear();
  const auto domain =
      project::channel_control_domain(project::TrackControlTarget::Key::channel_mute);
  source.automation.push_back(
      {id<project::AutomationLaneId>(5'000),
       project::TrackControlTarget{source.id, project::TrackControlTarget::Key::channel_mute},
       {project::AutomationRepresentation::physical, domain.semantic_key, domain.unit,
        domain.minimum, domain.maximum, lane_unmuted, domain.discrete, domain.step_count,
        domain.scale},
       project::AutomationTimebase::elapsed,
       {{id<project::AutomationPointId>(5'001), 0, lane_unmuted},
        {id<project::AutomationPointId>(5'002), mute_from, lane_muted}},
       1});
  const std::array stages{routing::BoundStage{.track = bus.id,
                                              .slot = slot,
                                              .processor = routing::StageProcessor::native_effect,
                                              .generation = 1,
                                              .detector = true}};
  routing::ProjectRouteIdRegistry ids;
  auto route = take(ids.compile(snapshot, 64, stages));
  REQUIRE_EQ(route.sends.size(), 1, "the key is live");
  const auto edge_id = route.sends.front().edge;
  auto lanes = take(automation_project::prepare_project_automation(snapshot, snapshot.revision));
  const auto mapped =
      std::ranges::find(route.track_nodes, source.id, &routing::ProjectRouteTrackNode::track);
  REQUIRE_NE(mapped, route.track_nodes.end(), "the source has a channel node");
  const auto node_id = mapped->node;
  struct Gains {
    std::vector<float> key;
    std::vector<float> channel;
  };
  // Plays `length` frames from `start` in one processing generation.
  const auto play = [&](routing::PreparedRouteGainBlocks& adapter, std::uint64_t start,
                        std::uint64_t length, std::uint64_t generation) {
    Gains gains;
    for (auto position = start; position < start + length; position += frames) {
      engine::RoutedGainSpans block;
      REQUIRE_TRUE(adapter.fill_block({position, generation}, frames, block),
                   "the key's gain block fills");
      const auto edge = std::ranges::find(block.edges, edge_id, &engine::EdgeGainSpan::id);
      const auto node = std::ranges::find(block.nodes, node_id, &engine::NodeGainSpan::id);
      REQUIRE_NE(edge, block.edges.end(), "the key's gain reaches its edge");
      REQUIRE_NE(node, block.nodes.end(), "the channel's gain reaches its node");
      REQUIRE_EQ(edge->values.size(), frames, "the key's gain reaches its edge");
      gains.key.insert(gains.key.end(), edge->values.begin(), edge->values.end());
      for (const auto& frame : node->values)
        gains.channel.push_back(frame.left);
    }
    return gains;
  };
  constexpr std::uint64_t first_generation = 1;
  constexpr std::uint64_t seek_generation = 2;
  constexpr auto length = static_cast<std::uint64_t>(mute_from) + past_the_glide;

  auto adapter = take(routing::PreparedRouteGainBlocks::prepare(snapshot, route, lanes, frames));
  REQUIRE_TRUE(adapter->binds_edge_gain(edge_id), "the source's mute lane binds its key");
  const auto played = play(*adapter, 0, length, first_generation);
  for (std::size_t frame = 0; frame < played.key.size(); ++frame)
    REQUIRE_EQ(played.key[frame], saved_gain * played.channel[frame],
               "the key takes its channel's mute gain, frame " + std::to_string(frame));
  REQUIRE_EQ(played.key.front(), saved_gain, "an open lane keeps the key's saved gain");
  REQUIRE_EQ(played.key.back(), 0.0F, "the key is silent where the lane mutes");

  auto delayed = route;
  const auto planned =
      std::ranges::find(delayed.latency.edges, edge_id, &engine::PlannedEdgeDelay::id);
  REQUIRE_NE(planned, delayed.latency.edges.end(), "the key has a planned edge");
  planned->compensation_frames = key_delay;
  adapter = take(routing::PreparedRouteGainBlocks::prepare(snapshot, delayed, lanes, frames));
  const auto late = play(*adapter, 0, length, first_generation);
  for (std::size_t frame = key_delay; frame < late.key.size(); ++frame)
    REQUIRE_EQ(late.key[frame], saved_gain * late.channel[frame - key_delay],
               "a delayed key takes the mute its audio left with, frame " + std::to_string(frame));
  REQUIRE_EQ(late.key[static_cast<std::size_t>(mute_from) + key_delay - 1], saved_gain,
             "a delayed key still carries audio from before the mute");

  // A seek just past the step starts the channel muted; the key, whose
  // delay line starts again too, is silent from its first frame.
  const auto seek = static_cast<std::uint64_t>(mute_from) + 1;
  const auto sought = play(*adapter, seek, past_the_glide, seek_generation);
  require(std::ranges::all_of(sought.key, [](float gain) { return gain == 0.0F; }),
          "a key sought past its source's mute stays silent");
}
} // namespace

int main() {
  try {
    binding_and_bounds();
    memory_budget();
    dormant_sidechain_automation_stays_saved();
    channel_level_binding_preserves_stereo_balance();
    a_mute_lane_gates_its_sources_key();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
