#include "oscillate/engine/routing_latency.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace oscillate::engine {
namespace {
constexpr std::size_t max_routing_nodes = 1024;
constexpr std::size_t max_routing_edges = 4096;
using Result = foundation::Result<RoutingLatencyPlan>;

[[nodiscard]] Result fail(foundation::ErrorCode code, std::string message) {
  return Result::failure({code, std::move(message)});
}
} // namespace

Result prepare_routing_latency(std::span<const RoutingNode> nodes,
                               std::span<const RoutingEdge> edges, RoutingLatencyOptions options) {
  using foundation::ErrorCode;
  if (nodes.empty() || options.master == 0) {
    return fail(ErrorCode::invalid_argument, "Routing graph needs a master node");
  }
  if (nodes.size() > max_routing_nodes || edges.size() > max_routing_edges) {
    return fail(ErrorCode::resource_exhausted, "Routing graph exceeds node or edge limit");
  }
  std::unordered_map<RoutingNodeId, std::size_t> node_index;
  node_index.reserve(nodes.size());
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    if (nodes[index].id == 0 || !node_index.emplace(nodes[index].id, index).second) {
      return fail(ErrorCode::conflict, "Routing node ID is zero or duplicated");
    }
  }
  const auto master_entry = node_index.find(options.master);
  if (master_entry == node_index.end()) {
    return fail(ErrorCode::not_found, "Routing master node is missing");
  }

  std::unordered_set<RoutingEdgeId> edge_ids;
  edge_ids.reserve(edges.size());
  std::vector<std::vector<std::size_t>> outgoing(nodes.size());
  std::vector<std::vector<std::size_t>> incoming(nodes.size());
  std::vector<std::uint32_t> indegree(nodes.size(), 0);
  for (std::size_t index = 0; index < edges.size(); ++index) {
    const auto& edge = edges[index];
    if (edge.kind != RoutingEdgeKind::main && edge.kind != RoutingEdgeKind::sidechain) {
      return fail(ErrorCode::invalid_argument,
                  "Routing edge " + std::to_string(edge.id) + " has an invalid kind");
    }
    if (!std::isfinite(edge.gain) || edge.gain < 0.0F || edge.gain > 4.0F) {
      return fail(ErrorCode::invalid_argument,
                  "Routing edge " + std::to_string(edge.id) + " gain must be finite and 0..4");
    }
    if (edge.id == 0 || !edge_ids.insert(edge.id).second) {
      return fail(ErrorCode::conflict, "Routing edge ID is zero or duplicated");
    }
    const auto source = node_index.find(edge.source);
    const auto destination = node_index.find(edge.destination);
    if (source == node_index.end() || destination == node_index.end()) {
      return fail(ErrorCode::not_found,
                  "Routing edge " + std::to_string(edge.id) + " has a missing endpoint");
    }
    outgoing[source->second].push_back(index);
    incoming[destination->second].push_back(index);
    ++indegree[destination->second];
  }
  if (!outgoing[master_entry->second].empty()) {
    return fail(ErrorCode::invalid_argument, "Routing master cannot feed another node");
  }

  using Ready = std::pair<RoutingNodeId, std::size_t>;
  std::priority_queue<Ready, std::vector<Ready>, std::greater<>> ready;
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    if (indegree[index] == 0) {
      ready.emplace(nodes[index].id, index);
    }
  }
  RoutingLatencyPlan plan;
  plan.topological_order.reserve(nodes.size());
  plan.nodes.reserve(nodes.size());
  plan.edges.reserve(edges.size());
  std::vector<std::uint64_t> output_latency(nodes.size(), 0);
  while (!ready.empty()) {
    const auto [id, index] = ready.top();
    ready.pop();
    std::uint64_t input_latency = 0;
    for (const auto edge_index : incoming[index]) {
      const auto predecessor = node_index.at(edges[edge_index].source);
      input_latency = std::max(input_latency, output_latency[predecessor]);
    }
    output_latency[index] = input_latency + nodes[index].processor_latency_frames;
    plan.topological_order.push_back(id);
    plan.nodes.push_back({id, output_latency[index]});
    for (const auto edge_index : outgoing[index]) {
      const auto next = node_index.at(edges[edge_index].destination);
      if (--indegree[next] == 0) {
        ready.emplace(nodes[next].id, next);
      }
    }
  }
  if (plan.topological_order.size() != nodes.size()) {
    return fail(ErrorCode::conflict, "Routing graph has a directed cycle");
  }

  // Require every track/bus to reach the master, so an accidental orphan does
  // not silently disappear from both playback and export.
  std::vector<bool> reaches_master(nodes.size(), false);
  reaches_master[master_entry->second] = true;
  for (auto it = plan.topological_order.rbegin(); it != plan.topological_order.rend(); ++it) {
    const auto index = node_index.at(*it);
    for (const auto edge_index : outgoing[index]) {
      reaches_master[index] =
          reaches_master[index] || reaches_master[node_index.at(edges[edge_index].destination)];
    }
    if (!reaches_master[index]) {
      return fail(ErrorCode::conflict,
                  "Routing node " + std::to_string(*it) + " cannot reach master");
    }
  }

  for (const auto& edge : edges) {
    const auto destination = node_index.at(edge.destination);
    const auto source = node_index.at(edge.source);
    const auto input_latency =
        output_latency[destination] - nodes[destination].processor_latency_frames;
    const auto required = input_latency - output_latency[source];
    if (required > options.max_compensation_frames) {
      return fail(ErrorCode::resource_exhausted,
                  "Routing edge " + std::to_string(edge.id) + " needs " + std::to_string(required) +
                      " compensation frames; limit is " +
                      std::to_string(options.max_compensation_frames));
    }
    plan.edges.push_back({edge.id, edge.source, edge.destination,
                          static_cast<std::uint32_t>(required), edge.gain, edge.kind});
  }
  plan.master_output_latency_frames = output_latency[master_entry->second];
  return Result::success(std::move(plan));
}

} // namespace oscillate::engine
