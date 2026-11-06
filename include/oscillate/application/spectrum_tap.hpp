#pragma once

#include "oscillate/engine/graph.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace oscillate::application {

// A worker-owned copy of a chronological 2048-frame stereo window. The epoch
// changes on seek, loop, unexpected position or stop. Consumers should verify
// epoch again before presenting an asynchronously analyzed result.
struct SpectrumWindow {
  static constexpr std::size_t frame_count = 2048;

  std::array<engine::StereoFrame, frame_count> frames{};
  std::uint64_t end_sample = 0;
  std::uint64_t transport_generation = 0;
  std::uint64_t epoch = 0;
};

class StereoSpectrumTap final {
public:
  static constexpr std::size_t hop_frames = SpectrumWindow::frame_count / 2;
  static constexpr std::size_t queue_capacity = 4;
  static constexpr std::size_t max_callback_frames = 1024;

  StereoSpectrumTap() = default;
  StereoSpectrumTap(const StereoSpectrumTap&) = delete;
  StereoSpectrumTap& operator=(const StereoSpectrumTap&) = delete;

  // One callback producer. A false result invalidates the incomplete window;
  // zero generation, oversized blocks and sample-position overflow are rejected.
  // Queue overflow drops a complete new window, never blocks or overwrites a
  // window the worker could be reading. Audio itself is never modified.
  [[nodiscard]] bool observe(std::span<const engine::StereoFrame> frames,
                             engine::BlockContext context) noexcept;

  // One worker consumer. Bounded to at most queue_capacity stale slots.
  // Returns only complete windows from the currently published epoch.
  [[nodiscard]] bool try_pop(SpectrumWindow& output) noexcept;

  // Call only after the producer callback has quiesced.
  void invalidate_after_quiesce() noexcept;

  [[nodiscard]] std::uint64_t epoch() const noexcept { return published_epoch_.load(); }
  [[nodiscard]] std::uint64_t dropped_windows() const noexcept { return dropped_windows_.load(); }
  [[nodiscard]] std::uint64_t rejected_blocks() const noexcept { return rejected_blocks_.load(); }

private:
  void reset_window(engine::BlockContext context) noexcept;
  void publish_window() noexcept;

  // Producer-only rolling state.
  std::array<engine::StereoFrame, SpectrumWindow::frame_count> rolling_{};
  std::uint64_t expected_sample_ = 0;
  std::uint64_t producer_generation_ = 0;
  std::uint64_t producer_epoch_ = 0;
  std::size_t write_position_ = 0;
  std::size_t filled_frames_ = 0;
  std::size_t frames_since_publish_ = 0;

  // Single-producer/single-consumer queue. The consumer advances read_sequence_
  // only after copying a slot; the producer sees that release before reuse.
  std::array<SpectrumWindow, queue_capacity> queue_{};
  std::atomic<std::uint64_t> write_sequence_{0};
  std::atomic<std::uint64_t> read_sequence_{0};
  std::atomic<std::uint64_t> published_epoch_{0};
  std::atomic<std::uint64_t> dropped_windows_{0};
  std::atomic<std::uint64_t> rejected_blocks_{0};
};

} // namespace oscillate::application
