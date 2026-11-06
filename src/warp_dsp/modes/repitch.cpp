#include "../core/bandlimited_kernel.hpp"
#include "../core/bandlimited_read.hpp"
#include "../core/source_window.hpp"
#include "oscillate/warp_dsp/stream.hpp"

#include <algorithm>
#include <cstddef>
#include <new>

namespace oscillate::warp_dsp {
namespace {

using detail::BandlimitedKernel;
using detail::SourceWindow;

// Each main-window chunk covers at most this many source frames.
constexpr std::size_t chunk_source_frames = std::size_t{1} << 16;

class RepitchStream final : public WarpStream {
public:
  RepitchStream(const StreamLimits& limits, std::shared_ptr<const BandlimitedKernel> kernel)
      : limits_(limits), kernel_(std::move(kernel)),
        max_reach_(detail::bandlimited_reach(*kernel_, repitch_max_speed)),
        main_(chunk_source_frames + 2 * static_cast<std::size_t>(max_reach_) + 2),
        // A crossfade reads at most splice_crossfade_frames at the maximum speed.
        splice_(static_cast<std::size_t>(splice_crossfade_frames * repitch_max_speed) +
                2 * static_cast<std::size_t>(max_reach_) + 4) {}

  StreamStatus start(const WarpTimeMap& map, SourceView& source,
                     std::uint64_t output_frame) noexcept override {
    map_ = nullptr;
    source_ = nullptr;
    main_.reset();
    splice_.reset();
    for (const auto& segment : map.segments()) {
      if (WarpTimeMap::speed(segment) > repitch_max_speed)
        return status_ = StreamStatus::unsupported_map;
    }
    if (output_frame >= map.output_frames())
      return status_ = StreamStatus::out_of_range;
    map_ = &map;
    source_ = &source;
    next_ = output_frame;
    return status_ = StreamStatus::ok;
  }

  std::size_t render(std::span<float> left, std::span<float> right) noexcept override {
    if (status_ != StreamStatus::ok || left.size() != right.size())
      return 0;
    const auto requested = std::min<std::size_t>(left.size(), limits_.max_render_frames);
    std::size_t written = 0;
    TimeMapCursor cursor(*map_);
    while (written < requested) {
      if (!cursor.locate(next_))
        break;
      const auto& segment = cursor.segment();
      const double speed = WarpTimeMap::speed(segment);
      const auto read_plan = detail::make_bandlimited_read_plan(speed);
      const auto reach = detail::bandlimited_reach(*kernel_, speed);
      // Frames until the segment ends, the request is filled, or the chunk's
      // source span would exceed the main window.
      auto frames = std::min<std::uint64_t>(segment.output_end - next_, requested - written);
      const auto first = WarpTimeMap::source_at(segment, next_);
      while (frames > 1) {
        const auto last = WarpTimeMap::source_at(segment, next_ + frames - 1);
        if (((last - first) >> source_fraction_bits) <= chunk_source_frames)
          break;
        frames /= 2;
      }
      const auto last = WarpTimeMap::source_at(segment, next_ + frames - 1);
      const auto window_first = static_cast<std::int64_t>(first >> source_fraction_bits) - reach;
      const auto window_count = static_cast<std::size_t>(
          static_cast<std::int64_t>(last >> source_fraction_bits) + reach + 1 - window_first);
      if (!main_.ensure(*source_, window_first, window_count)) {
        status_ = StreamStatus::source_failed;
        break;
      }
      SegmentStepper stepper(segment, next_);
      for (std::uint64_t offset = 0; offset < frames; ++offset) {
        const auto sample =
            detail::bandlimited_read(main_, *kernel_, stepper.position(), read_plan);
        left[written + offset] = sample.left;
        right[written + offset] = sample.right;
        stepper.advance();
      }
      if (segment.splice && !blend_splice(cursor.index(), next_, frames, left.subspan(written),
                                          right.subspan(written))) {
        status_ = StreamStatus::source_failed;
        break;
      }
      written += static_cast<std::size_t>(frames);
      next_ += frames;
    }
    return written;
  }

  std::uint64_t next_output_frame() const noexcept override { return next_; }
  StreamStatus status() const noexcept override { return status_; }

private:
  // Equal-gain linear crossfade from the previous segment's continuation into
  // the splice target over the first splice_crossfade_frames of the segment.
  bool blend_splice(std::size_t index, std::uint64_t first, std::uint64_t frames,
                    std::span<float> left, std::span<float> right) noexcept {
    const auto& segment = map_->segments()[index];
    const auto& previous = map_->segments()[index - 1];
    const auto length =
        std::min<std::uint64_t>(splice_crossfade_frames, segment.output_end - segment.output_begin);
    const auto blend_end = segment.output_begin + length;
    if (first >= blend_end)
      return true;
    const double speed = WarpTimeMap::speed(previous);
    const auto read_plan = detail::make_bandlimited_read_plan(speed);
    const auto reach = detail::bandlimited_reach(*kernel_, speed);
    const auto stop = std::min(first + frames, blend_end);
    const auto start_position = WarpTimeMap::extrapolate(previous, first);
    const auto end_position = WarpTimeMap::extrapolate(previous, stop - 1);
    const auto window_first =
        static_cast<std::int64_t>(start_position >> source_fraction_bits) - reach;
    const auto window_count = static_cast<std::size_t>(
        static_cast<std::int64_t>(end_position >> source_fraction_bits) + reach + 1 - window_first);
    if (!splice_.ensure(*source_, window_first, window_count))
      return false;
    for (auto frame = first; frame < stop; ++frame) {
      const auto outgoing = detail::bandlimited_read(
          splice_, *kernel_, WarpTimeMap::extrapolate(previous, frame), read_plan);
      const float weight =
          (static_cast<float>(frame - segment.output_begin) + 0.5F) / static_cast<float>(length);
      const auto at = static_cast<std::size_t>(frame - first);
      left[at] = outgoing.left + weight * (left[at] - outgoing.left);
      right[at] = outgoing.right + weight * (right[at] - outgoing.right);
    }
    return true;
  }

  StreamLimits limits_;
  std::shared_ptr<const BandlimitedKernel> kernel_;
  std::int64_t max_reach_;
  SourceWindow main_;
  SourceWindow splice_;
  const WarpTimeMap* map_ = nullptr;
  SourceView* source_ = nullptr;
  std::uint64_t next_ = 0;
  StreamStatus status_ = StreamStatus::not_started;
};

} // namespace

foundation::Result<std::unique_ptr<WarpStream>> create_repitch_stream(const StreamLimits& limits,
                                                                      QualityTier tier) {
  using Result = foundation::Result<std::unique_ptr<WarpStream>>;
  if (!valid_quality_tier(tier))
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Re-Pitch quality tier is invalid"});
  if (limits.sample_rate < 8'000 || limits.sample_rate > 384'000 || limits.max_render_frames == 0 ||
      limits.max_render_frames > 65'536)
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Re-Pitch stream limits are out of range"});
  try {
    return Result::success(
        std::make_unique<RepitchStream>(limits, detail::BandlimitedKernel::for_tier(tier)));
  } catch (const std::bad_alloc&) {
    return Result::failure(
        {foundation::ErrorCode::resource_exhausted, "Re-Pitch stream allocation failed"});
  }
}

} // namespace oscillate::warp_dsp
