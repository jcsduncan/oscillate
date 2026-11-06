#include "oscillate/warp_dsp/time_map.hpp"

#include <algorithm>
#include <new>

namespace oscillate::warp_dsp {
namespace {

using foundation::ErrorCode;
using Result = foundation::Result<WarpTimeMap>;

[[nodiscard]] Result invalid(const char* message) {
  return Result::failure({ErrorCode::invalid_argument, message});
}

// floor(d * delta_source / delta_output) without overflow: d * q never exceeds
// the extrapolated source span and d * r stays below 2^63 under the map bounds.
[[nodiscard]] std::uint64_t scaled(const TimeMapSegment& segment, std::uint64_t d) noexcept {
  const auto delta_output = segment.output_end - segment.output_begin;
  const auto delta_source = segment.source_end - segment.source_begin;
  const auto quotient = delta_source / delta_output;
  const auto remainder = delta_source % delta_output;
  return d * quotient + (d * remainder) / delta_output;
}

} // namespace

foundation::Result<WarpTimeMap> WarpTimeMap::create(std::vector<TimeMapSegment> segments) {
  if (segments.empty() || segments.size() > max_time_map_segments)
    return invalid("Warp time map needs 1 to 2^20 segments");
  if (segments.front().output_begin != 0 || segments.front().splice)
    return invalid("Warp time map must start at output frame 0 without a splice");
  for (std::size_t index = 0; index < segments.size(); ++index) {
    const auto& segment = segments[index];
    if (segment.output_end <= segment.output_begin ||
        segment.output_end - segment.output_begin > max_segment_output_frames)
      return invalid("Warp time map segment length must be 1 to 2^31 output frames");
    if (segment.source_end < segment.source_begin || segment.source_end > max_source_position)
      return invalid("Warp time map source must not reverse or exceed 2^44 frames");
    if (segment.source_end - segment.source_begin >
        (segment.output_end - segment.output_begin) << (max_map_speed_bits + source_fraction_bits))
      return invalid("Warp time map segment exceeds 1024 source frames per output frame");
    if (index == 0)
      continue;
    const auto& previous = segments[index - 1];
    if (segment.output_begin != previous.output_end)
      return invalid("Warp time map segments must be contiguous in output");
    if (!segment.splice && segment.source_begin != previous.source_end)
      return invalid("Warp time map source must be continuous except at a splice");
  }
  try {
    return Result::success(
        WarpTimeMap(std::move(segments), std::make_shared<const std::byte>(std::byte{0})));
  } catch (const std::bad_alloc&) {
    return Result::failure({ErrorCode::resource_exhausted, "Warp map allocation failed"});
  }
}

std::size_t WarpTimeMap::find_segment(std::uint64_t output_frame) const noexcept {
  const auto found =
      std::ranges::upper_bound(segments_, output_frame, {}, &TimeMapSegment::output_end);
  return static_cast<std::size_t>(found - segments_.begin());
}

std::uint64_t WarpTimeMap::source_at(const TimeMapSegment& segment,
                                     std::uint64_t output_frame) noexcept {
  return segment.source_begin + scaled(segment, output_frame - segment.output_begin);
}

std::uint64_t WarpTimeMap::first_output_reaching(const TimeMapSegment& segment,
                                                 std::uint64_t source) noexcept {
  if (source <= segment.source_begin)
    return segment.output_begin;
  // source_at is non-decreasing, so binary search finds the exact first frame.
  std::uint64_t low = segment.output_begin;
  std::uint64_t high = segment.output_end;
  while (low < high) {
    const auto middle = low + (high - low) / 2;
    if (source_at(segment, middle) >= source)
      high = middle;
    else
      low = middle + 1;
  }
  return low;
}

std::uint64_t WarpTimeMap::extrapolate(const TimeMapSegment& segment,
                                       std::uint64_t output_frame) noexcept {
  return source_at(segment, output_frame);
}

double WarpTimeMap::speed(const TimeMapSegment& segment) noexcept {
  return static_cast<double>(segment.source_end - segment.source_begin) /
         (static_cast<double>(segment.output_end - segment.output_begin) *
          static_cast<double>(source_unit));
}

SegmentStepper::SegmentStepper(const TimeMapSegment& segment, std::uint64_t output_frame) noexcept
    : delta_output_(segment.output_end - segment.output_begin) {
  const auto delta_source = segment.source_end - segment.source_begin;
  const auto offset = output_frame - segment.output_begin;
  quotient_ = delta_source / delta_output_;
  remainder_ = delta_source % delta_output_;
  const auto scaled_remainder = offset * remainder_;
  position_ = segment.source_begin + offset * quotient_ + scaled_remainder / delta_output_;
  error_ = scaled_remainder % delta_output_;
}

bool TimeMapCursor::locate(std::uint64_t output_frame) noexcept {
  const auto segments = map_->segments();
  if (output_frame >= segments.back().output_end)
    return false;
  const auto& current = segments[index_];
  if (output_frame >= current.output_begin && output_frame < current.output_end)
    return true;
  if (output_frame >= current.output_end && index_ + 1 < segments.size() &&
      output_frame < segments[index_ + 1].output_end) {
    ++index_;
    return true;
  }
  index_ = map_->find_segment(output_frame);
  return true;
}

} // namespace oscillate::warp_dsp
