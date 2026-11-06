#include "oscillate/processing/verified_source_view.hpp"

#include "oscillate/media/audio_reader.hpp"
#include "oscillate/persistence/store.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <ranges>
#include <unistd.h>
#include <utility>

namespace oscillate::processing {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

void silence(std::span<float> left, std::span<float> right) noexcept {
  std::ranges::fill(left, 0.0F);
  std::ranges::fill(right, 0.0F);
}
} // namespace

Result<std::unique_ptr<WorkerSourceView>>
WorkerSourceView::bind(std::uint64_t frames, ReadAt read_at, std::stop_token stop) {
  if (frames > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) || !read_at) {
    return Result<std::unique_ptr<WorkerSourceView>>::failure(
        {ErrorCode::invalid_argument, "Invalid worker source reader"});
  }
  return Result<std::unique_ptr<WorkerSourceView>>::success(std::unique_ptr<WorkerSourceView>(
      new WorkerSourceView(frames, std::move(read_at), std::move(stop))));
}

bool WorkerSourceView::read(std::int64_t first, std::span<float> left,
                            std::span<float> right) noexcept {
  const auto fail = [this, left, right](SourceReadStatus status, ErrorCode code) noexcept {
    status_ = status;
    error_code_ = code;
    silence(left, right);
    return false;
  };
  if (left.size() != right.size() || left.size() > max_request_frames) {
    return fail(SourceReadStatus::invalid_request, ErrorCode::invalid_argument);
  }
  if (stop_.stop_requested()) {
    return fail(SourceReadStatus::cancelled, ErrorCode::cancelled);
  }
  silence(left, right);
  status_ = SourceReadStatus::ok;
  if (left.empty()) {
    return true;
  }

  const auto negative = first < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(first) : 0U;
  const auto prefix = static_cast<std::size_t>(std::min<std::uint64_t>(left.size(), negative));
  const auto source_first = first < 0 ? 0U : static_cast<std::uint64_t>(first);
  if (prefix == left.size() || source_first >= frames_) {
    return true;
  }
  const auto count = static_cast<std::size_t>(
      std::min<std::uint64_t>(left.size() - prefix, frames_ - source_first));
  std::array<media::StereoFrame, read_chunk_frames> decoded{};
  for (std::size_t copied = 0; copied < count;) {
    if (stop_.stop_requested()) {
      return fail(SourceReadStatus::cancelled, ErrorCode::cancelled);
    }
    const auto chunk = std::min(read_chunk_frames, count - copied);
    try {
      auto result = read_at_(source_first + copied, std::span(decoded).first(chunk));
      if (!result) {
        return fail(SourceReadStatus::decoder_error, result.error().code);
      }
      if (result.value() != chunk) {
        return fail(SourceReadStatus::short_read, ErrorCode::io_failure);
      }
    } catch (const std::bad_alloc&) {
      return fail(SourceReadStatus::decoder_error, ErrorCode::resource_exhausted);
    } catch (const std::exception&) {
      return fail(SourceReadStatus::decoder_error, ErrorCode::internal);
    } catch (...) {
      return fail(SourceReadStatus::decoder_error, ErrorCode::internal);
    }
    if (stop_.stop_requested()) {
      return fail(SourceReadStatus::cancelled, ErrorCode::cancelled);
    }
    for (std::size_t index = 0; index < chunk; ++index) {
      if (!std::isfinite(decoded[index].left) || !std::isfinite(decoded[index].right)) {
        return fail(SourceReadStatus::nonfinite_sample, ErrorCode::corrupt_data);
      }
      left[prefix + copied + index] = decoded[index].left;
      right[prefix + copied + index] = decoded[index].right;
    }
    copied += chunk;
  }
  return true;
}

namespace {

// Verified managed source at its own rate; the callers check rate policy.
Result<std::unique_ptr<WorkerSourceView>>
open_verified_source_rate_view(const std::filesystem::path& project_root,
                               const project::SourceRecord& source, std::stop_token stop) {
  if (!project_root.is_absolute() || project_root != project_root.lexically_normal()) {
    return Result<std::unique_ptr<WorkerSourceView>>::failure(
        {ErrorCode::invalid_argument, "Warp source needs a normalized absolute project root"});
  }
  if (source.external_link) {
    return Result<std::unique_ptr<WorkerSourceView>>::failure(
        {ErrorCode::unsupported,
         "Warp processing needs a managed source; collect the linked file into the project"});
  }
  if (stop.stop_requested()) {
    return Result<std::unique_ptr<WorkerSourceView>>::failure(
        {ErrorCode::cancelled, "Warp source open cancelled"});
  }
  auto store = persistence::ProjectStore::open_reader(project_root);
  if (!store) {
    return Result<std::unique_ptr<WorkerSourceView>>::failure(store.error());
  }
  auto descriptor = store.value().open_verified_source(source, stop);
  if (!descriptor) {
    return Result<std::unique_ptr<WorkerSourceView>>::failure(descriptor.error());
  }
  auto audio = media::AudioReader::open_fd(descriptor.value(), source);
  (void)::close(descriptor.value());
  if (!audio) {
    return Result<std::unique_ptr<WorkerSourceView>>::failure(audio.error());
  }
  if (stop.stop_requested()) {
    return Result<std::unique_ptr<WorkerSourceView>>::failure(
        {ErrorCode::cancelled, "Warp source open cancelled"});
  }
  auto pinned = std::make_shared<media::AudioReader>(std::move(audio).value());
  WorkerSourceView::ReadAt read_at = [pinned](std::uint64_t first,
                                              std::span<media::StereoFrame> output) {
    return pinned->read_at(first, output);
  };
  return WorkerSourceView::bind(source.frames, std::move(read_at), std::move(stop));
}

} // namespace

Result<std::unique_ptr<WorkerSourceView>>
open_verified_managed_source_view(const std::filesystem::path& project_root,
                                  const project::SourceRecord& source, std::uint32_t project_rate,
                                  std::stop_token stop) {
  if (source.sample_rate != project_rate || project_rate == 0) {
    return Result<std::unique_ptr<WorkerSourceView>>::failure(
        {ErrorCode::invalid_argument, "Warp source must be managed, same-rate and project-bound"});
  }
  return open_verified_source_rate_view(project_root, source, std::move(stop));
}

Result<std::unique_ptr<ProjectRateSourceView>>
ProjectRateSourceView::wrap(std::unique_ptr<WorkerSourceView> source, std::uint32_t source_rate,
                            std::uint32_t project_rate) {
  using Opened = Result<std::unique_ptr<ProjectRateSourceView>>;
  if (!source)
    return Opened::failure({ErrorCode::invalid_argument, "Missing worker source view"});
  try {
    auto view =
        std::unique_ptr<ProjectRateSourceView>(new ProjectRateSourceView(std::move(source)));
    auto converted =
        warp_dsp::RateConvertedSourceView::create(*view->source_, source_rate, project_rate);
    if (!converted)
      return Opened::failure(converted.error());
    view->converted_ = std::move(converted).value();
    return Opened::success(std::move(view));
  } catch (const std::bad_alloc&) {
    return Opened::failure({ErrorCode::resource_exhausted, "Project-rate view allocation failed"});
  }
}

Result<std::unique_ptr<ProjectRateSourceView>>
ProjectRateSourceView::from_descriptor(int verified_fd, const project::SourceRecord& source,
                                       std::uint32_t project_rate) {
  using Opened = Result<std::unique_ptr<ProjectRateSourceView>>;
  if (source.external_link)
    return Opened::failure(
        {ErrorCode::unsupported,
         "Warp processing needs a managed source; collect the linked file into the project"});
  if (auto supported =
          warp_dsp::RateConvertedSourceView::supports(source.sample_rate, project_rate);
      !supported)
    return Opened::failure(supported.error());
  auto audio = media::AudioReader::open_fd(verified_fd, source);
  if (!audio)
    return Opened::failure(audio.error());
  if (audio.value().sample_rate() != source.sample_rate ||
      audio.value().channels() != source.channels || audio.value().frame_count() != source.frames)
    return Opened::failure({ErrorCode::corrupt_data, "Verified warp source metadata changed"});
  auto pinned = std::make_shared<media::AudioReader>(std::move(audio).value());
  WorkerSourceView::ReadAt read_at = [pinned](std::uint64_t first,
                                              std::span<media::StereoFrame> output) {
    return pinned->read_at(first, output);
  };
  auto bound = WorkerSourceView::bind(source.frames, std::move(read_at));
  if (!bound)
    return Opened::failure(bound.error());
  return wrap(std::move(bound).value(), source.sample_rate, project_rate);
}

Result<std::unique_ptr<ProjectRateSourceView>>
ProjectRateSourceView::open(const std::filesystem::path& project_root,
                            const project::SourceRecord& source, std::uint32_t project_rate,
                            std::stop_token stop) {
  using Opened = Result<std::unique_ptr<ProjectRateSourceView>>;
  // Check the converter's bounds before any file is opened or verified.
  if (auto supported =
          warp_dsp::RateConvertedSourceView::supports(source.sample_rate, project_rate);
      !supported)
    return Opened::failure(supported.error());
  if (auto frames = warp_dsp::project_rate_frames(source.frames, source.sample_rate, project_rate);
      !frames)
    return Opened::failure(frames.error());
  auto opened = open_verified_source_rate_view(project_root, source, std::move(stop));
  if (!opened)
    return Opened::failure(opened.error());
  return wrap(std::move(opened).value(), source.sample_rate, project_rate);
}

} // namespace oscillate::processing
