#pragma once

#include <oscillate/application/clip_playback.hpp>
#include <oscillate/foundation/error.hpp>
#include <oscillate/media/audio_reader.hpp>
#include <oscillate/media/prefetch.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace oscillate::application {

// Injectable monotonic tick source. Tests advance it explicitly; production may
// supply steady_clock ticks. Only the control thread calls now.
struct PrefetchClock {
  void* context = nullptr;
  std::uint64_t (*now)(void*) noexcept = nullptr;
};

// One immutable generation/range request with a worker-owned cursor. Exactly
// one prefetch worker may call fill_step at a time. It never runs in the audio
// callback. The control thread reads published_frames and complete only after
// the worker has stopped, or through their atomic observations.
class PrefetchFillJob {
public:
  PrefetchFillJob(const PrefetchFillJob&) = delete;
  PrefetchFillJob& operator=(const PrefetchFillJob&) = delete;

  [[nodiscard]] foundation::Result<std::size_t> fill_step(media::WavReader& source,
                                                          std::uint32_t project_rate,
                                                          std::span<media::StereoFrame> scratch);
  [[nodiscard]] foundation::Result<std::size_t> fill_step(media::AudioReader& source,
                                                          std::uint32_t project_rate,
                                                          std::span<media::StereoFrame> scratch);
  [[nodiscard]] std::size_t published_frames() const noexcept {
    return published_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool complete() const noexcept { return complete_.load(std::memory_order_acquire); }
  [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

private:
  friend class PrefetchControl;
  PrefetchFillJob(std::shared_ptr<media::PrefetchRing> ring, std::uint64_t generation,
                  std::uint64_t first, std::uint64_t end) noexcept;

  std::shared_ptr<media::PrefetchRing> ring_;
  const std::uint64_t generation_;
  const std::uint64_t end_;
  std::uint64_t next_; // Worker-owned.
  std::atomic<std::size_t> published_{0};
  std::atomic<bool> complete_{false};
};

enum class PrimingStatus : std::uint8_t { idle, priming, ready, timed_out };

struct PrimingPolicy {
  std::size_t minimum_frames;
  std::uint64_t timeout_ticks;
};

// One-clip control-thread sequencer. begin_start/begin_seek require a quiesced
// callback and worker; they discard pending frames and invalidate old jobs.
// The caller must not start playback until poll reports ready. Planned wraps
// do not reset the ring: current and next generations can coexist in FIFO order.
class PrefetchControl {
public:
  PrefetchControl(std::shared_ptr<media::PrefetchRing> ring,
                  std::shared_ptr<PlaybackReadiness> readiness, PrefetchClock clock);

  [[nodiscard]] foundation::Result<std::shared_ptr<PrefetchFillJob>>
  begin_start(std::uint64_t generation, std::uint64_t first, std::uint64_t end,
              PrimingPolicy policy);
  [[nodiscard]] foundation::Result<std::shared_ptr<PrefetchFillJob>>
  begin_seek(std::uint64_t generation, std::uint64_t first, std::uint64_t end,
             PrimingPolicy policy);
  [[nodiscard]] PrimingStatus poll() noexcept;
  [[nodiscard]] PrimingStatus status() const noexcept { return status_; }

  // Call after the current job has fully queued its source range. The worker
  // fills this job when current playback frees ring slots. A wrap is committed
  // only when the requested lead is actually published in the next generation.
  [[nodiscard]] foundation::Result<std::shared_ptr<PrefetchFillJob>>
  plan_loop_wrap(std::uint64_t next_generation, std::uint64_t first, std::uint64_t end,
                 std::size_t minimum_frames);
  [[nodiscard]] bool loop_ready() const noexcept;
  [[nodiscard]] bool commit_loop_wrap() noexcept;

private:
  [[nodiscard]] foundation::Result<std::shared_ptr<PrefetchFillJob>>
  begin(std::uint64_t generation, std::uint64_t first, std::uint64_t end, PrimingPolicy policy);

  std::shared_ptr<media::PrefetchRing> ring_;
  std::shared_ptr<PlaybackReadiness> readiness_;
  PrefetchClock clock_;
  std::shared_ptr<PrefetchFillJob> current_;
  std::shared_ptr<PrefetchFillJob> next_loop_;
  std::uint64_t deadline_ = 0;
  std::size_t minimum_frames_ = 0;
  std::size_t loop_minimum_frames_ = 0;
  PrimingStatus status_ = PrimingStatus::idle;
};

} // namespace oscillate::application
