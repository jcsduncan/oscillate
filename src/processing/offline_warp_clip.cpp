#include "oscillate/processing/offline_warp_clip.hpp"

#include "oscillate/processing/warp_stream_map.hpp"

#include <algorithm>
#include <cmath>
#include <new>
#include <string>
#include <utility>

namespace oscillate::processing {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

[[nodiscard]] foundation::Error failure(const ProjectRateSourceView& view, std::string_view what) {
  if (view.status() != SourceReadStatus::ok)
    return {view.error_code(), "Warp source read failed during export " + std::string(what)};
  return {ErrorCode::io_failure, "Warp stream stopped during export " + std::string(what)};
}
} // namespace

OfflineWarpClip::~OfflineWarpClip() = default;

Result<std::unique_ptr<OfflineWarpClip>> OfflineWarpClip::open(int verified_fd,
                                                               const WarpRenderRequest& request) {
  using Opened = Result<std::unique_ptr<OfflineWarpClip>>;
  if (!warp_dsp::valid_quality_tier(request.quality))
    return Opened::failure({ErrorCode::invalid_argument, "Invalid export warp quality"});
  auto view =
      ProjectRateSourceView::from_descriptor(verified_fd, request.source, request.project_rate);
  if (!view)
    return Opened::failure(view.error());
  auto map = make_warp_stream_map(request.plan, request.source.sample_rate, request.project_rate);
  if (!map)
    return Opened::failure(map.error());
  try {
    auto clip =
        std::unique_ptr<OfflineWarpClip>(new OfflineWarpClip(request.plan.warp.output_frames()));
    clip->view_ = std::move(view).value();
    clip->map_ = std::make_unique<warp_dsp::WarpTimeMap>(std::move(map).value());
    auto analysis = analyse_warp_source(request, *clip->view_);
    if (!analysis) {
      if (clip->view_->status() != SourceReadStatus::ok)
        return Opened::failure(failure(*clip->view_, "analysis"));
      return Opened::failure(analysis.error());
    }
    auto stream = create_warp_stream(request, analysis.value(), *clip->map_, request.quality,
                                     static_cast<std::uint32_t>(render_chunk_frames));
    if (!stream)
      return Opened::failure(stream.error());
    clip->stream_ = std::move(stream).value();
    const auto started = clip->stream_->start(*clip->map_, *clip->view_, 0);
    if (started == warp_dsp::StreamStatus::unsupported_map)
      return Opened::failure(
          {ErrorCode::unsupported, "Warp markers exceed the speed range of " +
                                       std::string(warp_algorithm_id(request.settings.mode))});
    if (started != warp_dsp::StreamStatus::ok)
      return Opened::failure(failure(*clip->view_, "start"));
    clip->started_ = true;
    clip->left_.assign(render_chunk_frames, 0.0F);
    clip->right_.assign(render_chunk_frames, 0.0F);
    return Opened::success(std::move(clip));
  } catch (const std::bad_alloc&) {
    return Opened::failure({ErrorCode::resource_exhausted, "Export warp allocation failed"});
  }
}

Result<std::size_t> OfflineWarpClip::read_at(std::uint64_t frame,
                                             std::span<media::StereoFrame> output) {
  if (frame >= frames_ || output.empty())
    return Result<std::size_t>::success(0);
  const auto total =
      static_cast<std::size_t>(std::min<std::uint64_t>(output.size(), frames_ - frame));
  if (!started_ || stream_->next_output_frame() != frame) {
    if (stream_->start(*map_, *view_, frame) != warp_dsp::StreamStatus::ok)
      return Result<std::size_t>::failure(failure(*view_, "seek"));
    started_ = true;
  }
  for (std::size_t done = 0; done < total;) {
    const auto count = std::min(render_chunk_frames, total - done);
    const auto rendered =
        stream_->render(std::span(left_).first(count), std::span(right_).first(count));
    if (rendered != count || stream_->status() != warp_dsp::StreamStatus::ok)
      return Result<std::size_t>::failure(failure(*view_, "render"));
    for (std::size_t index = 0; index < count; ++index) {
      if (!std::isfinite(left_[index]) || !std::isfinite(right_[index]))
        return Result<std::size_t>::failure(
            {ErrorCode::corrupt_data, "Export warp stream produced a nonfinite sample"});
      output[done + index] = {left_[index], right_[index]};
    }
    done += count;
  }
  return Result<std::size_t>::success(total);
}

} // namespace oscillate::processing
