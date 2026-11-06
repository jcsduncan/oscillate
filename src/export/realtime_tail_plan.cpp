#include "oscillate/export/realtime_tail_plan.hpp"

#include <algorithm>
#include <map>

namespace oscillate::exporting {

foundation::Result<std::uint64_t> plan_realtime_tail_frames(const engine::RoutingLatencyPlan& route,
                                                            engine::RoutingNodeId master,
                                                            std::span<const RealtimeNodeTail> tails,
                                                            std::uint64_t max_tail_frames) {
  using foundation::ErrorCode;
  using Result = foundation::Result<std::uint64_t>;
  if (master == 0 || route.topological_order.empty() ||
      route.topological_order.size() != route.nodes.size() || tails.size() != route.nodes.size())
    return Result::failure({ErrorCode::invalid_argument, "Real-time tail graph is incomplete"});

  std::map<engine::RoutingNodeId, std::size_t> order;
  for (std::size_t index = 0; index < route.topological_order.size(); ++index) {
    const auto id = route.topological_order[index];
    if (id == 0 || !order.emplace(id, index).second)
      return Result::failure({ErrorCode::corrupt_data, "Real-time tail graph has duplicate nodes"});
  }
  if (!order.contains(master) || route.topological_order.back() != master)
    return Result::failure({ErrorCode::corrupt_data, "Real-time tail master is not terminal"});
  std::map<engine::RoutingNodeId, bool> listed_nodes;
  for (const auto& node : route.nodes)
    if (!order.contains(node.id) || !listed_nodes.emplace(node.id, true).second)
      return Result::failure({ErrorCode::corrupt_data, "Real-time tail graph node is absent"});

  std::map<engine::RoutingNodeId, std::uint64_t> own;
  for (const auto& tail : tails) {
    if (!order.contains(tail.node) || !own.emplace(tail.node, 0).second)
      return Result::failure({ErrorCode::corrupt_data, "Real-time tail receipt is ambiguous"});
    if (!tail.frames)
      return Result::failure(
          {ErrorCode::unsupported, "Real-time export has an unknown processor tail"});
    if (*tail.frames > max_tail_frames)
      return Result::failure(
          {ErrorCode::resource_exhausted, "Real-time export tail exceeds its limit"});
    own[tail.node] = *tail.frames;
  }
  for (const auto& edge : route.edges)
    if (!order.contains(edge.source) || !order.contains(edge.destination) ||
        order.at(edge.source) >= order.at(edge.destination))
      return Result::failure({ErrorCode::corrupt_data, "Real-time tail route is not acyclic"});

  // A forged or stale route must not let a processor ring out on an omitted
  // branch while the planner reports only the master branch's extent.
  std::map<engine::RoutingNodeId, bool> reaches_master;
  reaches_master[master] = true;
  for (auto node = route.topological_order.rbegin(); node != route.topological_order.rend();
       ++node) {
    for (const auto& edge : route.edges)
      if (edge.source == *node && reaches_master[edge.destination])
        reaches_master[*node] = true;
    if (!reaches_master[*node])
      return Result::failure(
          {ErrorCode::corrupt_data, "Real-time tail route does not reach master"});
  }

  std::map<engine::RoutingNodeId, std::uint64_t> extent;
  for (const auto node : route.topological_order) {
    const auto incoming = extent[node];
    const auto stage = own.at(node);
    if (stage > max_tail_frames - incoming)
      return Result::failure(
          {ErrorCode::resource_exhausted, "Real-time export tail exceeds its limit"});
    const auto output = incoming + stage;
    extent[node] = output;
    for (const auto& edge : route.edges)
      if (edge.source == node)
        extent[edge.destination] = std::max(extent[edge.destination], output);
  }
  return Result::success(extent.at(master));
}

} // namespace oscillate::exporting
