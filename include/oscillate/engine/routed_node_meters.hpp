#pragma once

#include "oscillate/engine/graph.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace oscillate::engine {

// One node's post-fader sample peak and RMS over its most recent complete
// window. A seek, loop restart or position gap publishes silence at once.
struct RoutedNodeMeterReading {
  float left_peak = 0.0F;
  float right_peak = 0.0F;
  float left_rms = 0.0F;
  float right_rms = 0.0F;
  std::uint64_t end_sample = 0;
  std::uint64_t transport_generation = 0;
  bool clipped = false;
};

// Fixed per-node meter slots, sized once off the callback. The single
// callback writer accumulates each node's block and publishes a window once
// at least window_frames have been observed, at a block boundary. Control
// readers copy one slot through a bounded seqlock retry; no lock, allocation
// or syscall is used on either side.
class RoutedNodeMeters final {
public:
  static constexpr std::size_t window_frames = 1024;

  explicit RoutedNodeMeters(std::size_t nodes);
  RoutedNodeMeters(const RoutedNodeMeters&) = delete;
  RoutedNodeMeters& operator=(const RoutedNodeMeters&) = delete;

  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  // Callback only. Frames are finite (the mixer sanitizes node output).
  void observe(std::size_t node, std::span<const StereoFrame> frames,
               BlockContext context) noexcept;

  // Any control thread. Empty before the first publication, for an unknown
  // node, or when every bounded retry overlapped a callback write.
  [[nodiscard]] std::optional<RoutedNodeMeterReading> latest(std::size_t node) const noexcept;
  // Any control thread, when playback renders ahead of what is heard
  // (ADR-0128): the newest window of `generation` that ends by `heard_sample`,
  // among the last `history` windows, or the oldest of that generation when
  // none has been heard yet (a graph switched in ahead of the ear). Empty
  // without a window of that generation.
  [[nodiscard]] std::optional<RoutedNodeMeterReading>
  heard(std::size_t node, std::uint64_t heard_sample, std::uint64_t generation) const noexcept;

  // Windows kept per node: more than render-ahead's longest window (200 ms
  // at 96 kHz) holds.
  static constexpr std::size_t history = 32;

private:
  // One published window, read through its own seqlock.
  struct Window {
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<std::uint32_t> left_peak_bits{0};
    std::atomic<std::uint32_t> right_peak_bits{0};
    std::atomic<std::uint32_t> left_rms_bits{0};
    std::atomic<std::uint32_t> right_rms_bits{0};
    std::atomic<std::uint64_t> end_sample{0};
    std::atomic<std::uint64_t> published_generation{0};
    std::atomic<bool> published_clipped{false};
  };
  struct Slot {
    // Callback-owned window state.
    std::uint64_t expected_sample = 0;
    std::uint64_t generation = 0;
    std::size_t count = 0;
    double left_square_sum = 0.0;
    double right_square_sum = 0.0;
    float left_peak = 0.0F;
    float right_peak = 0.0F;
    bool clipped = false;

    // The last `history` windows, oldest overwritten first; `published`
    // counts every window, so the newest is at (published - 1) % history.
    std::array<Window, history> windows{};
    std::atomic<std::uint64_t> published{0};
  };

  static void clear_window(Slot& slot) noexcept;
  static void publish(Slot& slot, const RoutedNodeMeterReading& reading) noexcept;
  // One bounded seqlock read of a window; empty when every retry overlapped a write.
  [[nodiscard]] static std::optional<RoutedNodeMeterReading> read(const Window& window) noexcept;

  std::unique_ptr<Slot[]> slots_;
  std::size_t size_;
};

} // namespace oscillate::engine
