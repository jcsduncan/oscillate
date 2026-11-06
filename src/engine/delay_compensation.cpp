#include "oscillate/engine/delay_compensation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace oscillate::engine {
namespace {
using Result = foundation::Result<std::unique_ptr<DelayCompensationRuntime>>;

[[nodiscard]] Result fail(foundation::ErrorCode code, const char* message) {
  return Result::failure({code, message});
}

[[nodiscard]] StereoFrame finite_frame(StereoFrame frame) noexcept {
  return {std::isfinite(frame.left) ? frame.left : 0.0F,
          std::isfinite(frame.right) ? frame.right : 0.0F};
}
} // namespace

DelayCompensationRuntime::DelayCompensationRuntime(DelayCompensationConfig config,
                                                   std::vector<StereoFrame> history) noexcept
    : config_(config), history_(std::move(history)) {}

Result DelayCompensationRuntime::prepare(DelayCompensationConfig config) {
  if (config.route_id == 0) {
    return fail(foundation::ErrorCode::invalid_argument, "delay route ID must be nonzero");
  }
  if (config.delay_frames > config.max_compensation_frames) {
    return fail(foundation::ErrorCode::resource_exhausted,
                "required delay exceeds caller-supplied compensation budget");
  }
  const auto slots = static_cast<std::uint64_t>(config.max_compensation_frames) + 1;
  if (slots * sizeof(StereoFrame) > config.max_history_bytes) {
    return fail(foundation::ErrorCode::resource_exhausted,
                "delay history exceeds caller-supplied memory budget");
  }
  try {
    std::vector<StereoFrame> history(static_cast<std::size_t>(config.max_compensation_frames) + 1);
    return Result::success(std::unique_ptr<DelayCompensationRuntime>(
        new DelayCompensationRuntime(config, std::move(history))));
  } catch (const std::bad_alloc&) {
    return fail(foundation::ErrorCode::resource_exhausted,
                "delay history allocation failed; hold the old graph");
  }
}

Result
DelayCompensationRuntime::prepare_transition_after_quiesce(const DelayCompensationRuntime& prior,
                                                           DelayCompensationConfig next,
                                                           BlockContext switch_boundary) {
  if (next.route_id != prior.route_id()) {
    return fail(foundation::ErrorCode::conflict,
                "delay history route ID changed; hold the old graph");
  }
  if (next.delay_frames > next.max_compensation_frames) {
    return fail(foundation::ErrorCode::resource_exhausted,
                "required delay exceeds caller-supplied compensation budget");
  }
  if (!prior.has_position_ || switch_boundary.state_generation() == 0 ||
      prior.generation_ != switch_boundary.state_generation() ||
      prior.next_position_ != switch_boundary.project_sample_position) {
    return fail(foundation::ErrorCode::unavailable,
                "delay history boundary is not quiescent and current; hold the old graph");
  }
  if (next.delay_frames > prior.history_frames_) {
    return fail(foundation::ErrorCode::unavailable,
                "delay history is too short to prime the new latency; hold the old graph");
  }
  auto prepared = prepare(next);
  if (!prepared) {
    return prepared;
  }
  auto& replacement = *prepared.value();
  const auto count = std::min(prior.history_frames_, next.max_compensation_frames);
  const auto old_size = prior.history_.size();
  for (std::size_t index = 0; index < count; ++index) {
    replacement.history_[index] =
        prior.history_[(prior.cursor_ + old_size - count + index) % old_size];
  }
  replacement.cursor_ = count;
  replacement.history_frames_ = count;
  replacement.generation_ = prior.generation_;
  replacement.next_position_ = prior.next_position_;
  replacement.has_position_ = true;
  return prepared;
}

void DelayCompensationRuntime::adopt(const DelayCompensationRuntime& prior) noexcept {
  if (!prior.has_position_)
    return;
  const auto count = std::min({prior.history_frames_, config_.max_compensation_frames,
                               static_cast<std::uint32_t>(prior.history_.size())});
  const auto old_size = prior.history_.size();
  for (std::size_t index = 0; index < count; ++index)
    history_[index] = prior.history_[(prior.cursor_ + old_size - count + index) % old_size];
  cursor_ = count % history_.size();
  history_frames_ = count;
  generation_ = prior.generation_;
  next_position_ = prior.next_position_;
  has_position_ = true;
  // A delay that changed moves from its old reading to its new one over a
  // short crossfade rather than jumping (ADR-0049).
  crossfade_from_ = prior.config_.delay_frames;
  crossfade_left_ = prior.config_.delay_frames == config_.delay_frames ? 0 : crossfade_frames;
}

void DelayCompensationRuntime::prime(std::span<const StereoFrame> older,
                                     std::span<const StereoFrame> newer,
                                     BlockContext boundary) noexcept {
  const auto total = older.size() + newer.size();
  const auto count = std::min<std::size_t>(total, config_.max_compensation_frames);
  const auto skip = total - count;
  for (std::size_t index = 0; index < count; ++index) {
    const auto at = skip + index;
    history_[index] = finite_frame(at < older.size() ? older[at] : newer[at - older.size()]);
  }
  cursor_ = count % history_.size();
  history_frames_ = static_cast<std::uint32_t>(count);
  generation_ = boundary.state_generation();
  next_position_ = boundary.project_sample_position;
  has_position_ = true;
}

void DelayCompensationRuntime::shift(std::int64_t latency_change,
                                     std::span<const StereoFrame> appended) noexcept {
  // The shifted history keeps time exactly: no crossfade is needed.
  crossfade_left_ = 0;
  const auto size = history_.size();
  if (latency_change > 0) {
    const auto dropped =
        std::min<std::uint64_t>(static_cast<std::uint64_t>(latency_change), history_frames_);
    cursor_ = (cursor_ + size - static_cast<std::size_t>(dropped)) % size;
    history_frames_ -= static_cast<std::uint32_t>(dropped);
    return;
  }
  for (const auto frame : appended) {
    history_[cursor_] = finite_frame(frame);
    cursor_ = cursor_ + 1 == size ? 0 : cursor_ + 1;
    if (history_frames_ < config_.max_compensation_frames)
      ++history_frames_;
  }
}

void DelayCompensationRuntime::guard(std::span<StereoFrame> stereo) noexcept {
  std::ranges::fill(stereo, StereoFrame{});
  guarded_frames_ += std::min<std::uint64_t>(
      stereo.size(), std::numeric_limits<std::uint64_t>::max() - guarded_frames_);
}

void DelayCompensationRuntime::process(std::span<StereoFrame> stereo,
                                       BlockContext context) noexcept {
  if (stereo.empty()) {
    return;
  }
  if (context.state_generation() == 0 ||
      (generation_ != 0 && context.state_generation() < generation_) ||
      stereo.size() > std::numeric_limits<std::uint64_t>::max() - context.project_sample_position) {
    guard(stereo);
    return;
  }
  if (context.state_generation() != generation_) {
    generation_ = context.state_generation();
    history_frames_ = 0;
    cursor_ = 0;
    next_position_ = context.project_sample_position;
    has_position_ = true;
  } else if (context.planned_loop_wrap) {
    next_position_ = context.project_sample_position;
  } else if (!has_position_ || context.project_sample_position != next_position_) {
    guard(stereo);
    return;
  }
  const auto size = history_.size();
  const auto delay = static_cast<std::size_t>(config_.delay_frames);
  const auto read = [&](std::size_t frames) noexcept {
    return history_frames_ >= frames ? history_[(cursor_ + size - frames) % size] : StereoFrame{};
  };
  for (auto& frame : stereo) {
    const bool invalid = !std::isfinite(frame.left) || !std::isfinite(frame.right);
    history_[cursor_] = finite_frame(frame);
    frame = read(delay);
    if (crossfade_left_ != 0) {
      // A delay changed at a live switch: blend from the old reading.
      const auto from = read(crossfade_from_);
      const auto progress =
          static_cast<float>(crossfade_frames - crossfade_left_ + 1) / crossfade_frames;
      frame = {from.left + (frame.left - from.left) * progress,
               from.right + (frame.right - from.right) * progress};
      --crossfade_left_;
    }
    cursor_ = cursor_ + 1 == size ? 0 : cursor_ + 1;
    if (history_frames_ < config_.max_compensation_frames) {
      ++history_frames_;
    }
    if (invalid && guarded_frames_ != std::numeric_limits<std::uint64_t>::max()) {
      ++guarded_frames_;
    }
  }
  next_position_ += stereo.size();
}

} // namespace oscillate::engine
