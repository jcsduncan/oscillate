#include <oscillate/application/prefetch_control.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace oscillate::application {
namespace {

using foundation::ErrorCode;

[[nodiscard]] foundation::Result<std::shared_ptr<PrefetchFillJob>> fail(ErrorCode code,
                                                                        const char* message) {
  return foundation::Result<std::shared_ptr<PrefetchFillJob>>::failure({code, message});
}

} // namespace

PrefetchFillJob::PrefetchFillJob(std::shared_ptr<media::PrefetchRing> ring,
                                 std::uint64_t generation, std::uint64_t first,
                                 std::uint64_t end) noexcept
    : ring_(std::move(ring)), generation_(generation), end_(end), next_(first) {}

foundation::Result<std::size_t> PrefetchFillJob::fill_step(media::WavReader& source,
                                                           std::uint32_t project_rate,
                                                           std::span<media::StereoFrame> scratch) {
  if (next_ == end_) {
    complete_.store(true, std::memory_order_release);
    return foundation::Result<std::size_t>::success(0);
  }
  if (scratch.empty()) {
    return foundation::Result<std::size_t>::failure(
        {ErrorCode::invalid_argument, "prefetch scratch is empty"});
  }
  if (next_ >= source.frame_count()) {
    return foundation::Result<std::size_t>::failure(
        {ErrorCode::corrupt_data, "WAV ended before clip source range"});
  }
  const auto count =
      static_cast<std::size_t>(std::min<std::uint64_t>(scratch.size(), end_ - next_));
  auto result = media::fill_from_wav(source, project_rate, *ring_, {generation_, next_},
                                     scratch.first(count));
  if (!result) {
    return result;
  }
  next_ += result.value();
  published_.fetch_add(result.value(), std::memory_order_release);
  if (next_ == end_) {
    complete_.store(true, std::memory_order_release);
  }
  return result;
}

foundation::Result<std::size_t> PrefetchFillJob::fill_step(media::AudioReader& source,
                                                           std::uint32_t project_rate,
                                                           std::span<media::StereoFrame> scratch) {
  if (next_ == end_) {
    complete_.store(true, std::memory_order_release);
    return foundation::Result<std::size_t>::success(0);
  }
  if (scratch.empty()) {
    return foundation::Result<std::size_t>::failure(
        {ErrorCode::invalid_argument, "prefetch scratch is empty"});
  }
  if (next_ >= source.frame_count()) {
    return foundation::Result<std::size_t>::failure(
        {ErrorCode::corrupt_data, "Audio source ended before clip source range"});
  }
  const auto count =
      static_cast<std::size_t>(std::min<std::uint64_t>(scratch.size(), end_ - next_));
  auto result = media::fill_from_audio(source, project_rate, *ring_, {generation_, next_},
                                       scratch.first(count));
  if (!result) {
    return result;
  }
  next_ += result.value();
  published_.fetch_add(result.value(), std::memory_order_release);
  if (next_ == end_) {
    complete_.store(true, std::memory_order_release);
  }
  return result;
}

PrefetchControl::PrefetchControl(std::shared_ptr<media::PrefetchRing> ring,
                                 std::shared_ptr<PlaybackReadiness> readiness, PrefetchClock clock)
    : ring_(std::move(ring)), readiness_(std::move(readiness)), clock_(clock) {
  if (!ring_ || !readiness_ || clock_.now == nullptr) {
    throw std::invalid_argument("prefetch control requires ring, readiness and clock");
  }
}

foundation::Result<std::shared_ptr<PrefetchFillJob>>
PrefetchControl::begin(std::uint64_t generation, std::uint64_t first, std::uint64_t end,
                       PrimingPolicy policy) {
  if (generation == 0 || (current_ && generation <= current_->generation()) ||
      (next_loop_ && generation <= next_loop_->generation()) || first >= end ||
      end - first > std::numeric_limits<std::size_t>::max() || policy.minimum_frames == 0 ||
      policy.minimum_frames > ring_->capacity() || policy.minimum_frames > end - first) {
    return fail(ErrorCode::invalid_argument, "invalid prefetch generation or source range");
  }
  const auto now = clock_.now(clock_.context);
  if (policy.timeout_ticks > std::numeric_limits<std::uint64_t>::max() - now) {
    return fail(ErrorCode::invalid_argument, "prefetch deadline overflow");
  }
  // Caller has quiesced the callback and old producer before entering here.
  readiness_->set_steady(false);
  ring_->discard_pending();
  next_loop_.reset();
  current_ = std::shared_ptr<PrefetchFillJob>(new PrefetchFillJob(ring_, generation, first, end));
  deadline_ = now + policy.timeout_ticks;
  minimum_frames_ = policy.minimum_frames;
  status_ = PrimingStatus::priming;
  return foundation::Result<std::shared_ptr<PrefetchFillJob>>::success(current_);
}

foundation::Result<std::shared_ptr<PrefetchFillJob>>
PrefetchControl::begin_start(std::uint64_t generation, std::uint64_t first, std::uint64_t end,
                             PrimingPolicy policy) {
  return begin(generation, first, end, policy);
}

foundation::Result<std::shared_ptr<PrefetchFillJob>>
PrefetchControl::begin_seek(std::uint64_t generation, std::uint64_t first, std::uint64_t end,
                            PrimingPolicy policy) {
  return begin(generation, first, end, policy);
}

PrimingStatus PrefetchControl::poll() noexcept {
  if (status_ != PrimingStatus::priming) {
    return status_;
  }
  if (current_->published_frames() >= minimum_frames_) {
    readiness_->set_steady(true);
    status_ = PrimingStatus::ready;
  } else if (clock_.now(clock_.context) >= deadline_) {
    status_ = PrimingStatus::timed_out;
  }
  return status_;
}

foundation::Result<std::shared_ptr<PrefetchFillJob>>
PrefetchControl::plan_loop_wrap(std::uint64_t next_generation, std::uint64_t first,
                                std::uint64_t end, std::size_t minimum_frames) {
  if (status_ != PrimingStatus::ready || !current_->complete() || next_loop_ ||
      next_generation <= current_->generation() || first >= end ||
      end - first > std::numeric_limits<std::size_t>::max() || minimum_frames == 0 ||
      minimum_frames > ring_->capacity() || minimum_frames > end - first) {
    return fail(ErrorCode::invalid_argument, "invalid planned loop prefetch");
  }
  next_loop_ =
      std::shared_ptr<PrefetchFillJob>(new PrefetchFillJob(ring_, next_generation, first, end));
  loop_minimum_frames_ = minimum_frames;
  return foundation::Result<std::shared_ptr<PrefetchFillJob>>::success(next_loop_);
}

bool PrefetchControl::loop_ready() const noexcept {
  return next_loop_ && next_loop_->published_frames() >= loop_minimum_frames_;
}

bool PrefetchControl::commit_loop_wrap() noexcept {
  if (!loop_ready()) {
    return false;
  }
  current_ = std::move(next_loop_);
  loop_minimum_frames_ = 0;
  return true;
}

} // namespace oscillate::application
