#pragma once

#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/engine/graph.hpp"
#include "oscillate/plugins/project_binding.hpp"
#include "oscillate/timeline/timing.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace oscillate::plugins {

enum class Vst3GraphRuntimeStatus : std::uint8_t { ready, process_failed };

enum class Vst3ManualEventStatus : std::uint8_t {
  accepted,
  busy,
  stale_instance,
  invalid_revision,
  invalid_value,
  automated,
  unavailable,
  latency_unverified
};
enum class Vst3ManualEventPhase : std::uint8_t { empty, pending, processing, applied, failed };
struct Vst3ManualEventReceipt {
  std::uint64_t revision{0};
  Vst3ManualEventPhase phase{Vst3ManualEventPhase::empty};
  std::int64_t project_sample{-1};
};
struct Vst3ActiveGraphIdentity {
  std::uint64_t graph_revision{0};
  std::uint64_t instance_id{0};
  std::uint64_t compatibility_key{0};
};

// Session-local evidence for one actual VST3 process-call size. The provider
// intersects these IDs with the loaded instance's eligible controls.
struct Vst3QualifiedManualBlock {
  std::uint32_t block_frames{0};
  std::vector<std::uint32_t> parameter_ids;
};

// One producer (control) and one consumer (audio). A completed receipt must
// be collected before another event is submitted. No callback allocation or
// lock; admission does metadata checks off the callback.
class Vst3ManualEventSlot final {
public:
  Vst3ManualEventSlot(
      std::uint64_t instance_id, std::uint64_t compatibility_key,
      std::vector<std::uint32_t> eligible_ids, std::vector<std::uint32_t> automated_ids,
      bool fixed_latency_verified,
      std::optional<std::vector<Vst3QualifiedManualBlock>> qualified_blocks = std::nullopt);
  [[nodiscard]] Vst3ActiveGraphIdentity active_graph() const noexcept;
  // Control-side preflight; submit repeats every check after the project commit.
  [[nodiscard]] Vst3ManualEventStatus admission(std::uint64_t graph_revision,
                                                std::uint64_t instance_id,
                                                std::uint64_t compatibility_key,
                                                std::uint64_t revision,
                                                Vst3ParameterEvent event) const noexcept;
  [[nodiscard]] Vst3ManualEventStatus submit(std::uint64_t graph_revision,
                                             std::uint64_t instance_id,
                                             std::uint64_t compatibility_key,
                                             std::uint64_t revision, Vst3ParameterEvent event,
                                             std::uint64_t gesture_sequence = 0) noexcept;
  // A transient native-editor drag value. Latest value wins within each audio
  // block; this never publishes a project revision or a committed receipt.
  // A new drag may stage while an earlier commit awaits its receipt. Give
  // native gestures an instance-wide nonzero sequence; an older completion
  // renders first and cannot erase a later held drag. Zero stages an
  // untagged preview. A retired instance always discards previews.
  [[nodiscard]] Vst3ManualEventStatus
  preview(std::uint64_t graph_revision, std::uint64_t instance_id, std::uint64_t compatibility_key,
          Vst3ParameterEvent event, std::uint64_t gesture_sequence = 0) noexcept;
  [[nodiscard]] Vst3ManualEventReceipt receipt() const noexcept;
  [[nodiscard]] std::optional<Vst3ManualEventReceipt> collect() noexcept;
  [[nodiscard]] std::optional<Vst3ParameterEvent> claim() noexcept;
  [[nodiscard]] std::optional<Vst3ParameterEvent> claim_preview() noexcept;
  // Audio consumer only: one bounded, read-only lookup before a parameter is
  // passed to the host. nullopt evidence is reserved for explicit fixtures.
  [[nodiscard]] bool qualified_for_block(std::uint32_t parameter_id,
                                         std::size_t block_frames) const noexcept;
  void finish(bool applied, std::int64_t project_sample) noexcept;
  void activate(std::uint64_t graph_revision) noexcept;
  // A control-only publication keeps this host and its DSP state. It may
  // rebind only a quiescent committed slot; a tagged later gesture may carry
  // across this exact-host control overlay, while untagged previews are
  // discarded. A producer racing a replacement receives stale-instance.
  [[nodiscard]] bool ready_for_control_overlay() const noexcept;
  void control_overlay_rebound(std::uint64_t from, std::uint64_t to) noexcept;
  void deactivate() noexcept;
  void retire() noexcept;

private:
  std::uint64_t instance_id_;
  std::uint64_t compatibility_key_;
  std::vector<std::uint32_t> eligible_ids_;
  std::vector<std::uint32_t> automated_ids_;
  bool fixed_latency_verified_;
  std::optional<std::vector<Vst3QualifiedManualBlock>> qualified_blocks_;
  Vst3ParameterEvent event_{};
  std::uint64_t revision_{0};
  std::uint64_t last_submitted_revision_{0}; // Serialized control producer only.
  std::atomic<std::uint64_t> submitted_manual_revision_{0};
  std::atomic<std::int64_t> applied_sample_{-1};
  std::atomic<std::uint64_t> active_graph_revision_{0};
  std::atomic<std::uint64_t> first_reusable_graph_revision_{0};
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                "graph identity checks on the callback must be lock-free");
  std::uint64_t event_graph_revision_{0};
  std::atomic<Vst3ManualEventPhase> phase_{Vst3ManualEventPhase::empty};
  // A single atomic word keeps parameter ID and a normalized 32-bit value
  // coherent. The version is a one-producer publication/one-consumer receipt.
  // Zero means no preview. No callback-side retry or allocation is needed.
  std::atomic<std::uint64_t> preview_word_{0};
  std::atomic<std::uint64_t> preview_graph_revision_{0};
  std::atomic<std::uint64_t> preview_version_{0};
  std::uint64_t next_preview_version_{0};     // Serialized control producer only.
  std::uint64_t consumed_preview_version_{0}; // Audio consumer only.
  std::uint64_t consumed_manual_revision_{0}; // Audio consumer only.
  std::uint32_t preview_parameter_id_{0};     // Serialized control producer only.
  std::atomic<std::uint64_t> preview_gesture_sequence_{0};
  std::uint64_t last_submitted_gesture_sequence_{0}; // Serialized control producer only.
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
};

// A control-side receipt. Keep the receipt and reuse its request for topology-only
// revisions; its status then continues to describe the same live instance.
struct PreparedVst3GraphNode {
  Vst3BindingStatus binding_status{Vst3BindingStatus::scan_unavailable};
  std::vector<std::uint32_t> unresolved_parameter_ids;
  std::string diagnostic;
  std::uint32_t latency_samples{0};
  std::optional<std::uint32_t> finite_tail_samples;
  // Proven by the loaded, activated host, not by saved intent or scan metadata.
  bool routed_sidechain{false};
  engine::NodeRequest request{};
  std::shared_ptr<std::atomic<Vst3GraphRuntimeStatus>> runtime_status;
  std::shared_ptr<Vst3ManualEventSlot> manual_events;

  [[nodiscard]] bool ready() const noexcept {
    return binding_status == Vst3BindingStatus::ready && request.instance_id != 0;
  }
};

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
