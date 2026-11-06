#pragma once

#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/engine/routed_publisher.hpp"
#include "oscillate/routing/project_route_plan.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace oscillate::routing {

// Control-side binding of a saved revision to stable routed send edges and
// channel nodes. The callback evaluates immutable curves into preallocated
// block storage shared by live playback and offline export.
class PreparedRouteGainBlocks final : public engine::RoutedGainSource {
public:
  // The largest block the adapter evaluates (the engine's hard adapter cap).
  static constexpr std::size_t max_block_frames = 8192;
  // Bytes of per-block scratch all bindings may hold together.
  static constexpr std::size_t storage_budget_bytes = std::size_t{16} * 1024 * 1024;

  [[nodiscard]] static foundation::Result<std::shared_ptr<PreparedRouteGainBlocks>>
  prepare(const project::Snapshot& snapshot, const ProjectRoutePlan& route,
          std::span<const automation_project::PreparedTargetAutomation> targets,
          std::size_t max_frames);

  [[nodiscard]] bool fill_block(engine::BlockContext context, std::size_t frames,
                                engine::RoutedGainSpans& gains) noexcept override;
  [[nodiscard]] bool binds_node_gain(engine::RoutingNodeId node) const noexcept override;
  [[nodiscard]] bool binds_edge_gain(engine::RoutingEdgeId edge) const noexcept override;

private:
  struct EdgeBinding {
    std::size_t target_index;
    std::size_t edge_index;
    std::vector<double> values;
    std::vector<float> samples;
  };
  // One channel node's automated controls; an unautomated control keeps its
  // saved value. Mute steps glide over a short ramp so a toggle never clicks.
  struct NodeBinding {
    std::size_t node_index;
    std::optional<std::size_t> level_target;
    std::optional<std::size_t> pan_target;
    std::optional<std::size_t> mute_target;
    double level = 1.0;
    double pan = 0.0;
    std::vector<double> level_values;
    std::vector<double> pan_values;
    std::vector<double> mute_values;
    std::vector<engine::StereoFrame> samples;
    double mute_gain = 1.0;
    std::uint64_t mute_generation = 0;
  };

  std::uint32_t sample_rate_ = 0;
  std::shared_ptr<const timeline::TempoMap> tempo_;
  std::vector<automation_project::PreparedTargetAutomation> targets_;
  std::vector<engine::EdgeGainSpan> edge_spans_;
  std::vector<engine::NodeGainSpan> node_spans_;
  std::vector<timeline::TickPosition> ticks_;
  std::vector<EdgeBinding> edge_bindings_;
  std::vector<NodeBinding> node_bindings_;
};

} // namespace oscillate::routing
