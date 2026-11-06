#include "oscillate/processing/warp_stream_map.hpp"

#include <vector>

namespace oscillate::processing {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

[[nodiscard]] bool supported_source_rate(std::uint32_t rate) noexcept {
  return rate >= 8'000 && rate <= 384'000;
}
} // namespace

Result<std::uint64_t> project_rate_source_position(std::uint64_t source_frame,
                                                   std::uint32_t source_rate,
                                                   std::uint32_t project_rate) {
  if (!supported_source_rate(source_rate) || !timeline::supported_sample_rate(project_rate)) {
    return Result<std::uint64_t>::failure(
        {ErrorCode::invalid_argument, "Warp source or project sample rate is unsupported"});
  }
  // frame = whole * S + remainder; remainder * P * 65536 < 2^52 cannot overflow.
  const auto whole = source_frame / source_rate;
  const auto remainder = source_frame % source_rate;
  const auto scaled = remainder * project_rate * warp_dsp::source_unit;
  const auto fraction = (2 * scaled + source_rate) / (2 * std::uint64_t{source_rate});
  const auto per_second = std::uint64_t{project_rate} * warp_dsp::source_unit;
  if (whole > (warp_dsp::max_source_position - fraction) / per_second) {
    return Result<std::uint64_t>::failure(
        {ErrorCode::invalid_argument, "Warp source position exceeds the fixed-point range"});
  }
  return Result<std::uint64_t>::success(whole * per_second + fraction);
}

Result<warp_dsp::WarpTimeMap> make_warp_stream_map(const ClipFramePlan& plan,
                                                   std::uint32_t source_rate,
                                                   std::uint32_t project_rate) {
  using MapResult = Result<warp_dsp::WarpTimeMap>;
  if (!supported_source_rate(source_rate) || !timeline::supported_sample_rate(project_rate)) {
    return MapResult::failure(
        {ErrorCode::unsupported, "Warp stream map needs a supported source and project rate"});
  }
  constexpr auto max_source_frame = warp_dsp::max_source_position / warp_dsp::source_unit;
  if (plan.source_start_frame > max_source_frame ||
      plan.warp.source_frames() > max_source_frame - plan.source_start_frame ||
      plan.warp.segments().empty() ||
      plan.warp.segments().size() > warp_dsp::max_time_map_segments) {
    return MapResult::failure(
        {ErrorCode::invalid_argument, "Warp stream source range is out of bounds"});
  }
  std::vector<warp_dsp::TimeMapSegment> segments;
  segments.reserve(plan.warp.segments().size());
  for (const auto& segment : plan.warp.segments()) {
    auto begin = project_rate_source_position(plan.source_start_frame + segment.begin.source_frame,
                                              source_rate, project_rate);
    auto end = project_rate_source_position(plan.source_start_frame + segment.end.source_frame,
                                            source_rate, project_rate);
    if (!begin || !end)
      return MapResult::failure(!begin ? begin.error() : end.error());
    segments.push_back(
        {segment.begin.output_frame, segment.end.output_frame, begin.value(), end.value(), false});
  }
  return warp_dsp::WarpTimeMap::create(std::move(segments));
}

} // namespace oscillate::processing
