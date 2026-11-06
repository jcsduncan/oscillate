#pragma once

#include "oscillate/engine/audio_worker_pool.hpp"
#include "oscillate/engine/delay_compensation.hpp"
#include "oscillate/engine/routed_node_meters.hpp"

#include <atomic>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace oscillate::engine {

// Stereo route mixer: main edges sum into a node's audible input; sidechain
// edges sum into a separate aligned detector input for capable processors.
// Channel layouts and graph publication are not represented here. Source spans are in
// plan().topological_order.
// A node's optional processor consumes its summed input and is exclusively
// processed by one live mixer. Reuse the same shared owner for topology edits.
// A replacement is explicit: the predecessor runs on zero input for exactly
// drain_frames while the successor runs on live input. The producer must
// establish a conservative, finite tail bound and report any remaining
// audible limitation. No plugin or project setting is inferred here.
struct RoutedTailReplacement {
  std::shared_ptr<ProcessorRuntime> prior_owner;
  std::uint64_t drain_frames;
  // Optional processor owned by the predecessor chain. Only this stage
  // receives zero-input drain blocks when earlier stages are reused by the
  // successor. The prior owner remains pinned until drainage completes.
  std::shared_ptr<ProcessorRuntime> drain_processor = {};
  // The tail outlasts drain_frames (a cap, not a bound): its last
  // drain_fade_frames fade out rather than stop.
  bool fade_out = false;
};

inline constexpr std::uint64_t drain_fade_frames = 2048;

inline constexpr std::uint64_t routed_tail_frame_cap = 1'048'576;

struct RoutedNode {
  RoutingNode routing;
  std::shared_ptr<ProcessorRuntime> processor;
  std::optional<RoutedTailReplacement> replacement;
  bool muted = false;
  StereoFrame output_gain{1.0F, 1.0F};
  // An explicitly prepared successor chain may retain stages owned by this
  // quiescent predecessor. No predecessor drain may be active in this case.
  std::shared_ptr<ProcessorRuntime> state_handoff_prior = {};

  RoutedNode(RoutingNode route, std::shared_ptr<ProcessorRuntime> owner,
             std::optional<RoutedTailReplacement> tail = {}, bool is_muted = false,
             StereoFrame gain = {1.0F, 1.0F}, std::shared_ptr<ProcessorRuntime> handoff_prior = {})
      : routing(route), processor(std::move(owner)), replacement(std::move(tail)), muted(is_muted),
        output_gain(gain), state_handoff_prior(std::move(handoff_prior)) {}
};

struct RoutedMixerLimits {
  RoutingLatencyOptions latency;
  std::size_t max_frames;
  std::uint64_t max_buffer_bytes;
  // Covers every edge delay, one raw source-input delay for each sidechain
  // destination, and any opt-in pre-processor node input rings.
  std::uint64_t max_history_bytes;
  // Opt-in, separately bounded post-processor node history. With zero, a
  // newly added compensated edge cannot be primed.
  std::uint64_t max_source_history_bytes = 0;
  // A changed node gain reaches its new value over this many samples after a
  // held graph switch. Zero keeps the exact block-boundary behaviour.
  std::uint32_t gain_ramp_frames = 0;
  // Opt-in pre-processor node history for synthetic silent latency handoffs.
  // Its ring storage is charged to max_history_bytes with edge histories.
  std::uint32_t max_processor_input_history_frames = 0;
  // Opt-in post-fader (after level, pan, mute and inserts) peak/RMS slots,
  // one per node, allocated with the mixer. Live playback reads them.
  bool node_meters = false;
  // Opt-in for renders whose mutes cannot change (export): a muted node that
  // keys nothing and has finished its mute ramp skips its inputs and processor
  // and outputs silence, exactly what its mute would have left.
  bool skip_muted_nodes = false;
};

// Optional per-sample edge gain, in plan().edges order. An empty values span
// uses the saved static edge gain. Callers bind IDs on the control thread and
// supply only finite physical gains in [0,4].
struct EdgeGainSpan {
  RoutingEdgeId id;
  std::span<const float> values;
};

// Absolute per-sample post-insert gain, in plan().nodes order. Empty values
// retain the saved static node gain. Prepared off the callback.
struct NodeGainSpan {
  RoutingNodeId id;
  std::span<const StereoFrame> values;
};

// Complete, topological-order control projection for a control-only graph
// revision. Muting is applied after the processor, as in an ordinary route.
struct RoutedNodeControl {
  RoutingNodeId id;
  bool muted;
  StereoFrame output_gain;
};

// Complete gain projection for existing edges in the active plan's order.
// Edge identity, endpoints, kind and compensation remain unchanged.
struct RoutedEdgeControl {
  RoutingEdgeId id;
  float gain;
};

// Tasks a caller runs in the same parallel batch as an offline pipeline
// step's nodes (RoutedMixer::mix_step). They must not touch the mixer.
struct RoutedStepTasks {
  std::size_t count = 0;
  AudioWorkerPool::Task task = nullptr;
  void* context = nullptr;
};

class RoutedMixer final {
public:
  RoutedMixer(const RoutedMixer&) = delete;
  RoutedMixer& operator=(const RoutedMixer&) = delete;

  [[nodiscard]] const RoutingLatencyPlan& plan() const noexcept { return plan_; }
  [[nodiscard]] std::size_t max_frames() const noexcept { return limits_.max_frames; }
  // Callback-side publication hooks. Retired tail processors may still drain,
  // but only current audible processors receive activation.
  void graph_activated(std::uint64_t revision) noexcept;
  void graph_deactivated(std::uint64_t revision) noexcept;
  void control_overlay_rebound(std::uint64_t from, std::uint64_t to) noexcept;

  // Callback-side preflight before a source-fill cursor is advanced. A block
  // at an unexpected position or from an older generation cannot render.
  [[nodiscard]] bool accepts_context(BlockContext context, std::size_t frames) const noexcept;

  // Callback only. Empty source spans mean silence; nonempty spans must match
  // output size. A rejected block at or below the hard 8192-frame adapter cap
  // is silenced without advancing route history. A larger span is untouched.
  // With workers, the nodes of each dependency level run in parallel; every
  // node sums its inputs in the same order, so the output is bit-identical.
  [[nodiscard]] bool mix_block(std::span<const std::span<const StereoFrame>> sources,
                               std::span<StereoFrame> output, BlockContext context,
                               std::span<const EdgeGainSpan> gains = {},
                               std::span<const NodeGainSpan> node_gains = {},
                               AudioWorkerPool* workers = nullptr) noexcept;
  // Callback only, at the first block of a live switch (no hold): take over
  // `prior`'s running state (delay lines, histories, gain ramps) and
  // position, prepared for by prepare_routed_mixer_live(). Never allocates.
  // False when this mixer was not prepared against `prior`.
  [[nodiscard]] bool adopt_live_state(const RoutedMixer& prior, BlockContext context) noexcept;
  // Whether this mixer was prepared to continue `prior` and has not yet.
  [[nodiscard]] bool continues(const RoutedMixer& prior) const noexcept {
    return live_from_ == &prior;
  }
  // Only explicitly qualified owners, or empty nodes, may keep processing.
  [[nodiscard]] bool control_overlay_safe() const noexcept;
  // Callback only, after source preflight and before the next mix_block.
  void apply_node_controls(std::span<const RoutedNodeControl> controls) noexcept;
  void apply_edge_controls(std::span<const RoutedEdgeControl> controls) noexcept;
  // Callback only: a control mid-gesture (a fader or send knob being
  // dragged), without a revision. The node or edge glides to the gain over
  // the usual ramp. False when it is not in this graph.
  bool apply_live_node_gain(RoutingNodeId node, StereoFrame gain) noexcept;
  bool apply_live_edge_gain(RoutingEdgeId edge, float gain) noexcept;
  // Control only, while a plan owning this mixer is retained. Empty without
  // limits.node_meters, for an unknown node, or before the first window.
  [[nodiscard]] std::optional<RoutedNodeMeterReading> node_meter(RoutingNodeId node) const noexcept;
  // Offline renders only, on the thread that called mix_block, after it
  // returned (never on the live callback path): the first `frames` of node
  // `index`'s (plan().topological_order) output from that block. It is the
  // block the node fed its routes with: post-fader, after its summed inputs,
  // its processor and drains, its level, pan and gain automation and its
  // mute. Empty for an unknown index or more frames than a block holds.
  [[nodiscard]] std::span<const StereoFrame> node_output(std::size_t index,
                                                         std::size_t frames) const noexcept;

  // Offline renders only (export), control side, once before the first
  // mix_step: lets every node of the graph run at once, each dependency level
  // one block behind the level before it, so a bus or the master path no
  // longer waits on the tracks feeding it. Returns the pipeline's depth: a
  // block fed to mix_step leaves it depth - 1 steps later. Empty (refused)
  // with tail drains or node histories, which only live playback uses.
  [[nodiscard]] std::optional<std::size_t> prepare_pipeline();
  // Offline only, after prepare_pipeline(). Feeds one block, validated as
  // mix_block validates it, and writes the master output of the block fed
  // depth - 1 steps earlier to `output` (at least max_frames() long),
  // returning its frame count: zero while the pipeline fills, empty when the
  // block is refused. Every node sums the same inputs in the same order as in
  // mix_block, so the emitted blocks are bit-identical to mixing block by
  // block; only their arrival is delayed. A caller drains the last blocks by
  // feeding silent ones (any length up to max_frames()).
  [[nodiscard]] std::optional<std::size_t>
  mix_step(std::span<const std::span<const StereoFrame>> sources, std::size_t frames,
           BlockContext context, std::span<const EdgeGainSpan> gains,
           std::span<const NodeGainSpan> node_gains, std::span<StereoFrame> output,
           AudioWorkerPool* workers, RoutedStepTasks extra = {}) noexcept;

private:
  struct EdgeRuntime {
    std::size_t source_index;
    std::size_t destination_index;
    float gain;
    RoutingEdgeKind kind;
    std::unique_ptr<DelayCompensationRuntime> delay;
    float ramp_from = 1.0F;
    std::uint32_t ramp_elapsed = 0;
    std::uint32_t ramp_total = 0;
  };
  struct TailDrain {
    TailDrain(std::shared_ptr<ProcessorRuntime> owner, std::shared_ptr<ProcessorRuntime> drain,
              std::uint64_t frames, std::uint64_t generation, bool fade) noexcept
        : prior_owner(std::move(owner)), drain_processor(std::move(drain)),
          remaining_frames(frames), processing_generation(generation), fade_out(fade) {}
    std::shared_ptr<ProcessorRuntime> prior_owner;
    std::shared_ptr<ProcessorRuntime> drain_processor;
    // Callback-written; a live switch reads it from the control side.
    std::atomic<std::uint64_t> remaining_frames;
    std::uint64_t processing_generation;
    bool fade_out;
  };
  // A node's predecessors still ringing out on zero input, oldest first. Each
  // runs to the end of its bounded tail; a later edit adds another.
  using TailDrains = std::vector<std::shared_ptr<TailDrain>>;
  static constexpr std::size_t max_tail_drains_per_node = 8;
  struct NodeFrameHistory {
    std::vector<StereoFrame> frames;
    std::size_t cursor = 0;
    std::size_t recorded = 0;
    std::uint64_t generation = 0;
    std::uint64_t next_position = 0;
  };
  struct GainRamp {
    StereoFrame from{1.0F, 1.0F};
    std::uint32_t elapsed = 0;
    std::uint32_t total = 0;
  };
  struct NodeBlock {
    std::span<const std::span<const StereoFrame>> sources;
    std::size_t frames;
    BlockContext context;
    std::span<const EdgeGainSpan> gains;
    std::span<const NodeGainSpan> node_gains;
    // Which of the pipeline's block slots this block's node buffers use
    // (always 0 outside mix_step).
    std::size_t slot = 0;
  };
  // One fed block held for the levels still to process it.
  struct StepSlot {
    std::vector<std::vector<StereoFrame>> sources;
    std::vector<std::span<const StereoFrame>> views;
    std::vector<std::vector<float>> gains;
    std::vector<EdgeGainSpan> gain_spans;
    std::vector<std::vector<StereoFrame>> node_gains;
    std::vector<NodeGainSpan> node_gain_spans;
    bool has_gains = false;
    bool has_node_gains = false;
    std::size_t frames = 0;
    BlockContext context{0, 0};
  };
  // Shared preflight of mix_block and mix_step.
  [[nodiscard]] bool admits_block(std::span<const std::span<const StereoFrame>> sources,
                                  std::size_t frames, BlockContext context,
                                  std::span<const EdgeGainSpan> gains,
                                  std::span<const NodeGainSpan> node_gains) const noexcept;
  // A node's output and pre-fader buffers for a pipeline slot.
  [[nodiscard]] std::vector<StereoFrame>& node_buffer(std::size_t index,
                                                      std::size_t slot) noexcept {
    return slot == 0 ? buffers_[index] : slot_buffers_[(slot - 1) * buffers_.size() + index];
  }
  [[nodiscard]] std::vector<StereoFrame>& prefader_buffer(std::size_t index,
                                                          std::size_t slot) noexcept {
    return slot == 0 ? prefader_buffers_[index]
                     : slot_prefader_buffers_[(slot - 1) * buffers_.size() + index];
  }

  // One node's block. Writes only that node's buffers, incoming edges,
  // histories, processors, ramps and meter slot, and the given scratch lane.
  void mix_node(std::size_t index, const NodeBlock& block, std::span<StereoFrame> scratch) noexcept;
  // Control side, at preparation: dependency levels, scratch lanes and
  // pre-fader taps for sidechain sources.
  void build_levels();
  void ramp_node(std::size_t index, bool muted, StereoFrame gain) noexcept;
  void ramp_edge(std::size_t index, float gain) noexcept;

  RoutedMixer(RoutingLatencyPlan plan, RoutedMixerLimits limits,
              std::vector<std::shared_ptr<ProcessorRuntime>> processors,
              std::vector<std::uint32_t> processor_latencies, std::vector<TailDrains> tails,
              std::vector<bool> muted, std::vector<StereoFrame> output_gains,
              std::vector<GainRamp> gain_ramps, std::vector<std::vector<StereoFrame>> buffers,
              std::vector<std::unique_ptr<DelayCompensationRuntime>> source_input_delays,
              std::vector<std::vector<StereoFrame>> sidechain_buffers,
              std::vector<SidechainProcessorRuntime*> sidechain_processors,
              std::vector<NodeFrameHistory> source_histories,
              std::vector<NodeFrameHistory> processor_input_histories,
              std::vector<StereoFrame> scratch, std::vector<EdgeRuntime> edges,
              std::vector<std::vector<std::size_t>> incoming) noexcept;

  RoutingLatencyPlan plan_;
  RoutedMixerLimits limits_;
  std::vector<std::shared_ptr<ProcessorRuntime>> processors_;
  std::vector<std::uint32_t> processor_latencies_;
  std::vector<TailDrains> tails_;
  std::vector<bool> muted_;
  std::vector<StereoFrame> output_gains_;
  std::vector<GainRamp> gain_ramps_;
  std::vector<std::vector<StereoFrame>> buffers_;
  std::vector<std::unique_ptr<DelayCompensationRuntime>> source_input_delays_;
  std::vector<std::vector<StereoFrame>> sidechain_buffers_;
  std::vector<SidechainProcessorRuntime*> sidechain_processors_;
  std::vector<NodeFrameHistory> source_histories_;
  std::vector<NodeFrameHistory> processor_input_histories_;
  std::vector<StereoFrame> scratch_;
  std::vector<EdgeRuntime> edges_;
  std::vector<std::vector<std::size_t>> incoming_;
  // Node indices grouped by dependency level; level L is
  // level_nodes_[level_offsets_[L], level_offsets_[L + 1]).
  std::vector<std::size_t> level_nodes_;
  // Per node: its block after inserts and before fader and mute, kept only
  // for nodes that key a sidechain.
  std::vector<std::vector<StereoFrame>> prefader_buffers_;
  std::vector<std::size_t> level_offsets_;
  std::unique_ptr<RoutedNodeMeters> node_meters_;
  // The offline pipeline (prepare_pipeline): one slot per dependency level;
  // slot 0 is the ordinary buffers, slot k > 0 the k-th extra set.
  std::vector<std::size_t> node_level_;
  std::vector<StepSlot> step_slots_;
  std::vector<std::vector<StereoFrame>> slot_buffers_;
  std::vector<std::vector<StereoFrame>> slot_prefader_buffers_;
  std::uint64_t steps_ = 0;
  std::uint64_t generation_ = 0;
  std::uint64_t next_position_ = 0;
  // A live switch: the mixer this one continues, and each node's and edge's
  // index in it (no_prior for new ones).
  static constexpr std::size_t no_prior = static_cast<std::size_t>(-1);
  const RoutedMixer* live_from_ = nullptr;
  std::vector<std::size_t> live_nodes_;
  std::vector<std::size_t> live_edges_;
  // Per node, its latency change carried exactly across the switch.
  std::vector<std::int64_t> live_shifts_;
  // Per node, whether its changed processor continues (or releases) its
  // predecessor at the switch.
  std::vector<bool> live_continues_;
  std::vector<StereoFrame> live_flush_;
  void plan_latency_handover(const RoutedMixer& prior);

  [[nodiscard]] static foundation::Result<std::unique_ptr<RoutedMixer>>
  prepare_impl(std::span<const RoutedNode> nodes, std::span<const RoutingEdge> edges,
               RoutedMixerLimits limits, const RoutedMixer* prior, BlockContext switch_boundary,
               bool live);

  friend foundation::Result<std::unique_ptr<RoutedMixer>>
  prepare_routed_mixer(std::span<const RoutedNode>, std::span<const RoutingEdge>, RoutedMixerLimits,
                       const RoutedMixer*, BlockContext);
  friend foundation::Result<std::unique_ptr<RoutedMixer>>
  prepare_routed_mixer_live(std::span<const RoutedNode>, std::span<const RoutingEdge>,
                            RoutedMixerLimits, const RoutedMixer&);
};

// Control side only, after the prior mixer has quiesced at switch_boundary.
// The caller must hold exclusive access to prior until preparation finishes;
// matching counters do not make concurrent history reads safe.
// Passing prior=nullptr prepares an initially silent graph. Changed/new
// nonzero delays without sufficient history fail, leaving prior intact. New
// compensated edges can use opt-in source history at an exact held boundary;
// a zero-delay edge needs no source history.
[[nodiscard]] foundation::Result<std::unique_ptr<RoutedMixer>>
prepare_routed_mixer(std::span<const RoutedNode> nodes, std::span<const RoutingEdge> edges,
                     RoutedMixerLimits limits, const RoutedMixer* prior = nullptr,
                     BlockContext switch_boundary = {});

// Control side, while `prior` keeps rendering: a successor prepared from
// prior's configuration only. The callback must call adopt_live_state() with
// prior at the switching block before mixing it. A synthetic silent latency
// handoff still needs the held path above.
[[nodiscard]] foundation::Result<std::unique_ptr<RoutedMixer>>
prepare_routed_mixer_live(std::span<const RoutedNode> nodes, std::span<const RoutingEdge> edges,
                          RoutedMixerLimits limits, const RoutedMixer& prior);

} // namespace oscillate::engine
