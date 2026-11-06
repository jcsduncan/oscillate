#include "oscillate/engine/routing_latency.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace oscillate::engine;

using oscillate::test::require;

std::uint64_t arrival(const RoutingLatencyPlan& plan, const RoutingEdge& edge) {
  const auto source = std::ranges::find_if(
      plan.nodes, [id = edge.source](const auto& node) { return node.id == id; });
  const auto route =
      std::ranges::find_if(plan.edges, [id = edge.id](const auto& item) { return item.id == id; });
  REQUIRE_NE(source, plan.nodes.end(), "planned path missing");
  REQUIRE_NE(route, plan.edges.end(), "planned path missing");
  return source->output_latency_frames + route->compensation_frames;
}

void parallel_impulse_alignment_and_latency_change() {
  const std::array<RoutingNode, 4> nodes{{{3, 16}, {1, 0}, {4, 0}, {2, 64}}};
  const std::array<RoutingEdge, 3> edges{{{13, 3, 4}, {11, 1, 3}, {12, 2, 3}}};
  auto result = prepare_routing_latency(nodes, edges, {4, 128});
  REQUIRE_OK(result, "valid parallel routing graph rejected");
  const auto& plan = result.value();
  REQUIRE_EQ(plan.topological_order, std::vector<RoutingNodeId>({1, 2, 3, 4}),
             "topological order is not deterministic by stable ID");
  REQUIRE_EQ(arrival(plan, edges[1]), 64,
             "parallel impulse arrivals do not align at bus and master");
  REQUIRE_EQ(arrival(plan, edges[2]), 64,
             "parallel impulse arrivals do not align at bus and master");
  REQUIRE_EQ(arrival(plan, edges[0]), 80,
             "parallel impulse arrivals do not align at bus and master");
  REQUIRE_EQ(plan.master_output_latency_frames, 80,
             "parallel impulse arrivals do not align at bus and master");
  const std::array<RoutingNode, 4> slower{{{3, 16}, {1, 0}, {4, 0}, {2, 128}}};
  result = prepare_routing_latency(slower, edges, {4, 128});
  REQUIRE_TRUE(result, "plugin latency growth was not recomputed");
  REQUIRE_EQ(arrival(result.value(), edges[1]), 128, "plugin latency growth was not recomputed");
  REQUIRE_EQ(arrival(result.value(), edges[2]), 128, "plugin latency growth was not recomputed");
  REQUIRE_EQ(result.value().master_output_latency_frames, 144,
             "plugin latency growth was not recomputed");
  require(!prepare_routing_latency(slower, edges, {4, 127}), "over-budget parallel path accepted");
}

void sends_sidechains_and_equal_paths() {
  const std::array<RoutingNode, 5> nodes{{{1, 32}, {2, 16}, {3, 48}, {4, 0}, {5, 4}}};
  // IDs 10 and 11 may be a main send and sidechain from one source. The
  // planner treats both as timed audio paths, including duplicate endpoints.
  const std::array<RoutingEdge, 6> edges{
      {{10, 1, 4}, {11, 1, 4}, {12, 2, 4}, {13, 3, 4}, {14, 4, 5}, {15, 2, 3}}};
  auto result = prepare_routing_latency(nodes, edges, {5, 128});
  REQUIRE_OK(result, "send/sidechain graph rejected");
  const auto& plan = result.value();
  REQUIRE_EQ(arrival(plan, edges[0]), arrival(plan, edges[1]), "all bus inputs were not aligned");
  REQUIRE_EQ(arrival(plan, edges[1]), arrival(plan, edges[2]), "all bus inputs were not aligned");
  REQUIRE_EQ(arrival(plan, edges[2]), arrival(plan, edges[3]), "all bus inputs were not aligned");
  REQUIRE_EQ(plan.master_output_latency_frames, arrival(plan, edges[4]) + 4,
             "master latency omitted a processor");
}

void invalid_graphs() {
  const std::array<RoutingNode, 2> nodes{{{1, 0}, {2, 0}}};
  const std::array<RoutingEdge, 1> good{{{10, 1, 2}}};
  require(!prepare_routing_latency({}, good, {2, 0}), "empty graph accepted");
  require(!prepare_routing_latency(nodes, good, {0, 0}), "zero master accepted");
  require(!prepare_routing_latency(nodes, good, {99, 0}), "missing master accepted");
  const std::array<RoutingNode, 2> duplicate_nodes{{{1, 0}, {1, 0}}};
  require(!prepare_routing_latency(duplicate_nodes, good, {1, 0}), "duplicate node accepted");
  const std::array<RoutingNode, 2> zero_node{{{0, 0}, {2, 0}}};
  require(!prepare_routing_latency(zero_node, good, {2, 0}), "zero node ID accepted");
  const std::array<RoutingEdge, 1> zero_edge{{{0, 1, 2}}};
  require(!prepare_routing_latency(nodes, zero_edge, {2, 0}), "zero edge ID accepted");
  const std::array<RoutingEdge, 2> duplicate_edges{{{10, 1, 2}, {10, 1, 2}}};
  require(!prepare_routing_latency(nodes, duplicate_edges, {2, 0}), "duplicate edge accepted");
  const std::array<RoutingEdge, 1> missing{{{10, 1, 99}}};
  require(!prepare_routing_latency(nodes, missing, {2, 0}), "missing endpoint accepted");
  const std::array<RoutingEdge, 2> cycle{{{10, 1, 2}, {11, 2, 1}}};
  require(!prepare_routing_latency(nodes, cycle, {2, 0}), "master-output cycle accepted");
  const std::array<RoutingNode, 3> cyclic_nodes{{{1, 0}, {2, 0}, {3, 0}}};
  const std::array<RoutingEdge, 3> inner_cycle{{{10, 1, 2}, {11, 2, 1}, {12, 2, 3}}};
  auto cyclic = prepare_routing_latency(cyclic_nodes, inner_cycle, {3, 0});
  REQUIRE_TRUE(!cyclic, "directed cycle was not diagnosed as a conflict");
  REQUIRE_EQ(cyclic.error().code, oscillate::foundation::ErrorCode::conflict,
             "directed cycle was not diagnosed as a conflict");
  const std::array<RoutingNode, 3> orphan{{{1, 0}, {2, 0}, {3, 0}}};
  require(!prepare_routing_latency(orphan, good, {2, 0}), "orphan node accepted");
  std::vector<RoutingNode> too_many(1025, {1, 0});
  require(!prepare_routing_latency(too_many, {}, {1, 0}), "node budget ignored");
  std::vector<RoutingEdge> too_many_edges(4097, {1, 1, 2});
  require(!prepare_routing_latency(nodes, too_many_edges, {2, 0}), "edge budget ignored");

  for (const float gain :
       {-0.01F, 4.01F, std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
    const std::array<RoutingEdge, 1> invalid_gain{{{10, 1, 2, gain}}};
    const auto rejected = prepare_routing_latency(nodes, invalid_gain, {2, 0});
    REQUIRE_TRUE(!rejected, "invalid route gain accepted");
    REQUIRE_EQ(rejected.error().code, oscillate::foundation::ErrorCode::invalid_argument,
               "invalid route gain accepted");
  }
  const std::array<RoutingEdge, 1> max_gain{{{10, 1, 2, 4.0F}}};
  const auto bounded = prepare_routing_latency(nodes, max_gain, {2, 0});
  REQUIRE_TRUE(bounded, "boundary route gain was not retained in immutable plan");
  REQUIRE_EQ(bounded.value().edges[0].gain, 4.0F,
             "boundary route gain was not retained in immutable plan");
}
} // namespace

int main() {
  try {
    parallel_impulse_alignment_and_latency_change();
    sends_sidechains_and_equal_paths();
    invalid_graphs();
    std::cout << "routing latency planner fixtures passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
