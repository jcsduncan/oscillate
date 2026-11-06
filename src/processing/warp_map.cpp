#include "oscillate/processing/warp_map.hpp"

#include <algorithm>

namespace oscillate::processing {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

[[nodiscard]] std::uint64_t interpolate(std::uint64_t position, std::uint64_t from_span,
                                        std::uint64_t to_span) noexcept {
  return (position * to_span + from_span / 2U) / from_span;
}
} // namespace

Result<PreparedWarpMap> PreparedWarpMap::prepare(std::span<const WarpAnchor> anchors,
                                                 std::uint64_t source_frames,
                                                 std::uint64_t output_frames) {
  if (anchors.size() < 2 || anchors.size() > max_anchors || source_frames == 0 ||
      output_frames == 0 || anchors.front() != WarpAnchor{0, 0} ||
      anchors.back() != WarpAnchor{source_frames, output_frames}) {
    return Result<PreparedWarpMap>::failure(
        {ErrorCode::invalid_argument, "Warp anchors must include both nonempty clip endpoints"});
  }
  std::vector<WarpSegment> segments;
  segments.reserve(anchors.size() - 1);
  for (std::size_t index = 1; index < anchors.size(); ++index) {
    const auto previous = anchors[index - 1];
    const auto next = anchors[index];
    if (next.source_frame <= previous.source_frame || next.output_frame <= previous.output_frame ||
        next.source_frame - previous.source_frame > max_segment_frames ||
        next.output_frame - previous.output_frame > max_segment_frames) {
      return Result<PreparedWarpMap>::failure(
          {ErrorCode::invalid_argument, "Warp anchors must increase within bounded segments"});
    }
    segments.push_back({previous, next});
  }
  return Result<PreparedWarpMap>::success(PreparedWarpMap(std::move(segments), anchors.back()));
}

std::optional<std::uint64_t>
PreparedWarpMap::source_at_output(std::uint64_t output_frame) const noexcept {
  if (output_frame > output_frames_)
    return std::nullopt;
  if (output_frame == output_frames_)
    return source_frames_;
  const auto segment = std::ranges::upper_bound(
      segments_, output_frame, {}, [](const WarpSegment& item) { return item.end.output_frame; });
  // upper_bound with end coordinates returns the first segment ending after
  // the query; preparation guarantees the endpoint and strict ordering.
  const auto& chosen = *segment;
  return chosen.begin.source_frame + interpolate(output_frame - chosen.begin.output_frame,
                                                 chosen.output_frames(), chosen.source_frames());
}

std::optional<std::uint64_t>
PreparedWarpMap::output_at_source(std::uint64_t source_frame) const noexcept {
  if (source_frame > source_frames_)
    return std::nullopt;
  if (source_frame == source_frames_)
    return output_frames_;
  const auto segment = std::ranges::upper_bound(
      segments_, source_frame, {}, [](const WarpSegment& item) { return item.end.source_frame; });
  const auto& chosen = *segment;
  return chosen.begin.output_frame + interpolate(source_frame - chosen.begin.source_frame,
                                                 chosen.source_frames(), chosen.output_frames());
}

} // namespace oscillate::processing
