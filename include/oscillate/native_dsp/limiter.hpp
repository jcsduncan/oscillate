#pragma once

#include "oscillate/engine/graph.hpp"
#include "oscillate/foundation/error.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace oscillate::native_dsp {

struct LimiterSettings {
  double ceiling_db = -1.0;
  double lookahead_ms = 5.0;
  double release_ms = 100.0;
};

class PreparedLimiter {
public:
  [[nodiscard]] LimiterSettings settings() const noexcept { return settings_; }
  [[nodiscard]] std::uint32_t sample_rate() const noexcept { return sample_rate_; }
  [[nodiscard]] std::uint32_t latency_frames() const noexcept { return latency_frames_; }

private:
  struct Metadata {
    std::uint32_t sample_rate;
    std::uint32_t latency_frames;
    double ceiling;
    double release;
  };
  PreparedLimiter(LimiterSettings settings, Metadata metadata) noexcept
      : settings_(settings), sample_rate_(metadata.sample_rate),
        latency_frames_(metadata.latency_frames), ceiling_(metadata.ceiling),
        release_(metadata.release) {}

  LimiterSettings settings_;
  std::uint32_t sample_rate_;
  std::uint32_t latency_frames_;
  double ceiling_;
  double release_;

  friend foundation::Result<PreparedLimiter> prepare_limiter(LimiterSettings, std::uint32_t);
  friend class LimiterRuntime;
};

// Linked stereo, digital sample-peak ceiling. The reported latency is the
// lookahead delay; true/intersample peak limiting is separate work. A bounded
// monotonic peak queue covers the delayed sample through the current input.
[[nodiscard]] foundation::Result<PreparedLimiter> prepare_limiter(LimiterSettings settings,
                                                                  std::uint32_t sample_rate);

class LimiterRuntime final : public engine::ProcessorRuntime {
public:
  [[nodiscard]] bool control_overlay_revision_independent() const noexcept override { return true; }
  explicit LimiterRuntime(PreparedLimiter prepared);

  void process(std::span<engine::StereoFrame> stereo,
               engine::BlockContext context) noexcept override;

  // One serialized control producer offers settings prepared off the callback.
  // Only equal-rate, equal-lookahead edits can preserve the delay and peak history.
  // The callback adopts the newest offer at its next block boundary.
  [[nodiscard]] bool offer_prepared(PreparedLimiter prepared, std::uint64_t revision) noexcept;
  [[nodiscard]] std::uint64_t applied_prepared_revision() const noexcept {
    return applied_revision_.load(std::memory_order_acquire);
  }

  // Greatest attenuation actually applied to a non-silent delayed frame in
  // the latest block, as a non-negative dB magnitude. A silent, empty or
  // guarded block and reset_after_quiesce() publish zero. Control/UI readers
  // may sample this atomically without touching the callback-owned state.
  [[nodiscard]] float latest_gain_reduction_db() const noexcept {
    return published_reduction_db_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::uint32_t latency_frames() const noexcept { return latency_frames_; }
  // The delayed frames must be rendered with silence after source end.
  [[nodiscard]] std::uint32_t tail_frames() const noexcept { return latency_frames(); }
  [[nodiscard]] std::uint64_t guarded_frames_after_quiesce() const noexcept {
    return guarded_frames_;
  }
  void reset_after_quiesce() noexcept;

private:
  struct PendingSettings {
    PreparedLimiter prepared;
    std::uint64_t revision;
  };
  static constexpr std::size_t mailbox_capacity = 4;
  void adopt_pending() noexcept;

  struct Peak {
    double amplitude = 0.0;
    std::uint64_t index = 0;
  };
  void reset_state() noexcept;
  void push_peak(double amplitude) noexcept;

  PreparedLimiter prepared_;
  const std::uint32_t sample_rate_;
  const std::uint32_t latency_frames_;
  std::array<std::optional<PendingSettings>, mailbox_capacity> pending_{};
  std::atomic<std::uint64_t> pending_write_{0};
  std::atomic<std::uint64_t> pending_read_{0};
  std::atomic<std::uint64_t> applied_revision_{0};
  std::atomic<float> published_reduction_db_{0.0F};
  static_assert(std::atomic<float>::is_always_lock_free);
  std::uint64_t offered_revision_ = 0; // serialized control producer only
  std::vector<engine::StereoFrame> delay_;
  std::vector<Peak> peaks_;
  std::size_t delay_index_ = 0;
  std::size_t peak_head_ = 0;
  std::size_t peak_size_ = 0;
  std::uint64_t frame_index_ = 0;
  std::uint64_t generation_ = 0;
  std::uint64_t guarded_frames_ = 0;
  double gain_ = 1.0;
};

} // namespace oscillate::native_dsp
