#include "oscillate/native_dsp/limiter.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace oscillate::native_dsp {
namespace {
using foundation::ErrorCode;
using foundation::Result;

bool supported_rate(std::uint32_t rate) noexcept {
  return rate == 44'100 || rate == 48'000 || rate == 88'200 || rate == 96'000;
}
} // namespace

Result<PreparedLimiter> prepare_limiter(LimiterSettings settings, std::uint32_t sample_rate) {
  if (!supported_rate(sample_rate) || !std::isfinite(settings.ceiling_db) ||
      settings.ceiling_db < -30.0 || settings.ceiling_db > 0.0 ||
      !std::isfinite(settings.lookahead_ms) || settings.lookahead_ms < 0.0 ||
      settings.lookahead_ms > 10.0 || !std::isfinite(settings.release_ms) ||
      settings.release_ms < 1.0 || settings.release_ms > 2'000.0) {
    return Result<PreparedLimiter>::failure(
        {ErrorCode::invalid_argument, "invalid limiter rate or controls"});
  }
  const auto latency = static_cast<std::uint32_t>(
      std::llround(settings.lookahead_ms * static_cast<double>(sample_rate) / 1'000.0));
  const double ceiling = oscillate::native_dsp::dsp::db_to_gain(settings.ceiling_db);
  const double release = std::exp(-1'000.0 / (settings.release_ms * sample_rate));
  return Result<PreparedLimiter>::success(
      PreparedLimiter(settings, {sample_rate, latency, ceiling, release}));
}

LimiterRuntime::LimiterRuntime(PreparedLimiter prepared)
    : prepared_(prepared), sample_rate_(prepared.sample_rate()),
      latency_frames_(prepared.latency_frames()),
      delay_(static_cast<std::size_t>(prepared.latency_frames()) + 1), peaks_(delay_.size()) {}

bool LimiterRuntime::offer_prepared(PreparedLimiter prepared, std::uint64_t revision) noexcept {
  if (prepared.sample_rate() != sample_rate_ || prepared.latency_frames() != latency_frames_ ||
      revision == 0 || revision <= offered_revision_)
    return false;
  const auto write = pending_write_.load(std::memory_order_relaxed);
  if (write - pending_read_.load(std::memory_order_acquire) >= mailbox_capacity)
    return false;
  pending_[write % mailbox_capacity].emplace(PendingSettings{prepared, revision});
  offered_revision_ = revision;
  pending_write_.store(write + 1, std::memory_order_release);
  return true;
}

void LimiterRuntime::adopt_pending() noexcept {
  const auto read = pending_read_.load(std::memory_order_relaxed);
  const auto write = pending_write_.load(std::memory_order_acquire);
  if (read == write)
    return;
  const auto& newest = pending_[(write - 1) % mailbox_capacity];
  if (!newest)
    return;
  prepared_ = newest->prepared;
  applied_revision_.store(newest->revision, std::memory_order_release);
  pending_read_.store(write, std::memory_order_release);
}

void LimiterRuntime::reset_state() noexcept {
  std::ranges::fill(delay_, engine::StereoFrame{0.0F, 0.0F});
  delay_index_ = 0;
  peak_head_ = 0;
  peak_size_ = 0;
  frame_index_ = 0;
  gain_ = 1.0;
}

void LimiterRuntime::push_peak(double amplitude) noexcept {
  const auto capacity = peaks_.size();
  // Every queue index is below capacity, and its size is at most capacity,
  // so each sum below needs at most one subtraction to wrap.
  while (peak_size_ != 0 && frame_index_ - peaks_[peak_head_].index > prepared_.latency_frames_) {
    ++peak_head_;
    if (peak_head_ == capacity) {
      peak_head_ = 0;
    }
    --peak_size_;
  }
  while (peak_size_ != 0) {
    auto back = peak_head_ + peak_size_ - 1;
    if (back >= capacity) {
      back -= capacity;
    }
    if (peaks_[back].amplitude > amplitude) {
      break;
    }
    --peak_size_;
  }
  auto tail = peak_head_ + peak_size_;
  if (tail >= capacity) {
    tail -= capacity;
  }
  peaks_[tail] = {amplitude, frame_index_};
  ++peak_size_;
}

void LimiterRuntime::process(std::span<engine::StereoFrame> stereo,
                             engine::BlockContext context) noexcept {
  if (context.state_generation() == 0 ||
      (generation_ != 0 && context.state_generation() < generation_)) {
    std::ranges::fill(stereo, engine::StereoFrame{0.0F, 0.0F});
    guarded_frames_ += stereo.size();
    published_reduction_db_.store(0.0F, std::memory_order_release);
    return;
  }
  adopt_pending();
  if (context.state_generation() != generation_) {
    reset_state();
    generation_ = context.state_generation();
  }
  const auto capacity = delay_.size();
  double lowest_applied_gain = 1.0;
  for (auto& frame : stereo) {
    if (!std::isfinite(frame.left) || !std::isfinite(frame.right)) {
      frame = {0.0F, 0.0F};
      ++guarded_frames_;
    }
    const double peak = std::max(std::abs(static_cast<double>(frame.left)),
                                 std::abs(static_cast<double>(frame.right)));
    push_peak(peak);
    delay_[delay_index_] = frame;
    auto read_index = delay_index_ + 1;
    if (read_index == capacity) {
      read_index = 0;
    }
    auto delayed = delay_[read_index];
    delay_index_ = read_index;
    const double window_peak = peaks_[peak_head_].amplitude;
    const double target = window_peak > prepared_.ceiling_ ? prepared_.ceiling_ / window_peak : 1.0;
    gain_ =
        target < gain_ ? target : prepared_.release_ * gain_ + (1.0 - prepared_.release_) * target;
    if (delayed.left != 0.0F || delayed.right != 0.0F)
      lowest_applied_gain = std::min(lowest_applied_gain, gain_);
    const auto ceiling = prepared_.ceiling_;
    delayed.left = static_cast<float>(
        std::clamp(static_cast<double>(delayed.left) * gain_, -ceiling, ceiling));
    delayed.right = static_cast<float>(
        std::clamp(static_cast<double>(delayed.right) * gain_, -ceiling, ceiling));
    frame = delayed;
    ++frame_index_;
  }
  const double reduction = lowest_applied_gain > 0.0 && lowest_applied_gain < 1.0
                               ? -20.0 * std::log10(lowest_applied_gain)
                               : 0.0;
  published_reduction_db_.store(std::isfinite(reduction) ? static_cast<float>(reduction) : 0.0F,
                                std::memory_order_release);
}

void LimiterRuntime::reset_after_quiesce() noexcept {
  reset_state();
  generation_ = 0;
  guarded_frames_ = 0;
  pending_write_.store(0, std::memory_order_relaxed);
  pending_read_.store(0, std::memory_order_relaxed);
  applied_revision_.store(0, std::memory_order_relaxed);
  offered_revision_ = 0;
  published_reduction_db_.store(0.0F, std::memory_order_relaxed);
}

} // namespace oscillate::native_dsp
