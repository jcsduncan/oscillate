#pragma once

#include "oscillate/engine/graph.hpp"
#include "oscillate/export/wav_writer.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>

namespace oscillate::exporting {

// Captures the final stereo device render at the project rate. The producer is
// the sole audio callback; drain() and finish() run on one export worker. The
// caller must join the callback before finish() or destruction. No callback
// operation allocates, locks or writes a file. A full ring invalidates the
// render instead of dropping samples or slowing the device.
class RealtimeWavCapture {
public:
  struct DeviceEvidence {
    // Unknown cannot establish an xrun-free real-time export.
    std::optional<std::uint64_t> xruns_before;
    std::optional<std::uint64_t> xruns_after;
    std::uint64_t render_failures = 0;
    std::uint64_t source_underruns = 0;
    std::uint64_t callback_over_period_blocks = 0;
  };

  [[nodiscard]] static foundation::Result<std::unique_ptr<RealtimeWavCapture>>
  begin(const std::filesystem::path& target, WavSpec spec, std::uint64_t first_sample,
        std::size_t ring_frames);

  RealtimeWavCapture(const RealtimeWavCapture&) = delete;
  RealtimeWavCapture& operator=(const RealtimeWavCapture&) = delete;

  // Called after the final master processor, before device delivery. A failed
  // render or discontinuous project position fails the whole capture. Frames
  // past the requested export end are ignored; no loop or seek is accepted.
  void capture(std::span<const engine::StereoFrame> frames, engine::BlockContext block,
               bool rendered) noexcept;

  // Returns the number of frames written to the private, unpublished WAV.
  [[nodiscard]] foundation::Result<std::size_t> drain();
  // Only after the callback is quiesced. Existing target remains intact on any
  // failure, including missing device xrun evidence or a short capture.
  [[nodiscard]] foundation::Result<PublishOutcome> finish(DeviceEvidence evidence);
  [[nodiscard]] std::uint64_t frames_captured() const noexcept {
    return captured_.load(std::memory_order_acquire);
  }
  // True once the capture has failed (render, discontinuity, overflow or
  // xrun): it can no longer complete. Lock-free; any thread.
  [[nodiscard]] bool failed() const noexcept {
    return failure_.load(std::memory_order_acquire) != Failure::none;
  }

private:
  RealtimeWavCapture(WavWriter writer, WavSpec spec, std::uint64_t first_sample,
                     std::size_t ring_frames);

  enum class Failure : std::uint8_t { none, render, discontinuity, overflow, xrun };
  void fail(Failure reason) noexcept;

  WavWriter writer_;
  std::unique_ptr<engine::StereoFrame[]> ring_;
  const std::size_t capacity_;
  const std::uint64_t first_sample_;
  const std::uint64_t expected_frames_;
  std::atomic<std::uint64_t> written_{0};
  std::atomic<std::uint64_t> read_{0};
  std::atomic<std::uint64_t> captured_{0};
  std::atomic<Failure> failure_{Failure::none};
  std::uint64_t generation_ = 0; // callback-owned
  bool finished_ = false;        // worker-owned
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
  static_assert(std::atomic<Failure>::is_always_lock_free);
};

} // namespace oscillate::exporting
