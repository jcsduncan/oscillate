#pragma once

#include <oscillate/application/prefetch_control.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>

namespace oscillate::application {

enum class ClipPrefetchState : std::uint8_t { filling, complete, failed, stopped };

struct ClipPrefetchStatus {
  ClipPrefetchState state;
  std::size_t published_frames;
};

struct ClipPrefetchRequest {
  std::uint32_t project_rate;
  std::uint64_t generation;
  std::uint64_t first_source_frame;
  std::uint64_t end_source_frame;
  PrimingPolicy priming;
};

// One source range, one ring producer, and one transport generation. Start only
// while the callback is quiesced. The caller owns the ring and readiness shared
// with ClipFeed; neither is accessed through this object by the callback.
// Keep this object alive while its queued frames are being played, including
// after state becomes complete. Stop playback before requesting stop or destroying
// it: both actions clear readiness. Destruction can join an active producer, so
// move an unfinished object to an off-UI retirement task after stopping playback.
class ClipPrefetchWorker {
public:
  [[nodiscard]] static foundation::Result<std::unique_ptr<ClipPrefetchWorker>>
  start(media::WavReader reader, std::shared_ptr<media::PrefetchRing> ring,
        std::shared_ptr<PlaybackReadiness> readiness, ClipPrefetchRequest request,
        PrefetchClock clock);
  [[nodiscard]] static foundation::Result<std::unique_ptr<ClipPrefetchWorker>>
  start(media::AudioReader reader, std::shared_ptr<media::PrefetchRing> ring,
        std::shared_ptr<PlaybackReadiness> readiness, ClipPrefetchRequest request,
        PrefetchClock clock);

  ClipPrefetchWorker(const ClipPrefetchWorker&) = delete;
  ClipPrefetchWorker& operator=(const ClipPrefetchWorker&) = delete;
  ~ClipPrefetchWorker();

  // Single control-thread polling. A failed fill returns its original error. A prime
  // timeout stops the producer, but the caller must still retire it safely.
  [[nodiscard]] foundation::Result<PrimingStatus> poll_priming();
  [[nodiscard]] ClipPrefetchStatus status() const noexcept;
  [[nodiscard]] std::optional<foundation::Error> failure() const;

  void request_stop() noexcept;
  [[nodiscard]] bool finished() const noexcept;
  // Never joins an active producer. UI may poll finished() and call this later,
  // or move ownership to a background retirement task. A true result means the
  // thread is joined; false means the caller must retain ownership and retry.
  [[nodiscard]] bool join_finished() noexcept;

private:
  ClipPrefetchWorker(std::shared_ptr<media::PrefetchRing> ring,
                     std::shared_ptr<PlaybackReadiness> readiness, PrefetchClock clock);
  void run(media::AudioReader reader, std::uint32_t project_rate) noexcept;

  std::shared_ptr<media::PrefetchRing> ring_;
  std::shared_ptr<PlaybackReadiness> readiness_;
  PrefetchControl control_;
  std::shared_ptr<PrefetchFillJob> job_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> finished_{false};
  std::atomic<ClipPrefetchState> state_{ClipPrefetchState::filling};
  foundation::Error error_{foundation::ErrorCode::internal, ""}; // Published by failed state.
};

} // namespace oscillate::application
