#pragma once

#include "oscillate/engine/graph.hpp"
#include "oscillate/foundation/error.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace oscillate::native_dsp {

struct CompressorSettings {
  double threshold_db = -18.0;
  double ratio = 4.0;
  double knee_db = 6.0;
  double attack_ms = 10.0;
  double release_ms = 100.0;
  double makeup_db = 0.0;
  double sidechain_highpass_hz = 0.0; // 0 bypasses the detector filter.
};

class PreparedCompressor {
public:
  [[nodiscard]] std::uint32_t sample_rate() const noexcept { return sample_rate_; }
  [[nodiscard]] CompressorSettings settings() const noexcept { return settings_; }

private:
  struct Coefficients {
    double attack;
    double release;
    double makeup;
    double sidechain_highpass;
  };
  PreparedCompressor(CompressorSettings settings, std::uint32_t sample_rate,
                     Coefficients coefficients) noexcept
      : settings_(settings), sample_rate_(sample_rate), attack_(coefficients.attack),
        release_(coefficients.release), makeup_(coefficients.makeup),
        sidechain_highpass_(coefficients.sidechain_highpass) {}

  CompressorSettings settings_;
  std::uint32_t sample_rate_;
  double attack_;
  double release_;
  double makeup_;
  double sidechain_highpass_;

  friend foundation::Result<PreparedCompressor> prepare_compressor(CompressorSettings,
                                                                   std::uint32_t);
  friend class CompressorRuntime;
};

// Control-thread validation and coefficient preparation. No lookahead or latency.
// The linked detector is max(abs(left), abs(right)) per sample, optionally
// after a first-order sidechain high-pass, then smoothed in linear amplitude
// using attack on rising and release on falling peaks.
// Above threshold + knee/2, gain reduction in dB is
// (detector_dB - threshold_dB) * (1 - 1/ratio). Below threshold - knee/2
// it is zero; within the knee it is (1 - 1/ratio) *
// (detector_dB - threshold_dB + knee/2)^2 / (2*knee).
// The resulting gain, including makeup, is identical on both output channels.
[[nodiscard]] foundation::Result<PreparedCompressor> prepare_compressor(CompressorSettings settings,
                                                                        std::uint32_t sample_rate);

// The static gain computer above: the gain reduction in dB (non-negative,
// excluding makeup) for a steady detector level `detector_dB`, as the runtime
// applies it. For displays of the transfer curve.
[[nodiscard]] double compressor_reduction_db(double detector_db,
                                             const CompressorSettings& settings) noexcept;

class CompressorRuntime final : public engine::SidechainProcessorRuntime {
public:
  [[nodiscard]] bool control_overlay_revision_independent() const noexcept override { return true; }
  explicit CompressorRuntime(PreparedCompressor prepared) noexcept : prepared_(prepared) {}

  // Control side only, while the audio callback is held at a block boundary.
  // Replacing coefficients preserves detector envelope and filter history.
  [[nodiscard]] PreparedCompressor prepared_while_held() const noexcept { return prepared_; }
  void replace_prepared_while_held(PreparedCompressor prepared) noexcept {
    prepared_ = prepared;
    fading_from_.reset();
    transition_remaining_ = 0;
  }

  // One control producer offers prepared zero-latency settings. The callback
  // adopts the newest revision at a block boundary without resetting detector
  // state; a full mailbox leaves the project edit pending for control recovery.
  [[nodiscard]] bool offer_prepared(PreparedCompressor prepared, std::uint64_t revision) noexcept;
  [[nodiscard]] std::uint64_t applied_prepared_revision() const noexcept {
    return applied_revision_.load(std::memory_order_acquire);
  }

  // Greatest gain reduction applied in the latest processed block, as a
  // non-negative dB magnitude excluding makeup gain. Each callback publishes
  // one value after its block; any control/UI thread may read it lock-free.
  // The detector is the external sidechain when one is supplied. During a
  // live setting transition the reading follows the same 64-frame linear
  // crossfade between the old and new makeup-free gains, so a makeup-only
  // edit never reads as reduction. An empty block, a guarded block (stale
  // generation or mismatched sidechain) and reset_after_quiesce() publish
  // 0 dB: the guard's silencing is not compressor reduction. Non-finite
  // input still counts the gain the detector applied to that frame.
  [[nodiscard]] float latest_gain_reduction_db() const noexcept {
    return published_reduction_db_.load(std::memory_order_acquire);
  }

  void process(std::span<engine::StereoFrame> stereo,
               engine::BlockContext context) noexcept override;

  // Explicit sidechain entry point for a future graph adapter. Buffers must have
  // equal frame counts; a mismatch silences the output and increments the guard.
  // The sidechain may alias the processed buffer. Single callback owner only.
  void process_with_sidechain(std::span<engine::StereoFrame> stereo,
                              std::span<const engine::StereoFrame> sidechain,
                              engine::BlockContext context) noexcept override;

  // Call only after the callback has stopped and joined.
  void reset_after_quiesce() noexcept;
  [[nodiscard]] std::uint64_t guarded_frames_after_quiesce() const noexcept {
    return guarded_frames_;
  }

private:
  struct PendingCoefficients {
    PreparedCompressor prepared;
    std::uint64_t revision;
  };
  static constexpr std::size_t mailbox_capacity = 4;
  static constexpr std::uint32_t gain_transition_frames = 64;
  void adopt_pending() noexcept;
  // Curve gain without makeup; 1 means no reduction.
  [[nodiscard]] static double reduction_gain_for(double envelope,
                                                 const PreparedCompressor& prepared) noexcept;
  void publish_reduction(double lowest_reduction_gain) noexcept;

  void process_frames(std::span<engine::StereoFrame> stereo,
                      std::span<const engine::StereoFrame> sidechain,
                      engine::BlockContext context) noexcept;

  PreparedCompressor prepared_;
  std::optional<PreparedCompressor> fading_from_;
  std::uint32_t transition_remaining_ = 0;
  const std::uint32_t sample_rate_ = prepared_.sample_rate();
  std::array<std::optional<PendingCoefficients>, mailbox_capacity> pending_{};
  std::atomic<std::uint64_t> pending_write_{0};
  std::atomic<std::uint64_t> pending_read_{0};
  std::atomic<std::uint64_t> applied_revision_{0};
  std::uint64_t offered_revision_ = 0; // control producer only
  std::atomic<float> published_reduction_db_{0.0F};
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
  static_assert(std::atomic<float>::is_always_lock_free);
  double envelope_ = 0.0;
  struct HighPassState {
    double input = 0.0;
    double output = 0.0;
  };
  HighPassState sidechain_left_;
  HighPassState sidechain_right_;
  std::uint64_t generation_ = 0;
  std::uint64_t guarded_frames_ = 0;
};

} // namespace oscillate::native_dsp
