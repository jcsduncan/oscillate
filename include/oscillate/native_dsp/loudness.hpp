#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace oscillate::native_dsp {

// Worker/offline integrated loudness only. Interleaved float frames in the
// specified channel order; no real-time or true-peak contract is implied.
enum class LoudnessLayout : std::uint8_t { mono, stereo, surround_5_1 };
enum class LoudnessStatus : std::uint8_t {
  ok,
  unsupported_rate,
  invalid_configuration,
  malformed_input,
  capacity_exceeded,
};

struct LoudnessResult {
  LoudnessStatus status = LoudnessStatus::ok;
  // Empty when fewer than 400 ms were supplied or all blocks fell below the
  // absolute gate; no arbitrary floor is reported as a measurement.
  std::optional<double> integrated_lkfs;
  // Latest complete 400 ms and 3 s windows, updated every 100 ms. These
  // readings are ungated; silence and incomplete windows have no value.
  std::optional<double> momentary_lkfs;
  std::optional<double> short_term_lkfs;
  std::size_t total_blocks = 0;
  std::size_t absolute_gated_blocks = 0;
  std::size_t relative_gated_blocks = 0;
};

// The project's one BS.1770 loudness core (K-weighting, 100 ms steps and
// exact two-stage gating), shared with the master meters and the Loudness
// effect, so they agree on the same signal.
class IntegratedLoudnessAnalyzer final {
public:
  // A maximum of max_blocks complete overlapping windows is retained. The
  // default covers one hour at every supported rate with a 100 ms block step.
  explicit IntegratedLoudnessAnalyzer(std::uint32_t sample_rate, LoudnessLayout layout,
                                      std::size_t max_blocks = 36'000);
  ~IntegratedLoudnessAnalyzer();
  IntegratedLoudnessAnalyzer(IntegratedLoudnessAnalyzer&&) noexcept;
  IntegratedLoudnessAnalyzer& operator=(IntegratedLoudnessAnalyzer&&) noexcept;
  IntegratedLoudnessAnalyzer(const IntegratedLoudnessAnalyzer&) = delete;
  IntegratedLoudnessAnalyzer& operator=(const IntegratedLoudnessAnalyzer&) = delete;

  [[nodiscard]] LoudnessStatus append(std::span<const float> interleaved_samples);
  [[nodiscard]] LoudnessResult finish() const noexcept;
  [[nodiscard]] LoudnessStatus status() const noexcept { return status_; }
  [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
  void reset() noexcept;

private:
  struct Core;

  LoudnessStatus status_ = LoudnessStatus::ok;
  LoudnessLayout layout_ = LoudnessLayout::stereo;
  std::size_t channels_ = 0;
  std::size_t max_blocks_ = 0;
  std::size_t window_frames_ = 0;
  std::size_t step_frames_ = 0;
  std::uint64_t frames_ = 0;
  std::unique_ptr<Core> core_;
};

} // namespace oscillate::native_dsp
