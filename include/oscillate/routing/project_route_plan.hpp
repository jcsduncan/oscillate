#pragma once

#include "oscillate/engine/routed_mixer.hpp"

#include "oscillate/engine/routing_latency.hpp"
#include "oscillate/project/model.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace oscillate::routing {

struct ProjectRouteSource {
  project::TrackId track;
  engine::RoutingNodeId node;
  std::size_t topological_index;
};

struct ProjectRouteSend {
  project::TrackId track;
  project::SendId send;
  engine::RoutingEdgeId edge;
};

struct ProjectRouteTrackNode {
  project::TrackId track;
  engine::RoutingNodeId node;
};

// The kind of processor a prepared stage runs.
enum class StageProcessor : std::uint8_t { vst3, native_effect, convolution };

// A control-side receipt for one prepared stage of a track's chain (ADR-0050).
// A track's chain is its VST3 instrument, when it has one, then each enabled
// saved effect in saved order, built-in or VST3. The caller prepares every
// stage and supplies one receipt per stage, in that order; a receipt does not
// instantiate or identify a processor runtime. Absence of a receipt never
// causes a saved processor to be silently bypassed.
struct BoundStage {
  project::TrackId track;
  // The effect slot; absent for the track's VST3 instrument.
  std::optional<project::InsertSlotId> slot{};
  StageProcessor processor = StageProcessor::native_effect;
  // A VST3 stage's saved plugin instance.
  std::optional<project::PluginInstanceId> plugin{};
  // A built-in stage's saved generation.
  std::uint64_t generation = 0;
  // The stage's exact prepared processing delay.
  std::uint32_t latency_frames = 0;
  // The prepared stage reads a detector input, so a sidechain may key it.
  // Preparation proves this; saved sends and scan metadata cannot.
  bool detector = false;
};

// Control-side routing intent only. No audio source, processor, sidechain bus,
// runtime state or publication is created by this plan.
struct ProjectRoutePlan {
  engine::RoutingNodeId master;
  std::vector<engine::RoutingNode> nodes;
  std::vector<engine::RoutingEdge> edges;
  engine::RoutingLatencyPlan latency;
  std::vector<ProjectRouteSource> sources;
  std::vector<ProjectRouteSend> sends;
  // Includes group and return buses, which have no external source slot.
  std::vector<ProjectRouteTrackNode> track_nodes;
};

// Control-side mute projection. A solo retains its upstream contributors and
// downstream path to master; explicit track mute still wins. The result is in
// plan.track_nodes order and is shared by live and offline rendering.
[[nodiscard]] std::vector<bool> effective_track_mutes(const project::Snapshot& snapshot,
                                                      const ProjectRoutePlan& plan);

// The tracks whose rendering cannot be heard, in plan.track_nodes order: each
// effectively muted track, and each track whose every route leads only to
// such tracks. A track that keys a sidechain is never silent, since the key
// taps it before its mute. A render whose mutes cannot change may skip them.
[[nodiscard]] std::vector<bool> silent_track_nodes(const project::Snapshot& snapshot,
                                                   const ProjectRoutePlan& plan);

struct TrackStereoGain {
  float left;
  float right;
};
// Post-insert stereo balance with unity at center. Saved level is linear 0..4.
// A channel's post-fader gains for a level and pan: pan attenuates the far side.
[[nodiscard]] TrackStereoGain channel_stereo_gain(double level, double pan) noexcept;
[[nodiscard]] TrackStereoGain track_stereo_gain(const project::Track& track) noexcept;
// Build a complete node-control projection for a route with unchanged topology.
// The engine separately admits the resulting values against the active mixer.
[[nodiscard]] foundation::Result<std::vector<engine::RoutedNodeControl>>
project_node_controls(const project::Snapshot& snapshot, const ProjectRoutePlan& plan);
// Existing edge order and IDs, replacing only the saved gain on mapped sends.
[[nodiscard]] foundation::Result<std::vector<engine::RoutedEdgeControl>>
project_edge_controls(const project::Snapshot& snapshot, const ProjectRoutePlan& plan);
// A sidechain key is live only while the effect it keys is in the chain and
// switched on; a switched-off effect keeps its key saved for when it returns.
[[nodiscard]] bool live_sidechain(const project::Snapshot& snapshot,
                                  const project::SendRoute& send);
// Validated snapshots have at most one compressor target per destination.
[[nodiscard]] std::optional<project::InsertSlotId>
sidechain_target(const project::Snapshot& snapshot, project::TrackId destination);

// One registry per open project/session. IDs are never reused during the
// session, including after a track/send is removed; failed builds do not
// consume IDs. Do not call on the audio callback.
class ProjectRouteIdRegistry final {
public:
  // Bounds the per-boundary histories a chain keeps for latency handover.
  static constexpr std::size_t max_stages_per_path = 64;

  // Every saved processor on a track needs its exact receipt, in chain
  // order; unused receipts are rejected. Master effects are the caller's:
  // the master node carries no stage latency here.
  [[nodiscard]] foundation::Result<ProjectRoutePlan>
  compile(const project::Snapshot& snapshot, std::uint32_t max_compensation_frames,
          std::span<const BoundStage> stages = {}, bool convolution_runtime_available = false);

private:
  std::map<foundation::IdBits, engine::RoutingNodeId> tracks_;
  std::map<foundation::IdBits, engine::RoutingEdgeId> main_edges_;
  std::map<foundation::IdBits, engine::RoutingEdgeId> send_edges_;
  std::uint64_t next_id_ = 2; // Master is always 1.
};

} // namespace oscillate::routing
