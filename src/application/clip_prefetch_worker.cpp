#include <oscillate/application/clip_prefetch_worker.hpp>

#include <array>
#include <chrono>
#include <exception>
#include <system_error>
#include <thread>
#include <utility>

namespace oscillate::application {
namespace {
using foundation::ErrorCode;
using foundation::Result;
using namespace std::chrono_literals;
} // namespace

ClipPrefetchWorker::ClipPrefetchWorker(std::shared_ptr<media::PrefetchRing> ring,
                                       std::shared_ptr<PlaybackReadiness> readiness,
                                       PrefetchClock clock)
    : ring_(std::move(ring)), readiness_(std::move(readiness)), control_(ring_, readiness_, clock) {
}

ClipPrefetchWorker::~ClipPrefetchWorker() {
  request_stop();
  if (thread_.joinable()) {
    thread_.join();
  }
}

Result<std::unique_ptr<ClipPrefetchWorker>>
ClipPrefetchWorker::start(media::WavReader reader, std::shared_ptr<media::PrefetchRing> ring,
                          std::shared_ptr<PlaybackReadiness> readiness, ClipPrefetchRequest request,
                          PrefetchClock clock) {
  return start(media::AudioReader::from_wav(std::move(reader)), std::move(ring),
               std::move(readiness), request, clock);
}

Result<std::unique_ptr<ClipPrefetchWorker>>
ClipPrefetchWorker::start(media::AudioReader reader, std::shared_ptr<media::PrefetchRing> ring,
                          std::shared_ptr<PlaybackReadiness> readiness, ClipPrefetchRequest request,
                          PrefetchClock clock) {
  if (!ring || !readiness || !clock.now || request.project_rate == 0 ||
      reader.sample_rate() != request.project_rate ||
      request.end_source_frame > reader.frame_count()) {
    return Result<std::unique_ptr<ClipPrefetchWorker>>::failure(
        {ErrorCode::invalid_argument, "invalid clip prefetch source or rate"});
  }
  auto worker = std::unique_ptr<ClipPrefetchWorker>(
      new ClipPrefetchWorker(std::move(ring), std::move(readiness), clock));
  auto job = worker->control_.begin_start(request.generation, request.first_source_frame,
                                          request.end_source_frame, request.priming);
  if (!job) {
    return Result<std::unique_ptr<ClipPrefetchWorker>>::failure(job.error());
  }
  worker->job_ = std::move(job).value();
  try {
    worker->thread_ = std::thread([worker_ptr = worker.get(), source = std::move(reader),
                                   project_rate = request.project_rate]() mutable {
      worker_ptr->run(std::move(source), project_rate);
    });
  } catch (const std::system_error&) {
    return Result<std::unique_ptr<ClipPrefetchWorker>>::failure(
        {ErrorCode::resource_exhausted, "cannot start clip prefetch worker"});
  }
  return Result<std::unique_ptr<ClipPrefetchWorker>>::success(std::move(worker));
}

void ClipPrefetchWorker::run(media::AudioReader reader, std::uint32_t project_rate) noexcept {
  try {
    std::array<media::StereoFrame, 4096> scratch{};
    while (!stop_.load(std::memory_order_acquire)) {
      if (job_->complete()) {
        state_.store(ClipPrefetchState::complete, std::memory_order_release);
        break;
      }
      if (ring_->free_slots() == 0) {
        std::this_thread::sleep_for(1ms);
        continue;
      }
      auto result = job_->fill_step(reader, project_rate, scratch);
      if (!result) {
        error_ = result.error();
        readiness_->set_steady(false);
        state_.store(ClipPrefetchState::failed, std::memory_order_release);
        break;
      }
      if (result.value() == 0 && !job_->complete()) {
        error_ = {ErrorCode::corrupt_data, "clip prefetch made no progress"};
        readiness_->set_steady(false);
        state_.store(ClipPrefetchState::failed, std::memory_order_release);
        break;
      }
    }
  } catch (const std::exception&) {
    error_ = {ErrorCode::internal, "clip prefetch worker failed unexpectedly"};
    readiness_->set_steady(false);
    state_.store(ClipPrefetchState::failed, std::memory_order_release);
  } catch (...) {
    error_ = {ErrorCode::internal, "clip prefetch worker failed unexpectedly"};
    readiness_->set_steady(false);
    state_.store(ClipPrefetchState::failed, std::memory_order_release);
  }
  if (state_.load(std::memory_order_acquire) == ClipPrefetchState::filling) {
    state_.store(ClipPrefetchState::stopped, std::memory_order_release);
  }
  finished_.store(true, std::memory_order_release);
}

Result<PrimingStatus> ClipPrefetchWorker::poll_priming() {
  const auto current = state_.load(std::memory_order_acquire);
  if (current == ClipPrefetchState::failed) {
    return Result<PrimingStatus>::failure(error_);
  }
  if (current == ClipPrefetchState::stopped) {
    return Result<PrimingStatus>::failure(
        {ErrorCode::unavailable, "clip prefetch stopped before priming"});
  }
  const auto priming = control_.poll();
  if (state_.load(std::memory_order_acquire) == ClipPrefetchState::failed) {
    readiness_->set_steady(false);
    return Result<PrimingStatus>::failure(error_);
  }
  if (priming == PrimingStatus::timed_out) {
    request_stop();
  }
  return Result<PrimingStatus>::success(priming);
}

ClipPrefetchStatus ClipPrefetchWorker::status() const noexcept {
  return {state_.load(std::memory_order_acquire), job_->published_frames()};
}

std::optional<foundation::Error> ClipPrefetchWorker::failure() const {
  if (state_.load(std::memory_order_acquire) == ClipPrefetchState::failed) {
    return error_;
  }
  return std::nullopt;
}

void ClipPrefetchWorker::request_stop() noexcept {
  readiness_->set_steady(false);
  stop_.store(true, std::memory_order_release);
}

bool ClipPrefetchWorker::finished() const noexcept {
  return finished_.load(std::memory_order_acquire);
}

bool ClipPrefetchWorker::join_finished() noexcept {
  if (!finished()) {
    return false;
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  return true;
}

} // namespace oscillate::application
