#pragma once

#include "oscillate/engine/graph.hpp"
#include "oscillate/engine/routing_latency.hpp"
#include "oscillate/foundation/error.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace oscillate::engine {

// An edge's stable routing ID identifies its compensation history across plans.
// The builder supplies both the measured PDC frame budget and its memory budget;
// this primitive sets no release-wide numerical maximum.
struct DelayCompensationConfig {
  RoutingEdgeId route_id;
  std::uint32_t delay_frames;
  std::uint32_t max_compensation_frames;
  std::uint64_t max_history_bytes;
};

class DelayCompensationRuntime final : public ProcessorRuntime {
public:
  DelayCompensationRuntime(const DelayCompensationRuntime&) = delete;
  DelayCompensationRuntime& operator=(const DelayCompensationRuntime&) = delete;

  // Control side only. Storage for max_compensation_frames + 1 stereo samples
  // is allocated and zeroed before publication to the callback.
  [[nodiscard]] static foundation::Result<std::unique_ptr<DelayCompensationRuntime>>
  prepare(DelayCompensationConfig config);

  // Control side only after the prior callback block has fully quiesced. A
  // consistent boundary and enough recorded input history are prerequisites.
  // Failure leaves prior untouched; the graph owner must hold the old plan or
  // choose its visible mute/fade policy, never activate an unprimed line.
  [[nodiscard]] static foundation::Result<std::unique_ptr<DelayCompensationRuntime>>
  prepare_transition_after_quiesce(const DelayCompensationRuntime& prior,
                                   DelayCompensationConfig next, BlockContext switch_boundary);

  // Callback side, at a live graph switch (no hold): continue `prior`'s
  // recorded input, as prepare_transition_after_quiesce does, into this
  // preallocated line. History the new delay needs but `prior` lacks plays as
  // silence, as after a seek. A changed delay crossfades from the old
  // reading over crossfade_frames. Never allocates.
  static constexpr std::uint32_t crossfade_frames = 256;
  void adopt(const DelayCompensationRuntime& prior) noexcept;
  // Callback side, for a new compensated route at a live switch: record the
  // source's frames ending just before `boundary`, oldest first, given as the
  // two halves of a wrapped ring (`older` then `newer`).
  void prime(std::span<const StereoFrame> older, std::span<const StereoFrame> newer,
             BlockContext boundary) noexcept;
  // Callback side, after adopt(), when the source node's latency changed by
  // `latency_change` frames and this line's delay by its negative (ADR-0049):
  // a later source forgets its newest frames, which it will emit again; an
  // earlier one appends `appended`, the frames it had still been holding.
  void shift(std::int64_t latency_change, std::span<const StereoFrame> appended) noexcept;

  void process(std::span<StereoFrame> stereo, BlockContext context) noexcept override;

  [[nodiscard]] RoutingEdgeId route_id() const noexcept { return config_.route_id; }
  [[nodiscard]] std::uint32_t delay_frames() const noexcept { return config_.delay_frames; }
  [[nodiscard]] std::uint32_t history_frames_after_quiesce() const noexcept {
    return history_frames_;
  }
  [[nodiscard]] std::uint64_t guarded_frames_after_quiesce() const noexcept {
    return guarded_frames_;
  }
  // After the final nonzero input, exactly this many zero-input frames can
  // carry delayed samples. The caller must still drive those frames.
  [[nodiscard]] std::uint32_t tail_frames() const noexcept { return config_.delay_frames; }

private:
  DelayCompensationRuntime(DelayCompensationConfig config,
                           std::vector<StereoFrame> history) noexcept;
  void guard(std::span<StereoFrame> stereo) noexcept;

  DelayCompensationConfig config_;
  std::vector<StereoFrame> history_;
  std::size_t cursor_ = 0;           // Next slot to write.
  std::uint32_t history_frames_ = 0; // Previous frames in the current generation.
  std::uint64_t generation_ = 0;
  std::uint64_t next_position_ = 0;
  std::uint64_t guarded_frames_ = 0;
  bool has_position_ = false;
  std::uint32_t crossfade_from_ = 0;
  std::uint32_t crossfade_left_ = 0;
};

} // namespace oscillate::engine
