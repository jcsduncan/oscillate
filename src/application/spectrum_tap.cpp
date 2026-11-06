#include "oscillate/application/spectrum_tap.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace oscillate::application {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert((SpectrumWindow::frame_count & (SpectrumWindow::frame_count - 1)) == 0);

void StereoSpectrumTap::reset_window(engine::BlockContext context) noexcept {
  producer_generation_ = context.transport_generation;
  expected_sample_ = context.project_sample_position;
  write_position_ = 0;
  filled_frames_ = 0;
  frames_since_publish_ = 0;
  ++producer_epoch_;
  // Release makes the new epoch visible to a worker that may already have
  // copied an older queue slot. The worker checks again before returning it.
  published_epoch_.store(producer_epoch_, std::memory_order_release);
}

void StereoSpectrumTap::publish_window() noexcept {
  const auto write = write_sequence_.load(std::memory_order_relaxed);
  const auto read = read_sequence_.load(std::memory_order_acquire);
  if (write - read >= queue_capacity) {
    dropped_windows_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  auto& slot = queue_[write % queue_capacity];
  for (std::size_t index = 0; index < SpectrumWindow::frame_count; ++index) {
    slot.frames[index] = rolling_[(write_position_ + index) & (SpectrumWindow::frame_count - 1)];
  }
  slot.end_sample = expected_sample_;
  slot.transport_generation = producer_generation_;
  slot.epoch = producer_epoch_;
  write_sequence_.store(write + 1, std::memory_order_release);
}

bool StereoSpectrumTap::observe(std::span<const engine::StereoFrame> frames,
                                engine::BlockContext context) noexcept {
  if (context.transport_generation == 0 || frames.size() > max_callback_frames ||
      frames.size() > std::numeric_limits<std::uint64_t>::max() - context.project_sample_position) {
    rejected_blocks_.fetch_add(1, std::memory_order_relaxed);
    reset_window({0, 0});
    return false;
  }
  if (producer_generation_ != context.transport_generation ||
      (producer_generation_ != 0 && expected_sample_ != context.project_sample_position)) {
    reset_window(context);
  }

  for (const auto& frame : frames) {
    rolling_[write_position_] = frame;
    write_position_ = (write_position_ + 1) & (SpectrumWindow::frame_count - 1);
    const auto first_complete = filled_frames_ == SpectrumWindow::frame_count - 1;
    if (filled_frames_ < SpectrumWindow::frame_count) {
      ++filled_frames_;
    } else {
      ++frames_since_publish_;
    }
    ++expected_sample_;
    if (first_complete || frames_since_publish_ == hop_frames) {
      publish_window();
      frames_since_publish_ = 0;
    }
  }
  return true;
}

bool StereoSpectrumTap::try_pop(SpectrumWindow& output) noexcept {
  for (std::size_t attempt = 0; attempt < queue_capacity; ++attempt) {
    const auto read = read_sequence_.load(std::memory_order_relaxed);
    const auto write = write_sequence_.load(std::memory_order_acquire);
    if (read == write) {
      return false;
    }
    const auto& slot = queue_[read % queue_capacity];
    const auto slot_epoch = slot.epoch;
    const auto current_epoch = published_epoch_.load(std::memory_order_acquire);
    if (slot_epoch == current_epoch) {
      output = slot;
    }
    read_sequence_.store(read + 1, std::memory_order_release);
    if (slot_epoch == current_epoch &&
        current_epoch == published_epoch_.load(std::memory_order_acquire)) {
      return true;
    }
  }
  return false;
}

void StereoSpectrumTap::invalidate_after_quiesce() noexcept { reset_window({0, 0}); }

} // namespace oscillate::application
