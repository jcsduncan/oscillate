#include "oscillate/native_dsp/meter.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace oscillate::native_dsp {

static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

void StereoMeter::clear_window() noexcept {
  count_ = 0;
  left_square_sum_ = 0.0;
  right_square_sum_ = 0.0;
  left_peak_ = 0.0F;
  right_peak_ = 0.0F;
  invalid_frames_ = 0;
  clipped_ = false;
}

void StereoMeter::publish(MeterReading reading) noexcept {
  sequence_.fetch_add(1);
  left_peak_bits_.store(std::bit_cast<std::uint32_t>(reading.left_peak));
  right_peak_bits_.store(std::bit_cast<std::uint32_t>(reading.right_peak));
  left_rms_bits_.store(std::bit_cast<std::uint32_t>(reading.left_rms));
  right_rms_bits_.store(std::bit_cast<std::uint32_t>(reading.right_rms));
  end_sample_.store(reading.end_sample);
  published_generation_.store(reading.transport_generation);
  published_invalid_frames_.store(reading.invalid_frames);
  published_clipped_.store(reading.clipped);
  sequence_.fetch_add(1);
}

void StereoMeter::observe(std::span<const engine::StereoFrame> frames,
                          engine::BlockContext context) noexcept {
  if (context.transport_generation == 0 ||
      frames.size() > std::numeric_limits<std::uint64_t>::max() - context.project_sample_position) {
    if (generation_ != 0) {
      invalidate_after_quiesce();
    }
    return;
  }
  if (generation_ != context.transport_generation ||
      (generation_ != 0 && expected_sample_ != context.project_sample_position)) {
    const bool continuous_wrap = context.planned_loop_wrap && generation_ != 0 &&
                                 context.transport_generation == generation_ + 1;
    if (!continuous_wrap)
      clear_window();
    generation_ = context.transport_generation;
    expected_sample_ = context.project_sample_position;
    publish(MeterReading{.end_sample = expected_sample_, .transport_generation = generation_});
  }
  for (const auto& frame : frames) {
    const bool valid = std::isfinite(frame.left) && std::isfinite(frame.right);
    if (!valid) {
      ++invalid_frames_;
    }
    const float left = std::isfinite(frame.left) ? frame.left : 0.0F;
    const float right = std::isfinite(frame.right) ? frame.right : 0.0F;
    const float left_abs = std::abs(left);
    const float right_abs = std::abs(right);
    left_peak_ = std::max(left_peak_, left_abs);
    right_peak_ = std::max(right_peak_, right_abs);
    left_square_sum_ += static_cast<double>(left) * left;
    right_square_sum_ += static_cast<double>(right) * right;
    clipped_ = clipped_ || left_abs >= 1.0F || right_abs >= 1.0F;
    ++count_;
    ++expected_sample_;
    if (count_ == window_frames) {
      const auto window = static_cast<double>(window_frames);
      publish(MeterReading{.left_peak = left_peak_,
                           .right_peak = right_peak_,
                           .left_rms = static_cast<float>(std::sqrt(left_square_sum_ / window)),
                           .right_rms = static_cast<float>(std::sqrt(right_square_sum_ / window)),
                           .end_sample = expected_sample_,
                           .transport_generation = generation_,
                           .invalid_frames = invalid_frames_,
                           .clipped = clipped_});
      clear_window();
    }
  }
}

void StereoMeter::invalidate_after_quiesce() noexcept {
  clear_window();
  expected_sample_ = 0;
  generation_ = 0;
  publish({});
}

std::optional<MeterReading> StereoMeter::latest() const noexcept {
  for (int attempt = 0; attempt < 4; ++attempt) {
    const auto before = sequence_.load();
    if (before == 0 || (before & 1U) != 0) {
      continue;
    }
    MeterReading reading{.left_peak = std::bit_cast<float>(left_peak_bits_.load()),
                         .right_peak = std::bit_cast<float>(right_peak_bits_.load()),
                         .left_rms = std::bit_cast<float>(left_rms_bits_.load()),
                         .right_rms = std::bit_cast<float>(right_rms_bits_.load()),
                         .end_sample = end_sample_.load(),
                         .transport_generation = published_generation_.load(),
                         .invalid_frames = published_invalid_frames_.load(),
                         .clipped = published_clipped_.load()};
    if (before == sequence_.load()) {
      return reading;
    }
  }
  return std::nullopt;
}

} // namespace oscillate::native_dsp
