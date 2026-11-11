#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

// Frames captured on the callback for analysis off it (ADR-0125, ADR-0129):
// by the thread that reads an effect's telemetry, or the analysis worker,
// one at a time. One producer, one consumer, and neither ever waits. The
// callback drops a block that does not fit, so an analysis nobody reads
// costs the callback one comparison a block; the next block that fits
// starts a new run. A reset on the callback (a seek) starts a new run too.
// The reader drops whatever came before a new run and starts its analysis
// afresh there.
namespace oscillate::native_dsp::effects {

template <typename Frame> class CaptureQueue {
public:
  // Control only: room for at least `frames`.
  explicit CaptureQueue(std::size_t frames) {
    std::size_t capacity = 1;
    while (capacity < frames) {
      capacity *= 2;
    }
    frames_.resize(capacity);
    mask_ = capacity - 1;
  }

  // Callback only: `count` frames, frame i from fill(i). False when they
  // were dropped for want of room.
  template <typename Fill> bool write(std::size_t count, Fill&& fill) noexcept {
    const std::uint64_t taken = taken_.load(std::memory_order_acquire);
    if (written_ - taken + count > frames_.size()) {
      dropping_ = true;
      return false;
    }
    if (dropping_) {
      restart();
    }
    for (std::size_t i = 0; i < count; ++i) {
      frames_[(written_ + i) & mask_] = fill(i);
    }
    written_ += count;
    published_.store(written_, std::memory_order_release);
    return true;
  }

  // Callback only: true when more than half the room is waiting for the
  // reader.
  [[nodiscard]] bool backlogged() const noexcept {
    return written_ - taken_.load(std::memory_order_acquire) > frames_.size() / 2;
  }

  // Callback only: what follows starts a new run.
  void restart() noexcept {
    dropping_ = false;
    run_start_.store(written_, std::memory_order_relaxed);
    run_.fetch_add(1, std::memory_order_release);
  }

  // Reader only: every frame published since the last read, in order.
  // `start` runs before the first frame of each new run; `take(frame)` for
  // each frame of the current run.
  template <typename Start, typename Take> void read(Start&& start, Take&& take) {
    const std::uint64_t end = published_.load(std::memory_order_acquire);
    // Loaded after `end`: every run that began at or before it is seen.
    const std::uint64_t run = run_.load(std::memory_order_acquire);
    if (run != run_seen_) {
      run_seen_ = run;
      next_run_ = run_start_.load(std::memory_order_relaxed);
      pending_ = true;
    }
    while (read_ < end) {
      if (pending_) {
        // A run starts at or after what was read so far; drop up to it.
        if (next_run_ > end) {
          read_ = end;
          break;
        }
        read_ = next_run_ > read_ ? next_run_ : read_;
        pending_ = false;
        start();
        continue;
      }
      take(frames_[read_ & mask_]);
      ++read_;
    }
    taken_.store(read_, std::memory_order_release);
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return frames_.size(); }

private:
  std::vector<Frame> frames_;
  std::size_t mask_ = 0;
  // Frames written, ever: the callback's count, and the one it publishes.
  std::uint64_t written_ = 0;
  std::atomic<std::uint64_t> published_{0};
  // Frames the reader has finished with.
  std::atomic<std::uint64_t> taken_{0};
  // Runs started, and where the newest one starts.
  std::atomic<std::uint64_t> run_{0};
  std::atomic<std::uint64_t> run_start_{0};
  bool dropping_ = false;
  // Reader only.
  std::uint64_t read_ = 0;
  std::uint64_t run_seen_ = 0;
  std::uint64_t next_run_ = 0;
  bool pending_ = false;
};

} // namespace oscillate::native_dsp::effects
