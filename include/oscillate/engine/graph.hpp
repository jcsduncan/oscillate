#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

#include <oscillate/foundation/error.hpp>

namespace oscillate::engine {

struct StereoFrame {
  float left;
  float right;
};

// Position denotes the first frame in a half-open project-sample interval.
// Transport generation strictly increases on seek/loop so source prefetch
// cannot reuse frames from an earlier traversal. Stateful processors use
// state_generation(): a planned, primed loop wrap retains their history while
// an ordinary seek starts a new processing generation. A zero processing
// generation (renders that never wrap) makes the transport generation the
// state generation.
struct BlockContext {
  std::uint64_t project_sample_position;
  std::uint64_t transport_generation;
  std::uint64_t processing_generation = 0;
  bool planned_loop_wrap = false;

  [[nodiscard]] constexpr std::uint64_t state_generation() const noexcept {
    return processing_generation == 0 ? transport_generation : processing_generation;
  }
};

static_assert(std::is_standard_layout_v<StereoFrame> && std::is_trivially_copyable_v<StereoFrame>);
static_assert(sizeof(StereoFrame) == 2 * sizeof(float));
static_assert(std::is_standard_layout_v<BlockContext> &&
              std::is_trivially_copyable_v<BlockContext>);

// A runtime is exclusively processed by the live callback. Its process implementation
// must satisfy the callback's no-allocation/no-blocking contract.
class ProcessorRuntime {
public:
  virtual ~ProcessorRuntime() = default;
  virtual void process(std::span<StereoFrame> stereo, BlockContext context) noexcept = 0;
  // Called by the callback at an accepted graph switch. Deactivation may also
  // run after the callback has joined during owner retirement. Implementations
  // must obey the same bounded, allocation-free contract as process().
  virtual void graph_activated(std::uint64_t) noexcept {}
  virtual void graph_deactivated(std::uint64_t) noexcept {}
  // Opt in only if this runtime can retain its processing state through a
  // control-only revision. The control-side check may inspect its current
  // admission state. The callback calls control_overlay_rebound() exactly at
  // the accepted boundary; wrappers must forward both methods to every stage.
  [[nodiscard]] virtual bool control_overlay_revision_independent() const noexcept { return false; }
  virtual void control_overlay_rebound(std::uint64_t, std::uint64_t) noexcept {}
  // Whether processing this runtime processes `stage`: itself, or a stage of
  // a chain. A graph must never drain a runtime it also runs.
  [[nodiscard]] virtual bool contains(const ProcessorRuntime* stage) const noexcept {
    return stage == this;
  }

  // Live handover (ADR-0049, ADR-0050). Callback side, at the switching
  // block, a processor that replaces `prior` on a node (prior may be null:
  // the node had none) continues it as far as it can: it primes its fresh
  // state from `input`, the node's processor input before `boundary`, oldest
  // first, as two halves of a ring, and, when a lookahead it held is gone,
  // writes the samples prior still held, processed onward, to `flushed` and
  // returns how many. Control side, before publication,
  // continues_latency_of says whether that keeps exact time across a
  // latency change. Must not allocate.
  [[nodiscard]] virtual bool
  continues_latency_of(const ProcessorRuntime* /*prior*/) const noexcept {
    return false;
  }
  virtual std::size_t continue_latency_of(ProcessorRuntime* /*prior*/,
                                          std::span<const StereoFrame> /*input_older*/,
                                          std::span<const StereoFrame> /*input_newer*/,
                                          BlockContext /*boundary*/,
                                          std::span<StereoFrame> /*flushed*/) noexcept {
    return 0;
  }
  // When a node loses its processor at a live switch: the latency this one
  // can hand back, and, callback side, writing the input it still held as
  // the path's next dry frames, returning how many. Must not allocate.
  [[nodiscard]] virtual std::uint32_t releasable_latency() const noexcept { return 0; }
  virtual std::size_t release_latency(std::span<StereoFrame> /*flushed*/) noexcept { return 0; }

  // A processor that is a series of stages (a chain) may let the mixer run
  // them as a pipeline over a block's parts (ADR-0127): stage s of one part
  // beside stage s + 1 of the part before, on different helpers. Running
  // process_stage() for every stage in order, part after part, is the same
  // as process() on each part. Stages own separate state, so different
  // stages may run at once; one stage's parts come in order. `detector` is
  // the node's sidechain input for the part (empty without one).
  [[nodiscard]] virtual std::size_t stage_count() const noexcept { return 1; }
  virtual void process_stage(std::size_t /*stage*/, std::span<StereoFrame> stereo,
                             std::span<const StereoFrame> /*detector*/,
                             BlockContext context) noexcept {
    process(stereo, context);
  }
};

// Optional second input for processors that explicitly support a detector or
// other sidechain. The graph prepares and aligns both input spans off callback.
class SidechainProcessorRuntime : public ProcessorRuntime {
public:
  virtual void process_with_sidechain(std::span<StereoFrame> stereo,
                                      std::span<const StereoFrame> sidechain,
                                      BlockContext context) noexcept = 0;
};

struct NodeRequest {
  std::uint64_t instance_id;
  std::uint64_t compatibility_key;
  std::function<std::unique_ptr<ProcessorRuntime>()> create_runtime;
};

enum class TransitionKind : std::uint8_t { created, reused, replaced, retired };
struct Transition {
  std::uint64_t instance_id;
  TransitionKind kind;
  // A replacement loses live DSP history. Publication applies a bounded one-block
  // output fade; this does not satisfy full tail-preservation acceptance.
  bool audible_continuity_limited;
};

class RenderPlan {
public:
  RenderPlan(const RenderPlan&) = delete;
  RenderPlan& operator=(const RenderPlan&) = delete;
  RenderPlan(RenderPlan&&) = delete;
  RenderPlan& operator=(RenderPlan&&) = delete;
  ~RenderPlan() = default;

  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
  [[nodiscard]] std::uint64_t base_revision() const noexcept { return base_revision_; }
  [[nodiscard]] std::size_t max_frames() const noexcept { return max_frames_; }
  [[nodiscard]] const std::vector<Transition>& transitions() const noexcept { return transitions_; }

private:
  struct Identity {
    std::uint64_t revision;
    std::uint64_t base_revision;
    std::size_t max_frames;
  };
  struct Node {
    std::uint64_t instance_id;
    std::uint64_t compatibility_key;
    std::shared_ptr<ProcessorRuntime> owner;
    ProcessorRuntime* runtime;
  };

  RenderPlan(const Identity& identity, std::vector<Node> nodes,
             std::vector<Transition> transitions) noexcept;
  void render(std::span<StereoFrame> stereo, BlockContext context) const noexcept;

  std::uint64_t revision_;
  std::uint64_t base_revision_;
  std::size_t max_frames_;
  std::vector<Node> nodes_;
  std::vector<Transition> transitions_;
  bool requires_fallback_;

  friend foundation::Result<std::unique_ptr<RenderPlan>>
  prepare_graph(std::uint64_t, std::size_t, std::span<const NodeRequest>, const RenderPlan*);
  friend class GraphPublisher;
};

// Control thread only. The prior plan must remain owned for the entire call.
// A matching ID/key reuses its runtime only when the new maximum block size
// does not exceed the prior plan's maximum. A changed key or larger maximum
// constructs a fresh runtime and marks an audible limitation. Failure leaves
// prior untouched.
[[nodiscard]] foundation::Result<std::unique_ptr<RenderPlan>>
prepare_graph(std::uint64_t revision, std::size_t max_frames, std::span<const NodeRequest> requests,
              const RenderPlan* prior);

enum class PublishStatus : std::uint8_t { accepted, invalid_plan, stale_revision };
struct CollectResult {
  std::size_t reclaimed = 0;
  std::uint64_t rejected_revision = 0; // zero means no stale-base rejection
};

// Single control producer and single callback consumer. Control calls publish and
// collect; only the callback calls render_block. All plans and shared_ptr owners
// are destroyed by control. Destroy this object only after the audio callback has
// been unregistered and joined, then call collect_retired once more.
class GraphPublisher {
public:
  GraphPublisher() = default;
  GraphPublisher(const GraphPublisher&) = delete;
  GraphPublisher& operator=(const GraphPublisher&) = delete;
  ~GraphPublisher();

  // Requested revisions increase. The sole exception is retry of the latest
  // stale-base rejection after collect_retired() reports it and no plan is
  // pending. Other equal or older revisions leave pending work untouched.
  [[nodiscard]] PublishStatus publish(std::unique_ptr<RenderPlan> plan);
  // Reports stale-base rejection and reclaims plans. Control calls are serialized.
  [[nodiscard]] CollectResult collect_retired();
  // Control only: pointer remains valid while this control thread prepares a
  // plan, provided it does not collect retired plans during that preparation.
  [[nodiscard]] const RenderPlan* active_plan() const noexcept;
  [[nodiscard]] std::uint64_t active_revision() const noexcept;
  [[nodiscard]] bool has_pending() const noexcept;
  // No allocation, refcount change, destruction, lock or wait. The adapter must
  // pass at most 8192 frames. A block over the active plan's maximum is silenced
  // and returns false. A span over the hard cap violates the adapter contract:
  // it returns false untouched so the callback cannot perform unbounded work;
  // the adapter must silence or discard that invalid buffer itself. A pending
  // plan too small for this block remains pending; the current graph renders
  // where possible, and a later fitting block may activate the pending plan.
  // A zero-frame call also leaves pending publication untouched.
  // Position zero is valid. A generation of zero, or an interval whose
  // exclusive end is not representable in uint64_t, is invalid: the bounded
  // output is silenced and no pending plan is activated. The caller advances
  // position/generation; this method never infers transport movement.
  [[nodiscard]] bool render_block(std::span<StereoFrame> stereo, BlockContext context) noexcept;

private:
  void destroy_owned(const RenderPlan* plan);

  // Control owns these plans. Callback only moves their raw pointers.
  std::vector<std::unique_ptr<RenderPlan>> owned_;
  std::uint64_t highest_requested_revision_{0}; // control-owned
  std::uint64_t retryable_revision_{0};         // control-owned, zero means none
  std::atomic<RenderPlan*> active_{nullptr};
  std::atomic<RenderPlan*> pending_{nullptr};
  std::atomic<RenderPlan*> retired_{nullptr};
  std::atomic<RenderPlan*> rejected_{nullptr};
  static_assert(std::atomic<RenderPlan*>::is_always_lock_free,
                "callback pointer atomics must be lock-free");
  bool fade_next_block_{false};         // callback-owned
  StereoFrame last_output_{0.0F, 0.0F}; // callback-owned
};

} // namespace oscillate::engine
