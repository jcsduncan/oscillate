#include "oscillate/export/realtime_capture.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace oscillate::exporting {
namespace {
using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;
constexpr std::size_t drain_chunk_frames = 4096;
} // namespace

RealtimeWavCapture::RealtimeWavCapture(WavWriter writer, WavSpec spec, std::uint64_t first_sample,
                                       std::size_t ring_frames)
    : writer_(std::move(writer)), ring_(std::make_unique<engine::StereoFrame[]>(ring_frames)),
      capacity_(ring_frames), first_sample_(first_sample), expected_frames_(spec.expected_frames) {}

Result<std::unique_ptr<RealtimeWavCapture>>
RealtimeWavCapture::begin(const std::filesystem::path& target, WavSpec spec,
                          std::uint64_t first_sample, std::size_t ring_frames) {
  if (spec.expected_frames == 0 ||
      spec.expected_frames > std::numeric_limits<std::uint64_t>::max() - first_sample ||
      ring_frames == 0 || ring_frames > (1U << 20U)) {
    return Result<std::unique_ptr<RealtimeWavCapture>>::failure(
        {ErrorCode::invalid_argument, "Invalid real-time export interval or ring size"});
  }
  auto writer = WavWriter::begin(target, spec);
  if (!writer)
    return Result<std::unique_ptr<RealtimeWavCapture>>::failure(writer.error());
  return Result<std::unique_ptr<RealtimeWavCapture>>::success(std::unique_ptr<RealtimeWavCapture>(
      new RealtimeWavCapture(std::move(writer.value()), spec, first_sample, ring_frames)));
}

void RealtimeWavCapture::fail(Failure reason) noexcept {
  auto expected = Failure::none;
  (void)failure_.compare_exchange_strong(expected, reason, std::memory_order_release,
                                         std::memory_order_relaxed);
}

void RealtimeWavCapture::capture(std::span<const engine::StereoFrame> frames,
                                 engine::BlockContext block, bool rendered) noexcept {
  if (failure_.load(std::memory_order_relaxed) != Failure::none ||
      captured_.load(std::memory_order_relaxed) == expected_frames_)
    return;
  if (!rendered) {
    fail(Failure::render);
    return;
  }
  const auto progress = captured_.load(std::memory_order_relaxed);
  if (block.project_sample_position != first_sample_ + progress || block.planned_loop_wrap ||
      block.transport_generation == 0 ||
      (generation_ != 0 && generation_ != block.transport_generation)) {
    fail(Failure::discontinuity);
    return;
  }
  generation_ = block.transport_generation;
  const auto count =
      static_cast<std::size_t>(std::min<std::uint64_t>(frames.size(), expected_frames_ - progress));
  if (count == 0)
    return;
  const auto write = written_.load(std::memory_order_relaxed);
  const auto read = read_.load(std::memory_order_acquire);
  if (count > capacity_ - static_cast<std::size_t>(write - read)) {
    fail(Failure::overflow);
    return;
  }
  const auto first = std::min(count, capacity_ - static_cast<std::size_t>(write % capacity_));
  std::copy_n(frames.data(), first, ring_.get() + (write % capacity_));
  std::copy_n(frames.data() + first, count - first, ring_.get());
  written_.store(write + count, std::memory_order_release);
  captured_.store(progress + count, std::memory_order_release);
}

Result<std::size_t> RealtimeWavCapture::drain() {
  if (finished_)
    return Result<std::size_t>::failure({ErrorCode::conflict, "Capture already finished"});
  std::array<float, drain_chunk_frames * 2> samples{};
  std::size_t total = 0;
  for (;;) {
    const auto read = read_.load(std::memory_order_relaxed);
    const auto write = written_.load(std::memory_order_acquire);
    if (read == write)
      break;
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>({write - read, drain_chunk_frames,
                                 capacity_ - static_cast<std::size_t>(read % capacity_)}));
    for (std::size_t index = 0; index < count; ++index) {
      const auto& frame = ring_[(read % capacity_) + index];
      samples[index * 2] = frame.left;
      samples[index * 2 + 1] = frame.right;
    }
    auto result = writer_.write_interleaved_stereo(std::span(samples).first(count * 2));
    if (!result) {
      fail(Failure::render);
      return Result<std::size_t>::failure(result.error());
    }
    read_.store(read + count, std::memory_order_release);
    total += count;
  }
  return Result<std::size_t>::success(total);
}

Result<PublishOutcome> RealtimeWavCapture::finish(DeviceEvidence evidence) {
  if (finished_)
    return Result<PublishOutcome>::failure({ErrorCode::conflict, "Capture already finished"});
  // The caller has joined the producer before this point. Drain the final
  // committed frames without allowing a second finish or publication attempt.
  auto drained = drain();
  finished_ = true;
  if (!drained)
    return Result<PublishOutcome>::failure(drained.error());
  const auto failure = failure_.load(std::memory_order_acquire);
  if (failure != Failure::none) {
    const char* message = failure == Failure::xrun       ? "Real-time export xrun"
                          : failure == Failure::overflow ? "Real-time export ring overflow"
                          : failure == Failure::discontinuity
                              ? "Real-time export transport discontinuity"
                              : "Real-time export render failure";
    return Result<PublishOutcome>::failure({ErrorCode::unavailable, message});
  }
  if (!evidence.xruns_before || !evidence.xruns_after) {
    return Result<PublishOutcome>::failure(
        {ErrorCode::unavailable, "Device cannot verify an xrun-free real-time export"});
  }
  if (*evidence.xruns_after < *evidence.xruns_before ||
      *evidence.xruns_after != *evidence.xruns_before || evidence.render_failures != 0 ||
      evidence.source_underruns != 0 || evidence.callback_over_period_blocks != 0) {
    return Result<PublishOutcome>::failure(
        {ErrorCode::unavailable, "Real-time export had an xrun or incomplete audio block"});
  }
  if (captured_.load(std::memory_order_acquire) != expected_frames_ ||
      writer_.frames_written() != expected_frames_) {
    return Result<PublishOutcome>::failure(
        {ErrorCode::unavailable, "Real-time export ended before the requested interval"});
  }
  return writer_.finish();
}

} // namespace oscillate::exporting
