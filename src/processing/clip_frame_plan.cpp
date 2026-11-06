#include "oscillate/processing/clip_frame_plan.hpp"

#include <utility>
#include <vector>

namespace oscillate::processing {
namespace {

using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

[[nodiscard]] Result<timeline::SamplePosition>
render_position(const timeline::TempoMap& tempo, std::uint32_t rate,
                const project::Placement& placement,
                const std::variant<timeline::TickPosition, timeline::SamplePosition>& position) {
  if (std::holds_alternative<project::MusicalInterval>(placement)) {
    if (const auto* tick = std::get_if<timeline::TickPosition>(&position)) {
      return tempo.tick_to_sample(*tick, rate);
    }
  } else if (const auto* sample = std::get_if<timeline::SamplePosition>(&position)) {
    return Result<timeline::SamplePosition>::success(*sample);
  }
  return Result<timeline::SamplePosition>::failure(
      {ErrorCode::invalid_argument, "warp marker timebase differs from clip placement"});
}

} // namespace

Result<ClipFramePlan> prepare_clip_frame_plan(const project::Clip& clip,
                                              const project::SourceRecord& source,
                                              const timeline::TempoMap& tempo,
                                              std::uint32_t project_rate,
                                              std::span<const ClipWarpMarker> markers) {
  if (markers.empty())
    markers = clip.processing.reviewed_warp_markers;
  if (auto valid = project::validate_source_record(source); !valid) {
    return Result<ClipFramePlan>::failure(valid.error());
  }
  if (clip.source != source.id || !timeline::supported_sample_rate(project_rate) ||
      clip.source_range.start.value < 0 || clip.source_range.end <= clip.source_range.start ||
      static_cast<std::uint64_t>(clip.source_range.end.value) > source.frames ||
      markers.size() > PreparedWarpMap::max_anchors - 2) {
    return Result<ClipFramePlan>::failure(
        {ErrorCode::invalid_argument, "invalid clip source, range, rate or marker count"});
  }

  std::variant<timeline::TickPosition, timeline::SamplePosition> start;
  std::variant<timeline::TickPosition, timeline::SamplePosition> end;
  if (const auto* musical = std::get_if<project::MusicalInterval>(&clip.placement)) {
    start = musical->start;
    end = musical->end;
  } else {
    const auto& elapsed = std::get<project::ElapsedInterval>(clip.placement);
    start = elapsed.start;
    end = elapsed.end;
  }
  const auto first = render_position(tempo, project_rate, clip.placement, start);
  if (!first) {
    return Result<ClipFramePlan>::failure(first.error());
  }
  const auto last = render_position(tempo, project_rate, clip.placement, end);
  if (!last) {
    return Result<ClipFramePlan>::failure(last.error());
  }
  if (first.value().value < 0 || last.value().value <= first.value().value) {
    return Result<ClipFramePlan>::failure(
        {ErrorCode::invalid_argument, "clip placement has no rendered duration"});
  }

  const auto source_start = static_cast<std::uint64_t>(clip.source_range.start.value);
  const auto source_frames =
      static_cast<std::uint64_t>(clip.source_range.end.value - clip.source_range.start.value);
  const auto output_frames = static_cast<std::uint64_t>(last.value().value - first.value().value);
  std::vector<WarpAnchor> anchors;
  anchors.reserve(markers.size() + 2);
  anchors.push_back({0, 0});
  for (const auto& marker : markers) {
    if (marker.source_frame.value <= clip.source_range.start.value ||
        marker.source_frame.value >= clip.source_range.end.value) {
      return Result<ClipFramePlan>::failure(
          {ErrorCode::invalid_argument, "warp marker lies outside the clip source range"});
    }
    Result<timeline::SamplePosition> target = Result<timeline::SamplePosition>::failure(
        {ErrorCode::invalid_argument, "warp marker timebase differs from clip placement"});
    if (const auto* musical = std::get_if<project::MusicalInterval>(&clip.placement)) {
      if (const auto* offset = std::get_if<timeline::TickPosition>(&marker.target_offset)) {
        if (offset->value > 0 && musical->start.value >= 0 &&
            offset->value < musical->end.value - musical->start.value) {
          target = tempo.tick_to_sample({musical->start.value + offset->value}, project_rate);
        }
      }
    } else if (const auto* offset = std::get_if<timeline::SamplePosition>(&marker.target_offset)) {
      if (offset->value > 0 && static_cast<std::uint64_t>(offset->value) < output_frames) {
        target = Result<timeline::SamplePosition>::success({first.value().value + offset->value});
      }
    }
    if (!target) {
      return Result<ClipFramePlan>::failure(target.error());
    }
    if (target.value() <= first.value() || target.value() >= last.value()) {
      return Result<ClipFramePlan>::failure(
          {ErrorCode::invalid_argument, "warp marker lies outside the rendered clip"});
    }
    anchors.push_back(
        {static_cast<std::uint64_t>(marker.source_frame.value - clip.source_range.start.value),
         static_cast<std::uint64_t>(target.value().value - first.value().value)});
  }
  anchors.push_back({source_frames, output_frames});
  auto warp = PreparedWarpMap::prepare(anchors, source_frames, output_frames);
  if (!warp) {
    return Result<ClipFramePlan>::failure(warp.error());
  }
  return Result<ClipFramePlan>::success(
      {static_cast<std::uint64_t>(first.value().value), source_start, std::move(warp).value()});
}

} // namespace oscillate::processing
