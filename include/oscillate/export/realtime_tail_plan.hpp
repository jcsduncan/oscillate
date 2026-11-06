#pragma once

#include "oscillate/engine/routing_latency.hpp"
#include "oscillate/foundation/error.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace oscillate::exporting {

// Control-side tail receipt for one prepared route node. The caller must sum
// every enabled stage in saved chain order, including a prepared VST3 stage's
// finite_tail_frames. Nullopt means a tail cannot be bounded and fails closed.
// A node with no active processor still needs an explicit zero-tail receipt.
struct RealtimeNodeTail {
  engine::RoutingNodeId node = 0;
  std::optional<std::uint64_t> frames;
};

// Computes the longest tail through the prepared graph, including sends and
// sidechain paths conservatively. The receipts must cover every topology node
// exactly once; no output can be opened before this control-side preflight.
// PDC and terminal output latency are handled by the routed render/capture,
// not added to this tail bound. This function never runs in the callback.
[[nodiscard]] foundation::Result<std::uint64_t>
plan_realtime_tail_frames(const engine::RoutingLatencyPlan& route, engine::RoutingNodeId master,
                          std::span<const RealtimeNodeTail> tails, std::uint64_t max_tail_frames);

} // namespace oscillate::exporting
