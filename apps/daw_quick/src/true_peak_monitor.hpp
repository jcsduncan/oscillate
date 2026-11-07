#pragma once

#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/true_peak.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <thread>

namespace oscillate::quick {

struct LiveTruePeakReading {
  native_dsp::TruePeakReading peak;
  std::uint64_t end_sample = 0;
  std::uint64_t transport_generation = 0;
  std::uint64_t epoch = 0;
  bool complete = false;
  // The master section's other readings, from the same blocks: BS.1770
  // loudness since play, the last seek or a reset (absent until a window
  // is complete or while gated), and the mono check.
  std::optional<double> momentary_lufs;
  std::optional<double> short_term_lufs;
  std::optional<double> integrated_lufs;
  std::optional<double> correlation;
  std::optional<double> mono_loss_db;
};

// A single callback producer copies final stereo blocks into a bounded SPSC
// queue. The worker alone owns the interpolator, the loudness analyser and
// the correlation meter. A dropped block makes the current transport's
// reading incomplete until its next seek or restart.
class LiveTruePeakMonitor final {
public:
  static constexpr std::size_t max_callback_frames = 1024;
  static constexpr std::size_t queue_capacity = 64;

  LiveTruePeakMonitor();
  LiveTruePeakMonitor(const LiveTruePeakMonitor&) = delete;
  LiveTruePeakMonitor& operator=(const LiveTruePeakMonitor&) = delete;
  ~LiveTruePeakMonitor() = default;

  void set_sample_rate_after_quiesce(std::uint32_t sample_rate) noexcept;
  void invalidate_after_quiesce() noexcept;
  void observe(std::span<const engine::StereoFrame> frames, engine::BlockContext context) noexcept;
  [[nodiscard]] std::optional<LiveTruePeakReading> latest() const;
  [[nodiscard]] bool incomplete() const noexcept {
    return incomplete_.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::uint64_t dropped_blocks() const noexcept {
    return dropped_blocks_.load(std::memory_order_relaxed);
  }
  // Control side: restart integrated loudness from the next block, keeping
  // the true peak (the master section's reset button).
  void reset_loudness() noexcept { loudness_reset_.fetch_add(1, std::memory_order_release); }

private:
  struct Block {
    std::array<engine::StereoFrame, max_callback_frames> frames{};
    std::size_t count = 0;
    std::uint64_t end_sample = 0;
    std::uint64_t generation = 0;
    std::uint64_t epoch = 0;
    bool complete = false;
  };

  // A producer epoch: its transport generation, the project sample it
  // expects next, and whether its blocks are all present.
  struct Epoch {
    std::uint64_t generation = 0;
    std::uint64_t sample = 0;
    bool complete = false;
  };
  void begin_epoch(Epoch epoch) noexcept;
  void run(const std::stop_token& stop) noexcept;

  std::array<Block, queue_capacity> queue_{};
  std::atomic<std::uint64_t> write_sequence_{0};
  std::atomic<std::uint64_t> read_sequence_{0};
  std::atomic<std::uint64_t> published_epoch_{0};
  std::atomic<std::uint64_t> dropped_blocks_{0};
  std::atomic<bool> incomplete_{false};
  std::atomic<std::uint32_t> sample_rate_{0};
  std::atomic<std::uint64_t> loudness_reset_{0};
  std::uint64_t producer_epoch_ = 0;
  std::uint64_t producer_generation_ = 0;
  std::uint64_t expected_sample_ = 0;
  bool producer_complete_ = false;
  mutable std::mutex latest_mutex_;
  std::optional<LiveTruePeakReading> latest_;
  // Destroyed first: the worker joins before the queue and reading state.
  std::jthread worker_;
};

} // namespace oscillate::quick
