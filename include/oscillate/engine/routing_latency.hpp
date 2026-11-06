#pragma once

#include "oscillate/foundation/error.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace oscillate::engine {

using RoutingNodeId = std::uint64_t;
using RoutingEdgeId = std::uint64_t;

enum class RoutingEdgeKind : std::uint8_t { main, sidechain };

struct RoutingNode {
  RoutingNodeId id;
  std::uint32_t processor_latency_frames;
};

// Audio, send and sidechain connections all participate in arrival alignment.
// A distinct stable ID identifies each connection even between the same nodes.
struct RoutingEdge {
  RoutingEdgeId id;
  RoutingNodeId source;
  RoutingNodeId destination;
  // Linear amplitude, validated on the control side. Existing main routes
  // default to unity; a send may use a separately owned edge and gain.
  float gain = 1.0F;
  RoutingEdgeKind kind = RoutingEdgeKind::main;
};

struct PlannedNodeLatency {
  RoutingNodeId id;
  std::uint64_t output_latency_frames;
};

struct PlannedEdgeDelay {
  RoutingEdgeId id;
  RoutingNodeId source;
  RoutingNodeId destination;
  std::uint32_t compensation_frames;
  float gain;
  RoutingEdgeKind kind = RoutingEdgeKind::main;
};

// Immutable, control-side plan. Delay-line allocation, history priming and
// block-boundary publication are separate contracts; this contains no runtime.
struct RoutingLatencyPlan {
  std::vector<RoutingNodeId> topological_order;
  std::vector<PlannedNodeLatency> nodes;
  std::vector<PlannedEdgeDelay> edges;
  std::uint64_t master_output_latency_frames = 0;
};

struct RoutingLatencyOptions {
  RoutingNodeId master;
  std::uint32_t max_compensation_frames;
};

// The caller supplies the measured PDC limit. No default budget is implied.
// All nodes must reach master. Graph construction and diagnostics run off RT.
[[nodiscard]] foundation::Result<RoutingLatencyPlan>
prepare_routing_latency(std::span<const RoutingNode> nodes, std::span<const RoutingEdge> edges,
                        RoutingLatencyOptions options);

} // namespace oscillate::engine
