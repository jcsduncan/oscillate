#pragma once

#include "oscillate/engine/routed_mixer.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace oscillate::engine {

enum class RoutedRenderStatus : std::uint8_t { rendered, holding, no_graph, invalid_block };

// The callback fills already-sized source spans only after the publisher has
// decided that this block will render. A false result must leave source cursors
// unchanged; the publisher writes silence and does not publish a new graph.
struct RoutedSourceFill {
  void* context = nullptr;
  // Identifies the graph whose source buffers will be consumed. During a held
  // handoff this is the candidate revision, before it becomes active.
  bool (*fill)(void*, std::uint64_t, BlockContext, std::size_t) noexcept = nullptr;
  // A revision-aware adapter may supply a different preallocated source bank
  // for the candidate graph. The returned spans must remain valid through
  // mix_block. A false return leaves the graph and source cursor unchanged.
  // This callback runs instead of fill when both are supplied.
  bool (*fill_selected)(void*, std::uint64_t, BlockContext, std::size_t,
                        std::span<const std::span<const StereoFrame>>&) noexcept = nullptr;
};

// One published revision owns preallocated gain spans. Evaluation runs before
// source fill, so an invalid block cannot consume source frames.
struct RoutedGainSpans {
  std::span<const EdgeGainSpan> edges;
  std::span<const NodeGainSpan> nodes;
};

class RoutedGainSource {
public:
  virtual ~RoutedGainSource() = default;
  [[nodiscard]] virtual bool fill_block(BlockContext context, std::size_t frames,
                                        RoutedGainSpans& gains) noexcept = 0;
  // Conservative by default: an unknown gain source may override any channel.
  [[nodiscard]] virtual bool binds_node_gain(RoutingNodeId) const noexcept { return true; }
  [[nodiscard]] virtual bool binds_edge_gain(RoutingEdgeId) const noexcept { return true; }
};

// One serialized control producer and one callback consumer. Control owns and
// destroys all mixers. Stop/join the callback before destruction.
class RoutedMixerPublisher final {
public:
  RoutedMixerPublisher() = default;
  RoutedMixerPublisher(const RoutedMixerPublisher&) = delete;
  RoutedMixerPublisher& operator=(const RoutedMixerPublisher&) = delete;
  ~RoutedMixerPublisher(); // only after the callback has joined

  // Initial publication needs no hold. An active graph requires request_hold()
  // and a callback acknowledgement. The transport owner must keep the project
  // cursor at held_boundary() during the hold, including silent device blocks.
  [[nodiscard]] foundation::Result<void>
  prepare_and_publish(std::uint64_t revision, std::span<const RoutedNode> nodes,
                      std::span<const RoutingEdge> edges, RoutedMixerLimits limits,
                      std::shared_ptr<RoutedGainSource> gains = {});
  // A successor that takes over at the next block without a hold or a silent
  // block: prepared from the active graph's configuration while it keeps
  // rendering; the callback hands over its delay lines, histories and gain
  // ramps (RoutedMixer::adopt_live_state) before mixing the switching block.
  // Processors shared with the active graph keep running.
  [[nodiscard]] foundation::Result<void>
  prepare_and_publish_live(std::uint64_t revision, std::span<const RoutedNode> nodes,
                           std::span<const RoutingEdge> edges, RoutedMixerLimits limits,
                           std::shared_ptr<RoutedGainSource> gains = {});
  // A source-only revision keeps the exact mixer, processor and gain owners.
  // Its candidate source bank is selected by revision on the next callback.
  // If that bank cannot supply a complete block, the old source renders and
  // the candidate remains pending for a later boundary.
  [[nodiscard]] foundation::Result<void> prepare_source_overlay(std::uint64_t revision);
  // Revision-independent processors only. Controls must cover every node in the active
  // topological order; changed nodes with channel automation are rejected.
  // The callback reuses mixer/source/delay state and applies a bounded gain
  // ramp at the next valid block boundary without a transport hold.
  [[nodiscard]] foundation::Result<void>
  prepare_control_overlay(std::uint64_t revision, std::span<const RoutedNodeControl> controls);
  // Same settled-graph contract as node controls, but changes only existing
  // edge gain targets. All edge IDs must match the active plan in order.
  [[nodiscard]] foundation::Result<void>
  prepare_edge_overlay(std::uint64_t revision, std::span<const RoutedEdgeControl> controls);
  // A control mid-gesture (a fader or send being dragged) for whichever
  // graph is active, without a revision: the callback glides to it at the
  // next block. A channel or send under automation keeps its automation.
  // Control thread only; false when the mailbox is full or the gain is not
  // finite in [0, 4].
  [[nodiscard]] bool push_live_node_gain(RoutingNodeId node, StereoFrame gain) noexcept;
  [[nodiscard]] bool push_live_edge_gain(RoutingEdgeId edge, float gain) noexcept;
  // Observation and lifecycle calls below are also control-thread only and
  // must be serialized with preparation; callback readers use render_block.
  [[nodiscard]] bool request_hold() noexcept;
  [[nodiscard]] std::optional<BlockContext> held_boundary() const noexcept;
  [[nodiscard]] bool cancel_hold() noexcept;
  [[nodiscard]] std::size_t collect_retired();
  // Asks the callback to withdraw the pending candidate, for one that can
  // never be adopted (a live switch whose max_frames is below every device
  // block, or whose source bank never fills). The callback retires it at the
  // start of its next block, before touching it, unless that block already
  // adopted it; collect_retired() then frees it. Until the callback answers,
  // has_pending() stays true, so nothing new is published meanwhile. False
  // when nothing is pending or a withdrawal is already requested.
  [[nodiscard]] bool withdraw_pending() noexcept;
  // Blocks the active graph rendered while a live candidate stayed pending,
  // counted since construction. A live candidate takes over at the first
  // block it can, so each one is a block it could not: the control side
  // withdraws a candidate after a bounded number of them.
  [[nodiscard]] std::uint64_t live_candidate_passed_blocks() const noexcept {
    return live_passed_blocks_.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::uint64_t active_revision() const noexcept;
  // A candidate awaits adoption, or a withdrawal awaits the callback.
  [[nodiscard]] bool has_pending() const noexcept;
  // The callback-accepted mixer's post-fader meter for this node. Retired
  // plans are destroyed only by collect_retired(), so this control-side read
  // cannot outlive the mixer it reads.
  [[nodiscard]] std::optional<RoutedNodeMeterReading>
  active_node_meter(RoutingNodeId node) const noexcept;

  // Callback only. Sources use the active plan's topological order. This first
  // contract rejects new plans that change that order. A holding block is
  // bounded silence and does not advance the project cursor. An adapter block
  // over 8192 frames is left untouched for the caller to silence or discard.
  // Workers, when given, mix the graph's levels in parallel (ADR-0048).
  [[nodiscard]] RoutedRenderStatus
  render_block(std::span<const std::span<const StereoFrame>> sources, std::span<StereoFrame> output,
               BlockContext context) noexcept;
  [[nodiscard]] RoutedRenderStatus
  render_block(std::span<const std::span<const StereoFrame>> sources, std::span<StereoFrame> output,
               BlockContext context, RoutedSourceFill source_fill,
               AudioWorkerPool* workers = nullptr) noexcept;

private:
  struct Plan {
    std::uint64_t revision = 0;
    std::shared_ptr<RoutedMixer> mixer{};
    std::shared_ptr<RoutedGainSource> gains{};
    bool source_overlay = false;
    bool control_overlay = false;
    // Prepared while the active graph kept rendering; the callback hands its
    // running state over at the switching block (no hold).
    bool live_switch = false;
    std::uint64_t source_revision = 0;
    std::vector<RoutedNodeControl> controls{};
    std::vector<RoutedEdgeControl> edge_controls{};
  };

  [[nodiscard]] foundation::Result<void>
  publish(std::uint64_t revision, std::span<const RoutedNode> nodes,
          std::span<const RoutingEdge> edges, RoutedMixerLimits limits,
          std::shared_ptr<RoutedGainSource> gains, bool live);
  struct LiveControl {
    bool edge = false;
    std::uint64_t id = 0;
    StereoFrame gain{};
  };
  static constexpr std::size_t live_capacity = 256;

  void destroy_owned(const Plan* plan);
  void answer_withdrawal() noexcept;
  [[nodiscard]] bool push_live(LiveControl control) noexcept;
  void drain_live(const Plan& active) noexcept;
  static void silence(std::span<StereoFrame> output) noexcept;
  static bool valid_inputs(const Plan* plan, std::span<const std::span<const StereoFrame>> sources,
                           std::size_t frames) noexcept;

  std::vector<std::unique_ptr<Plan>> owned_; // control only
  std::atomic<Plan*> active_{nullptr};
  std::atomic<Plan*> pending_{nullptr};
  std::atomic<Plan*> retired_{nullptr};
  std::atomic<bool> hold_requested_{false};
  std::atomic<bool> held_{false};
  std::atomic<bool> withdraw_requested_{false};
  std::atomic<std::uint64_t> live_passed_blocks_{0}; // callback writes, control reads
  BlockContext boundary_{0, 0}; // callback writes, control reads after held_ acquire
  // Single-producer (control), single-consumer (callback) live controls.
  std::array<LiveControl, live_capacity> live_{};
  std::atomic<std::size_t> live_head_{0};
  std::atomic<std::size_t> live_tail_{0};
  static_assert(std::atomic<Plan*>::is_always_lock_free);
  static_assert(std::atomic<bool>::is_always_lock_free);
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
};

} // namespace oscillate::engine
