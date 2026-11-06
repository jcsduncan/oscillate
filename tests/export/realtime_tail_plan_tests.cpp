#include "oscillate/export/realtime_tail_plan.hpp"
#include "oscillate/test/check.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace {
using oscillate::engine::RoutingLatencyPlan;
using oscillate::engine::RoutingNodeId;
using oscillate::exporting::plan_realtime_tail_frames;
using oscillate::exporting::RealtimeNodeTail;

RoutingLatencyPlan parallel_bus_route() {
  RoutingLatencyPlan route;
  route.topological_order = {1, 2, 3, 4};
  route.nodes = {{1, 0}, {2, 128}, {3, 256}, {4, 256}};
  // Path 1 -> 2 -> master has an eight-frame VST3 tail and a 12-frame
  // downstream bus tail. Parallel path 3 has a 25-frame tail. Both are
  // lengthened by the master's seven-frame stage.
  route.edges = {{10, 1, 2, 0, 1.0F}, {11, 2, 4, 0, 1.0F}, {12, 3, 4, 0, 1.0F}};
  route.master_output_latency_frames = 256;
  return route;
}

void longest_path_includes_serial_vst3_tail_and_parallel_routes() {
  const auto route = parallel_bus_route();
  const std::vector<RealtimeNodeTail> tails{{1, 0}, {2, 20}, {3, 25}, {4, 7}};
  const auto result = plan_realtime_tail_frames(route, 4, tails, 32);
  REQUIRE_TRUE(result && result.value() == 32);
  // Output latency belongs to capture alignment, not the tail duration.
  REQUIRE_EQ(route.master_output_latency_frames, 256);
  auto changed = tails;
  changed[1].frames = 26;
  REQUIRE_TRUE(!plan_realtime_tail_frames(route, 4, changed, 32));
  changed[1].frames = 20;
  changed[2].frames = 0;
  REQUIRE_EQ(plan_realtime_tail_frames(route, 4, changed, 32).value(), 27);
}

void unknown_or_incomplete_prepared_graph_fails_closed() {
  const auto route = parallel_bus_route();
  std::vector<RealtimeNodeTail> tails{{1, 0}, {2, 20}, {3, 25}, {4, 7}};
  tails[1].frames = std::nullopt; // VST3 reports infinite/unavailable tail.
  REQUIRE_TRUE(!plan_realtime_tail_frames(route, 4, tails, 480'000));
  tails[1].frames = 20;
  tails.pop_back();
  REQUIRE_TRUE(!plan_realtime_tail_frames(route, 4, tails, 480'000));
  tails.push_back({2, 7});
  REQUIRE_TRUE(!plan_realtime_tail_frames(route, 4, tails, 480'000));
  tails.back() = {99, 7};
  REQUIRE_TRUE(!plan_realtime_tail_frames(route, 4, tails, 480'000));

  auto disconnected = route;
  disconnected.edges.pop_back();
  tails.back() = {4, 7};
  REQUIRE_TRUE(!plan_realtime_tail_frames(disconnected, 4, tails, 480'000));
  auto cyclic = route;
  cyclic.edges.push_back({13, 4, 1, 0, 1.0F});
  REQUIRE_TRUE(!plan_realtime_tail_frames(cyclic, 4, tails, 480'000));
}

void detector_paths_are_bounded_conservatively() {
  auto route = parallel_bus_route();
  route.topological_order = {1, 3, 2, 4};
  route.edges[2] = {12, 3, 2, 0, 1.0F, oscillate::engine::RoutingEdgeKind::sidechain};
  // The detector does not itself reach the audible master, but its history
  // can affect a downstream dynamics processor. Include it conservatively.
  const std::vector<RealtimeNodeTail> tails{{1, 0}, {2, 20}, {3, 25}, {4, 7}};
  const auto result = plan_realtime_tail_frames(route, 4, tails, 52);
  REQUIRE_TRUE(result && result.value() == 52);
}

void excessive_tail_and_unaligned_master_fail() {
  auto route = parallel_bus_route();
  const std::vector<RealtimeNodeTail> tails{{1, 0}, {2, 20}, {3, 25}, {4, 7}};
  REQUIRE_TRUE(!plan_realtime_tail_frames(route, 4, tails, 31));
  REQUIRE_TRUE(!plan_realtime_tail_frames(route, 2, tails, 480'000));
  route.topological_order.pop_back();
  REQUIRE_TRUE(!plan_realtime_tail_frames(route, 4, tails, 480'000));
  route = parallel_bus_route();
  route.nodes[3].id = 2;
  REQUIRE_TRUE(!plan_realtime_tail_frames(route, 4, tails, 480'000));
  const std::vector<RealtimeNodeTail> huge{
      {1, 0}, {2, std::numeric_limits<std::uint64_t>::max()}, {3, 0}, {4, 1}};
  REQUIRE_TRUE(!plan_realtime_tail_frames(parallel_bus_route(), 4, huge,
                                          std::numeric_limits<std::uint64_t>::max()));
}
} // namespace

int main() {
  longest_path_includes_serial_vst3_tail_and_parallel_routes();
  unknown_or_incomplete_prepared_graph_fails_closed();
  detector_paths_are_bounded_conservatively();
  excessive_tail_and_unaligned_master_fail();
}
