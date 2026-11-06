#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/media/prefetch.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/warp_dsp/rate_view.hpp"
#include "oscillate/warp_dsp/source_view.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <utility>

namespace oscillate::processing {

enum class SourceReadStatus : std::uint8_t {
  ok,
  invalid_request,
  cancelled,
  decoder_error,
  short_read,
  nonfinite_sample
};

// Worker-only adapter. read() may perform I/O or allocate through the
// supplied decoder and must never run in the audio callback. Each underlying
// call requests at most read_chunk_frames;
// a failed call silences the whole requested planar output and latches its
// status until the next read. The reader and view belong to one worker.
class WorkerSourceView final : public warp_dsp::SourceView {
public:
  using ReadAt =
      std::function<foundation::Result<std::size_t>(std::uint64_t, std::span<media::StereoFrame>)>;
  static constexpr std::size_t read_chunk_frames = 1'024;
  static constexpr std::size_t max_request_frames = 1'048'576;

  // The caller must bind a verified, same-rate reader. This low-level seam
  // also permits deterministic short-read/fault injection in contract tests.
  [[nodiscard]] static foundation::Result<std::unique_ptr<WorkerSourceView>>
  bind(std::uint64_t frames, ReadAt read_at, std::stop_token stop = {});

  [[nodiscard]] std::uint64_t frames() const noexcept override { return frames_; }
  [[nodiscard]] bool read(std::int64_t first, std::span<float> left,
                          std::span<float> right) noexcept override;
  [[nodiscard]] SourceReadStatus status() const noexcept { return status_; }
  // Meaningful only after status() reports a failure.
  [[nodiscard]] foundation::ErrorCode error_code() const noexcept { return error_code_; }

private:
  WorkerSourceView(std::uint64_t frames, ReadAt read_at, std::stop_token stop)
      : frames_(frames), read_at_(std::move(read_at)), stop_(std::move(stop)) {}

  std::uint64_t frames_;
  ReadAt read_at_;
  std::stop_token stop_;
  SourceReadStatus status_ = SourceReadStatus::ok;
  foundation::ErrorCode error_code_ = foundation::ErrorCode::internal;
};

// Opens an exact saved managed source through ProjectStore verification, then
// binds its pinned AudioReader to a worker view at the source's own rate.
// Rejects rate mismatches; ProjectRateSourceView is the converting path.
[[nodiscard]] foundation::Result<std::unique_ptr<WorkerSourceView>>
open_verified_managed_source_view(const std::filesystem::path& project_root,
                                  const project::SourceRecord& source, std::uint32_t project_rate,
                                  std::stop_token stop = {});

// Worker-only project-rate view of a verified managed source (ADR-0026 point
// 6): the verified source-rate decoder wrapped by the in-house exact-rational
// converter, which bypasses itself at equal rates. Pair it with
// make_warp_stream_map(), whose source positions use the same coordinates.
// Read failures silence the request and report through status().
class ProjectRateSourceView final : public warp_dsp::SourceView {
public:
  [[nodiscard]] static foundation::Result<std::unique_ptr<ProjectRateSourceView>>
  open(const std::filesystem::path& project_root, const project::SourceRecord& source,
       std::uint32_t project_rate, std::stop_token stop = {});
  // Binds a descriptor the caller already verified for `source` (for example
  // ProjectStore::open_managed_source), so the view reads exactly the bytes
  // that were verified, never a file reopened by path. The descriptor is
  // duplicated, not adopted: the caller still closes it. Decoded metadata
  // must match the record.
  [[nodiscard]] static foundation::Result<std::unique_ptr<ProjectRateSourceView>>
  from_descriptor(int verified_fd, const project::SourceRecord& source, std::uint32_t project_rate);
  // Low-level seam for fault injection: wraps an already bound worker view.
  [[nodiscard]] static foundation::Result<std::unique_ptr<ProjectRateSourceView>>
  wrap(std::unique_ptr<WorkerSourceView> source, std::uint32_t source_rate,
       std::uint32_t project_rate);

  [[nodiscard]] std::uint64_t frames() const noexcept override { return converted_->frames(); }
  [[nodiscard]] bool read(std::int64_t first, std::span<float> left,
                          std::span<float> right) noexcept override {
    return converted_->read(first, left, right);
  }
  [[nodiscard]] SourceReadStatus status() const noexcept { return source_->status(); }
  [[nodiscard]] foundation::ErrorCode error_code() const noexcept { return source_->error_code(); }
  [[nodiscard]] bool converts() const noexcept { return !converted_->bypass(); }

private:
  explicit ProjectRateSourceView(std::unique_ptr<WorkerSourceView> source) noexcept
      : source_(std::move(source)) {}

  std::unique_ptr<WorkerSourceView> source_;
  std::unique_ptr<warp_dsp::RateConvertedSourceView> converted_;
};

} // namespace oscillate::processing
