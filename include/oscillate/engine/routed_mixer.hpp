#pragma once

#include "oscillate/engine/audio_worker_pool.hpp"
#include "oscillate/engine/delay_compensation.hpp"
#include "oscillate/engine/routed_node_meters.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
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
  // A zero-latency stage on the node's output, after its fader, pan, gain
  // automation and mute, before its meter and its routes (the master
  // rebalance stage, ADR-0123). Like the processor, one mixer at a time
  // processes it; the same owner may continue across graph switches.
  std::shared_ptr<ProcessorRuntime> output_stage = {};

  RoutedNode(RoutingNode route, std::shared_ptr<ProcessorRuntime> owner,
             std::optional<RoutedTailReplacement> tail = {}, bool is_muted = false,
             StereoFrame gain = {1.0F, 1.0F}, std::shared_ptr<ProcessorRuntime> handoff_prior = {})
      : routing(route), processor(std::move(owner)), replacement(std::move(tail)), muted(is_muted),
        output_gain(gain), state_handoff_prior(std::move(handoff_prior)) {}
};

// How a live block is cut into sub-blocks for the wavefront schedule
// (ADR-0127). `adaptive` splits only while the levels after the first cost a
// measurable share of the block; `fixed` always splits into
// RoutedMixerLimits::fixed_subblock_frames (tests, benchmarks); `off` renders
// whole blocks level by level.
enum class RoutedSubblocks : std::uint8_t { adaptive, fixed, off };

// The adaptive policy (ADR-0127). Splitting is considered once the levels
// after the first (the buses and the master) cost split_enter_share of the
// block's render time, a running mean over about share_smoothing_blocks
// blocks, and dropped below split_leave_share. Then the two ways are timed
// against each other: probe_blocks blocks of the other way every
// reprobe_blocks blocks, and blocks split only while splitting renders them
// at least split_gain faster. A split block has enough parts, between two
// and max_subblocks of at least min_subblock_frames each, that the deeper
// levels' serial time left exposed is about target_exposed_share of it.
inline constexpr std::size_t min_subblock_frames = 64;
inline constexpr std::size_t max_subblocks = 8;
inline constexpr double split_enter_share = 0.10;
inline constexpr double split_leave_share = 0.05;
inline constexpr double target_exposed_share = 0.025;
inline constexpr double share_smoothing_blocks = 16.0;
inline constexpr std::uint32_t probe_blocks = 16;
inline constexpr std::uint32_t reprobe_blocks = 1024;
inline constexpr double split_gain = 0.10;

// The adaptive policy's state and decisions, one per live mixer, carried
// across live switches. Callback only.
class SubblockPolicy final {
public:
  // One block rendered: the deeper levels' serial share of its render time,
  // and its render time per frame.
  void observe(double serial_share, double ns_per_frame) noexcept;
  [[nodiscard]] bool split() const noexcept { return split_; }
  [[nodiscard]] double serial_share() const noexcept { return serial_share_; }
  // The parts a split block of `frames` takes (1 when not splitting).
  [[nodiscard]] std::size_t parts(std::size_t frames) const noexcept;

private:
  double serial_share_ = 0.0;
  bool split_ = false;
  // Render time per frame: a running mean for the way in use, and the mean
  // of the current probe of the other way.
  double current_ns_per_frame_ = 0.0;
  double probe_ns_per_frame_ = 0.0;
  std::uint32_t probing_ = 0; // probe blocks still to render
  std::uint32_t since_probe_ = 0;
  bool probed_ = false; // splitting has been timed at least once
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
  // Sub-block scheduling of live blocks with helpers (ADR-0127).
  RoutedSubblocks subblocks = RoutedSubblocks::adaptive;
  // The part length under RoutedSubblocks::fixed; the last part of a block
  // takes the remainder.
  std::size_t fixed_subblock_frames = 0;
};

// A node's own source, rendered inside the mixer's schedule (ADR-0127): the
// mixer calls `fill` for every node of a block's graph, part by part in
// order, just before mixing that part of the node. It writes frames
// [offset, offset + frames) of the node's source span (the span mix_block
// was given) and may hand voices to `workers` (run_nested). A node without a
// source returns true at once. False marks the block failed: the mixer
// finishes its schedule and returns false, and the caller silences it.
struct RoutedNodeFill {
  void* context = nullptr;
  bool (*fill)(void* context, std::size_t node, BlockContext part, std::size_t offset,
               std::size_t frames, AudioWorkerPool* workers) noexcept = nullptr;
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
  // With workers the block may also be cut into sub-blocks scheduled as a
  // wavefront (ADR-0127): level L mixes part k while level L - 1 mixes part
  // k + 1, so the master overlaps the tracks. Every node still processes its
  // frames in order, and the engine's processors render identically under
  // any split, so the output is the same. With `fill`, each node renders its
  // own source inside the schedule (sources then hold the spans `fill`
  // writes, unfilled on entry).
  [[nodiscard]] bool mix_block(std::span<const std::span<const StereoFrame>> sources,
                               std::span<StereoFrame> output, BlockContext context,
                               std::span<const EdgeGainSpan> gains = {},
                               std::span<const NodeGainSpan> node_gains = {},
                               AudioWorkerPool* workers = nullptr,
                               RoutedNodeFill fill = {}) noexcept;
  // The parts the last mix_block cut its block into (1: whole). Any thread;
  // a telemetry read.
  [[nodiscard]] std::size_t last_subblocks() const noexcept {
    return last_subblocks_.load(std::memory_order_relaxed);
  }
  // How many parts a block of `frames` would be cut into now, from the
  // limits and the measured shares (no workers: 1). Callback only.
  [[nodiscard]] std::size_t subblocks_for(std::size_t frames,
                                          const AudioWorkerPool* workers) const noexcept;
  // Callback only, at the first block of a live switch (no hold): take over
  // `prior`'s running state (delay lines, histories, gain ramps) and
  // position, prepared for by prepare_routed_mixer_live(). Never allocates.
  // False when this mixer was not prepared against `prior`.
  [[nodiscard]] bool adopt_live_state(const RoutedMixer& prior, BlockContext context) noexcept;
  // Whether this mixer was prepared to continue `prior` and has not yet.
  [[nodiscard]] bool continues(const RoutedMixer& prior) const noexcept {
    return live_from_ == &prior;
  }
  // Callback only, before the switching block's sources are filled: every
  // node of `prior` this graph drops has faded to silence (muted, its ramp
  // done), so leaving it out cannot click (ADR-0098).
  [[nodiscard]] bool ready_to_continue(const RoutedMixer& prior) const noexcept;
  // Callback only, while ready_to_continue() is false: mutes in `prior`, with
  // its usual fade, each node this graph drops. restore_dropped() unmutes
  // them again when this graph is withdrawn instead.
  void fade_out_dropped(RoutedMixer& prior) noexcept;
  void restore_dropped(RoutedMixer& prior) noexcept;
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
    // The part's first frame within the block's buffers and spans
    // (ADR-0127); `frames` and `context` are the part's.
    std::size_t offset = 0;
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
  // mix_block's schedule: `parts` parts (1: level by level), nodes filling
  // their sources through `fill` when it has one. False when a fill failed.
  [[nodiscard]] bool run_wavefront(const NodeBlock& block, std::size_t parts,
                                   AudioWorkerPool& workers, RoutedNodeFill fill) noexcept;
  // Whole blocks with helpers: one batch per dependency level.
  [[nodiscard]] bool run_levels(const NodeBlock& block, AudioWorkerPool& workers,
                                RoutedNodeFill fill) noexcept;
  // The adaptive policy (ADR-0127): a piece's cost at its level and part,
  // and the block's measured shares folded into the running mean.
  void record_cost(std::size_t level, std::size_t part, std::chrono::nanoseconds spent) noexcept;
  void update_policy(std::size_t parts, std::size_t frames,
                     std::chrono::nanoseconds spent) noexcept;
  // The same schedule on the calling thread alone: every node in
  // topological order, part by part.
  [[nodiscard]] bool run_serial(const NodeBlock& block, RoutedNodeFill fill) noexcept;
  // Part `part` of `parts` of a block.
  [[nodiscard]] NodeBlock part_of(const NodeBlock& block, std::size_t part,
                                  std::size_t parts) const noexcept;
  // One (node, part) task of the schedule.
  [[nodiscard]] bool mix_part(std::size_t index, const NodeBlock& block, std::size_t part,
                              std::size_t parts, RoutedNodeFill fill, AudioWorkerPool* workers,
                              std::span<StereoFrame> scratch) noexcept;
  // One (node, stage, part) task of a staged node: its input with stage 0,
  // its output with the last stage.
  [[nodiscard]] bool mix_stage(std::size_t index, std::size_t stage, const NodeBlock& block,
                               std::size_t part, std::size_t parts, RoutedNodeFill fill,
                               AudioWorkerPool* workers, std::span<StereoFrame> scratch) noexcept;
  // mix_node's halves around the processor: the summed input (returning how
  // many frames of drained tails it left in the lane) and the output.
  [[nodiscard]] std::size_t mix_input(std::size_t index, const NodeBlock& block,
                                      std::span<StereoFrame> scratch) noexcept;
  void mix_output(std::size_t index, const NodeBlock& block, std::span<StereoFrame> scratch,
                  std::size_t tail_count) noexcept;
  // The node's detector input for the block's frames, empty without one.
  [[nodiscard]] std::span<const StereoFrame> node_sidechain(std::size_t index,
                                                            const NodeBlock& block) const noexcept;
  // The schedule's pieces and ready queues (ADR-0127).
  struct PartPlan;
  struct ReadyQueue;
  // Whether the master pipelines its stages over this block's parts.
  [[nodiscard]] bool stages_sink(std::size_t parts) const noexcept;
  void push_ready(ReadyQueue& queue, std::uint32_t item) noexcept;
  [[nodiscard]] std::optional<std::uint32_t> pop_ready(ReadyQueue& queue,
                                                       std::size_t total) noexcept;
  [[nodiscard]] bool has_ready(std::size_t total) const noexcept;
  // One input of `piece` in part `part` finished; queues it when it was the
  // last.
  void release_piece(const PartPlan& plan, std::size_t part, std::size_t piece) noexcept;
  // One thread's share of a block: ready pieces until every piece is done.
  template <typename Job> void run_lane(Job& run, std::size_t lane) noexcept;
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
  // Per node, its output stage (RoutedNode::output_stage), or none.
  std::vector<std::shared_ptr<ProcessorRuntime>> output_stages_;
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
  // A live switch carries it over, so an edit does not restart it.
  SubblockPolicy policy_;
  // The schedule (ADR-0127), sized at preparation. A piece is a node's part
  // of a block, or one stage of the master's part.
  static constexpr std::size_t no_stage = static_cast<std::size_t>(-1);
  struct Piece {
    std::size_t node = 0;
    std::size_t stage = no_stage; // no_stage: the whole node
  };
  // One part's pieces in topological order and, per piece, how many pieces
  // of its part it waits for, which pieces wait for it, and whether it is
  // below the first level (queued ahead of the tracks). Plan 0 has whole
  // nodes; plan 1 the master's stages as pieces of their own.
  struct PartPlan {
    std::vector<Piece> pieces;
    std::vector<std::uint32_t> inputs;
    std::vector<std::size_t> dependent_offsets;
    std::vector<std::uint32_t> dependents;
    std::vector<bool> deep;
  };
  // Pieces ready to run, in the order they became ready: a slot is written
  // once a block (0 = not yet), and head and tail only grow within it.
  struct ReadyQueue {
    std::unique_ptr<std::atomic<std::uint32_t>[]> items;
    std::atomic<std::size_t> head{0};
    std::atomic<std::size_t> tail{0};
  };
  std::array<PartPlan, 2> part_plans_;
  std::size_t sink_stages_ = 1;
  // The most parts a block can have, and the lanes of scratch_.
  std::size_t max_parts_ = max_subblocks;
  std::size_t scratch_lanes_ = 1;
  // Per block: each piece's inputs still running (part-major), the ready
  // queues, the finished count, and the sleep word threads with nothing
  // ready wait on.
  std::unique_ptr<std::atomic<std::uint32_t>[]> pending_;
  ReadyQueue deep_ready_;
  ReadyQueue shallow_ready_;
  std::atomic<std::size_t> pieces_done_{0};
  std::atomic<std::uint32_t> ready_sequence_{0};
  std::atomic<std::uint32_t> sleepers_{0};
  // Per (level, part), the level's cost (ns) in the current block, for the
  // adaptive policy.
  std::unique_ptr<std::atomic<std::uint64_t>[]> level_part_ns_;
  std::atomic<std::size_t> last_subblocks_{1};
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
  // The prior's nodes this graph drops, by their index there, and those of
  // them it muted (fade_out_dropped), reserved for all of them.
  std::vector<std::size_t> live_removed_;
  std::vector<std::size_t> live_faded_;
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
