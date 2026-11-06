#pragma once

#include "oscillate/engine/graph.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace oscillate::native_dsp {

// A 1024-frame, non-overlapping sample-peak/RMS window. One audio callback
// writes; any control/UI thread may read a complete published window.
struct MeterReading {
  float left_peak = 0.0F;
  float right_peak = 0.0F;
  float left_rms = 0.0F;
  float right_rms = 0.0F;
  std::uint64_t end_sample = 0;
  std::uint64_t transport_generation = 0;
  std::uint32_t invalid_frames = 0;
  bool clipped = false;
};

class StereoMeter final {
public:
  static constexpr std::size_t window_frames = 1024;

  StereoMeter() = default;
  StereoMeter(const StereoMeter&) = delete;
  StereoMeter& operator=(const StereoMeter&) = delete;

  // Observes without modifying audio. A seek/loop generation or an unexpected
  // position discards an incomplete window and publishes silence immediately.
  void observe(std::span<const engine::StereoFrame> frames, engine::BlockContext context) noexcept;

  // The transport owner calls this after the final callback has quiesced, so
  // stopped playback cannot leave an old clipping/level indication visible.
  void invalidate_after_quiesce() noexcept;

  // Bounded retries; a failed read simply leaves the previous UI value intact.
  [[nodiscard]] std::optional<MeterReading> latest() const noexcept;

private:
  void publish(MeterReading reading) noexcept;
  void clear_window() noexcept;

  std::uint64_t expected_sample_ = 0;
  std::uint64_t generation_ = 0;
  std::size_t count_ = 0;
  double left_square_sum_ = 0.0;
  double right_square_sum_ = 0.0;
  float left_peak_ = 0.0F;
  float right_peak_ = 0.0F;
  std::uint32_t invalid_frames_ = 0;
  bool clipped_ = false;

  std::atomic<std::uint64_t> sequence_{0};
  std::atomic<std::uint32_t> left_peak_bits_{0};
  std::atomic<std::uint32_t> right_peak_bits_{0};
  std::atomic<std::uint32_t> left_rms_bits_{0};
  std::atomic<std::uint32_t> right_rms_bits_{0};
  std::atomic<std::uint64_t> end_sample_{0};
  std::atomic<std::uint64_t> published_generation_{0};
  std::atomic<std::uint32_t> published_invalid_frames_{0};
  std::atomic<bool> published_clipped_{false};
};

} // namespace oscillate::native_dsp
