#pragma once

#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/plugins/vst3_graph_node.hpp"
#include "oscillate/plugins/vst3_scan.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/timeline/timing.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// A saved project VST3 effect as a graph node: the application binds the
// instance and turns its tempo map and automation into the plugins module's
// neutral inputs (ADR-0104).
namespace oscillate::plugins {

// Control side only. engine_instance_id is allocated by the graph composition
// owner and must be unique among all nodes; it is not a truncated project UUID.
// Failure returns no node so the caller can keep the prior graph active and
// report the binding diagnostic. A successful factory is single-use; retain
// the request for compatible graph revisions, and rebind for replacement.
[[nodiscard]] PreparedVst3GraphNode prepare_vst3_graph_node(
    const project::Vst3PluginInstance& saved, const ScanResult& scanned,
    std::uint64_t engine_instance_id, const timeline::TempoMap& tempo, double sample_rate,
    std::uint32_t max_block_frames, Vst3ProcessingMode mode = Vst3ProcessingMode::realtime,
    std::vector<automation_project::PreparedTargetAutomation> automation = {},
    std::uint64_t automation_key = 0, bool fixed_latency_verified = false,
    std::span<const std::uint32_t> qualified_manual_ids = {}, bool routed_sidechain = false,
    std::optional<std::vector<Vst3QualifiedManualBlock>> qualified_blocks = std::nullopt);

} // namespace oscillate::plugins
