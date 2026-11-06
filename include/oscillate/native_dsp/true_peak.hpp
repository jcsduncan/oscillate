#pragma once

#include "oscillate/engine/graph.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace oscillate::native_dsp {

struct TruePeakReading {
  float left_sample_peak = 0.0F;
  float right_sample_peak = 0.0F;
  float left_true_peak = 0.0F;
  float right_true_peak = 0.0F;
  std::uint64_t frames = 0;
  std::uint64_t invalid_frames = 0;
};

// A single-owner, 4x intersample peak estimator: the project's one BS.1770
// true-peak core (a 33-tap windowed-sinc interpolator), shared with export
// and the Loudness effect. prepare() belongs on the control thread;
// process(), finish(), reset() and reading() are bounded, allocation-free,
// and must be called by the same owner. A seek requires reset. The caller
// must publish readings safely if another thread displays them.
class StereoTruePeakMeter final {
public:
  static constexpr std::size_t max_block_frames = 4096;
  static constexpr std::size_t kernel_radius = 16;
  static constexpr std::size_t kernel_taps = kernel_radius * 2 + 1;

  StereoTruePeakMeter() noexcept;
  ~StereoTruePeakMeter();
  StereoTruePeakMeter(StereoTruePeakMeter&&) noexcept;
  StereoTruePeakMeter& operator=(StereoTruePeakMeter&&) noexcept;
  StereoTruePeakMeter(const StereoTruePeakMeter&) = delete;
  StereoTruePeakMeter& operator=(const StereoTruePeakMeter&) = delete;

  // Only the project's four supported rates are admitted. This builds the
  // fixed, DC-normalised interpolation kernels outside the audio callback.
  [[nodiscard]] bool prepare(std::uint32_t sample_rate) noexcept;
  void reset() noexcept;

  // Rejects oversized blocks and an unprepared or finished meter atomically.
  // Non-finite channel samples count as invalid and are treated as zero.
  [[nodiscard]] bool process(std::span<const engine::StereoFrame> frames) noexcept;

  // Drains the fixed interpolation lookahead with zero padding. Idempotent;
  // call reset() before another stream. A live meter need not call finish().
  [[nodiscard]] bool finish() noexcept;
  [[nodiscard]] TruePeakReading reading() const noexcept { return reading_; }
  [[nodiscard]] std::uint32_t sample_rate() const noexcept { return sample_rate_; }

private:
  struct Interpolators;
  void consume(engine::StereoFrame frame) noexcept;

  std::unique_ptr<Interpolators> interpolators_;
  TruePeakReading reading_{};
  std::uint32_t sample_rate_ = 0;
  bool finished_ = false;
};

} // namespace oscillate::native_dsp
