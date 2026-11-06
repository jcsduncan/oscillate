#include "oscillate/processing/warp_live_reader.hpp"

#include "oscillate/processing/warp_stream_map.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <new>
#include <string>
#include <utility>

namespace oscillate::processing {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

constexpr std::array<warp_dsp::QualityTier, 3> tiers{
    warp_dsp::QualityTier::q0, warp_dsp::QualityTier::q1, warp_dsp::QualityTier::q2};

// Raises the analysis cancellation flag while opening, from the caller's
// stop token; it is gone before any playback read.
class OpenStopFlag {
public:
  explicit OpenStopFlag(const std::stop_token& stop)
      : callback_(stop, [this]() noexcept { flag_.store(true, std::memory_order_relaxed); }) {}
  [[nodiscard]] const std::atomic<bool>& flag() const noexcept { return flag_; }

private:
  std::atomic<bool> flag_{false};
  std::stop_callback<std::function<void()>> callback_;
};

[[nodiscard]] foundation::Error stream_failure(const ProjectRateSourceView& view,
                                               std::string_view what) {
  if (view.status() != SourceReadStatus::ok)
    return {view.error_code(), "Warp source read failed during live " + std::string(what)};
  return {ErrorCode::io_failure, "Warp stream stopped during live " + std::string(what)};
}
} // namespace

WarpLiveReader::~WarpLiveReader() = default;

Result<std::unique_ptr<WarpLiveReader>>
WarpLiveReader::open(const std::filesystem::path& project_root, media::CacheStore* cache,
                     const WarpRenderRequest& request, const std::stop_token& stop) {
  using Opened = Result<std::unique_ptr<WarpLiveReader>>;
  if (request.quality != warp_dsp::QualityTier::q0)
    return Opened::failure({ErrorCode::invalid_argument, "Live warp starts at quality Q0"});
  auto key = warp_render_cache_key(request);
  if (!key)
    return Opened::failure(key.error());
  if (stop.stop_requested())
    return Opened::failure({ErrorCode::cancelled, "Live warp preparation cancelled"});
  // The managed source is verified even when a cache render exists, so a
  // missing or changed source is never masked by derived cache data. The
  // view's own reads must not be tied to the preparation stop token.
  auto view = open_request_view(project_root, request);
  if (!view)
    return Opened::failure(view.error());
  if (stop.stop_requested())
    return Opened::failure({ErrorCode::cancelled, "Live warp preparation cancelled"});
  try {
    auto reader = std::unique_ptr<WarpLiveReader>(
        new WarpLiveReader(request.settings.mode, request.plan.warp.output_frames()));
    if (cache != nullptr) {
      // A cache filesystem error is not a source error; it only means the
      // stream path renders this clip instead.
      auto cached = WarpRenderCacheReader::open(
          *cache, key.value(),
          {request.plan.warp.output_frames(), request.settings.mode, warp_dsp::QualityTier::q0});
      if (cached) {
        if (auto valid = std::move(cached).value()) {
          reader->cache_.emplace(std::move(*valid));
          return Opened::success(std::move(reader));
        }
      }
    }
    auto map = make_warp_stream_map(request.plan, request.source.sample_rate, request.project_rate);
    if (!map)
      return Opened::failure(map.error());
    reader->map_ = std::make_unique<warp_dsp::WarpTimeMap>(std::move(map).value());
    reader->view_ = std::move(view).value();
    const OpenStopFlag stop_flag(stop);
    // The analysis, once per source and kind, is kept in the project's cache.
    auto analysis = cached_warp_analysis(cache, request, *reader->view_, &stop_flag.flag());
    if (!analysis) {
      if (reader->view_->status() != SourceReadStatus::ok)
        return Opened::failure(
            {reader->view_->error_code(), "Warp source read failed during live analysis"});
      return Opened::failure(analysis.error());
    }
    for (std::size_t tier = 0; tier < tiers.size(); ++tier) {
      auto stream = create_warp_stream(request, analysis.value(), *reader->map_, tiers[tier],
                                       static_cast<std::uint32_t>(max_read_frames));
      if (!stream)
        return Opened::failure(stream.error());
      reader->streams_[tier] = std::move(stream).value();
    }
    reader->left_.assign(max_read_frames, 0.0F);
    reader->right_.assign(max_read_frames, 0.0F);
    // Starting Q0 here proves the map is within the mode's speed range
    // before playback, and leaves the stream ready at frame 0.
    const auto started = reader->streams_[0]->start(*reader->map_, *reader->view_, 0);
    if (started == warp_dsp::StreamStatus::unsupported_map)
      return Opened::failure(
          {ErrorCode::unsupported, "Warp markers exceed the speed range of " +
                                       std::string(warp_algorithm_id(request.settings.mode))});
    if (started != warp_dsp::StreamStatus::ok)
      return Opened::failure(stream_failure(*reader->view_, "start"));
    reader->started_ = true;
    return Opened::success(std::move(reader));
  } catch (const std::bad_alloc&) {
    return Opened::failure({ErrorCode::resource_exhausted, "Live warp allocation failed"});
  }
}

Result<std::size_t> WarpLiveReader::read_at(std::uint64_t frame,
                                            std::span<media::StereoFrame> output) {
  if (output.size() > max_read_frames)
    return Result<std::size_t>::failure(
        {ErrorCode::invalid_argument, "Live warp read exceeds its chunk bound"});
  if (frame >= frames_ || output.empty())
    return Result<std::size_t>::success(0);
  const auto count =
      static_cast<std::size_t>(std::min<std::uint64_t>(output.size(), frames_ - frame));
  if (cache_)
    return cache_->read_at(frame, output.first(count));
  return read_stream(frame, output.first(count));
}

Result<std::size_t> WarpLiveReader::read_stream(std::uint64_t frame,
                                                std::span<media::StereoFrame> output) {
  auto& stream = *streams_[active_tier_];
  if (!started_ || stream.next_output_frame() != frame) {
    // A seek, loop wrap or tier change restarts the stream at this frame.
    if (stream.start(*map_, *view_, frame) != warp_dsp::StreamStatus::ok)
      return Result<std::size_t>::failure(stream_failure(*view_, "seek"));
    started_ = true;
  }
  const auto count = output.size();
  const auto rendered =
      stream.render(std::span(left_).first(count), std::span(right_).first(count));
  if (rendered != count || stream.status() != warp_dsp::StreamStatus::ok)
    return Result<std::size_t>::failure(stream_failure(*view_, "render"));
  for (std::size_t index = 0; index < count; ++index) {
    if (!std::isfinite(left_[index]) || !std::isfinite(right_[index]))
      return Result<std::size_t>::failure(
          {ErrorCode::corrupt_data, "Live warp stream produced a nonfinite sample"});
    output[index] = {left_[index], right_[index]};
  }
  return Result<std::size_t>::success(count);
}

bool WarpLiveReader::step_down() noexcept {
  if (cache_ || active_tier_ + 1 >= tiers.size())
    return false;
  ++active_tier_;
  started_ = false;
  published_tier_.store(static_cast<std::uint8_t>(active_tier_), std::memory_order_release);
  step_downs_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

WarpLiveStatus WarpLiveReader::status() const noexcept {
  return {mode_, cache_ ? WarpLiveBacking::cache : WarpLiveBacking::stream,
          tiers[published_tier_.load(std::memory_order_acquire)],
          step_downs_.load(std::memory_order_relaxed)};
}

} // namespace oscillate::processing
