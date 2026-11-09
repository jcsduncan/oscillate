// Marker commands on the arrangement and the transition ramp (ADR-0112).
#include "oscillate/application/clip_warp_edit.hpp"

#include "oscillate/processing/clip_frame_plan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

namespace oscillate::application {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

[[nodiscard]] const project::Clip* find_audio_clip(const project::Snapshot& snapshot,
                                                   project::ClipId id) {
  for (const auto& track : snapshot.tracks) {
    const auto clip = std::ranges::find(track.clips, id, &project::Clip::id);
    if (clip != track.clips.end())
      return &*clip;
  }
  return nullptr;
}

// The clip and its layout, with the marker index checked against both.
struct MarkedClip {
  const project::Clip* clip = nullptr;
  ClipWarpLayout layout;
};

[[nodiscard]] Result<MarkedClip> marked_clip(const project::Snapshot& snapshot, project::ClipId id,
                                             std::size_t index) {
  const auto* clip = find_audio_clip(snapshot, id);
  if (clip == nullptr)
    return Result<MarkedClip>::failure(
        {ErrorCode::not_found, "Warp clip is no longer in the project"});
  auto layout = clip_warp_layout(snapshot, id);
  if (!layout)
    return Result<MarkedClip>::failure(layout.error());
  if (index >= clip->processing.reviewed_warp_markers.size() ||
      index >= layout.value().markers.size())
    return Result<MarkedClip>::failure({ErrorCode::not_found, "Warp marker no longer exists"});
  return Result<MarkedClip>::success({clip, std::move(layout).value()});
}

// The anchors around the segments: clip start, the markers, clip end.
[[nodiscard]] WarpMarkerPosition clip_start(const ClipWarpLayout& layout) noexcept {
  return {layout.source_first, 0};
}
[[nodiscard]] WarpMarkerPosition clip_end(const ClipWarpLayout& layout) noexcept {
  return {layout.source_first + layout.source_frames, layout.output_frames};
}

// The absolute project tick of a clip-local output frame, under the map.
[[nodiscard]] Result<std::int64_t> tick_at_output(const project::Snapshot& snapshot,
                                                  const ClipWarpLayout& layout,
                                                  std::uint64_t output_frame) {
  auto tick = snapshot.tempo.sample_to_tick(
      {static_cast<std::int64_t>(layout.project_start + output_frame)}, snapshot.sample_rate);
  if (!tick)
    return Result<std::int64_t>::failure(tick.error());
  return Result<std::int64_t>::success(tick.value().value);
}

// The clip's span on the timeline, in absolute project ticks.
struct TickSpan {
  std::int64_t start = 0;
  std::int64_t end = 0;
};
[[nodiscard]] Result<TickSpan> tick_span(const project::Snapshot& snapshot,
                                         const project::Clip& clip) {
  if (const auto* musical = std::get_if<project::MusicalInterval>(&clip.placement))
    return Result<TickSpan>::success({musical->start.value, musical->end.value});
  const auto& elapsed = std::get<project::ElapsedInterval>(clip.placement);
  auto start = snapshot.tempo.sample_to_tick(elapsed.start, snapshot.sample_rate);
  auto end = snapshot.tempo.sample_to_tick(elapsed.end, snapshot.sample_rate);
  if (!start || !end)
    return Result<TickSpan>::failure(!start ? start.error() : end.error());
  return Result<TickSpan>::success({start.value().value, end.value().value});
}

// The bar line nearest `tick` under the meter map.
[[nodiscard]] Result<std::int64_t> nearest_bar_line(const project::Snapshot& snapshot,
                                                    std::int64_t tick) {
  auto located = snapshot.meter.locate({std::max<std::int64_t>(0, tick)});
  if (!located)
    return Result<std::int64_t>::failure(located.error());
  auto here = snapshot.meter.tick_at({located.value().bar, 1, 0});
  auto next = snapshot.meter.tick_at({located.value().bar + 1, 1, 0});
  if (!here || !next)
    return Result<std::int64_t>::failure(!here ? here.error() : next.error());
  return Result<std::int64_t>::success(tick - here.value().value <= next.value().value - tick
                                           ? here.value().value
                                           : next.value().value);
}

// The bar line one bar after the bar that starts at `bar_tick`.
[[nodiscard]] Result<std::int64_t> next_bar_line(const project::Snapshot& snapshot,
                                                 std::int64_t bar_tick) {
  auto located = snapshot.meter.locate({bar_tick});
  if (!located)
    return Result<std::int64_t>::failure(located.error());
  auto next = snapshot.meter.tick_at({located.value().bar + 1, 1, 0});
  if (!next)
    return Result<std::int64_t>::failure(next.error());
  return Result<std::int64_t>::success(next.value().value);
}

} // namespace

Result<project::ClipProcessing> slide_warp_marker(const project::Snapshot& snapshot,
                                                  project::ClipId id, WarpMarkerSlide slide) {
  using Edited = Result<project::ClipProcessing>;
  auto marked = marked_clip(snapshot, id, slide.index);
  if (!marked)
    return Edited::failure(marked.error());
  const auto& [clip, layout] = marked.value();
  const auto source = std::ranges::find(snapshot.sources, clip->source, &project::SourceRecord::id);
  if (source == snapshot.sources.end())
    return Edited::failure({ErrorCode::not_found, "Warp clip source is missing"});
  auto plan =
      processing::prepare_clip_frame_plan(*clip, *source, snapshot.tempo, snapshot.sample_rate);
  if (!plan)
    return Edited::failure(plan.error());
  const auto local = plan.value().warp.source_at_output(slide.output_frame);
  if (!local)
    return Edited::failure({ErrorCode::invalid_argument, "Slide the audio from inside the clip"});
  const auto source_frame = layout.source_first + *local;
  const auto& markers = layout.markers;
  const auto lower =
      slide.index == 0 ? clip_start(layout).source_frame : markers[slide.index - 1].source_frame;
  const auto upper = slide.index + 1 == markers.size() ? clip_end(layout).source_frame
                                                       : markers[slide.index + 1].source_frame;
  if (source_frame <= lower || source_frame >= upper)
    return Edited::failure(
        {ErrorCode::invalid_argument, "The audio cannot slide past the neighbouring markers"});
  auto processing = clip->processing;
  processing.reviewed_warp_markers[slide.index].source_frame = {
      static_cast<std::int64_t>(source_frame)};
  return Edited::success(std::move(processing));
}

Result<project::ClipProcessing> remove_warp_markers_after(const project::Snapshot& snapshot,
                                                          project::ClipId id, std::size_t index) {
  using Edited = Result<project::ClipProcessing>;
  auto marked = marked_clip(snapshot, id, index);
  if (!marked)
    return Edited::failure(marked.error());
  auto processing = marked.value().clip->processing;
  if (index + 1 == processing.reviewed_warp_markers.size())
    return Edited::failure({ErrorCode::not_found, "No warp markers after this one"});
  processing.reviewed_warp_markers.resize(index + 1);
  return Edited::success(std::move(processing));
}

Result<ClipWarpProposal> straighten_after_warp_marker(const project::Snapshot& snapshot,
                                                      project::ClipId id, std::size_t index) {
  using Proposal = Result<ClipWarpProposal>;
  auto marked = marked_clip(snapshot, id, index);
  if (!marked)
    return Proposal::failure(marked.error());
  const auto& [clip, layout] = marked.value();
  const auto& here = layout.markers[index];
  const auto before = index == 0 ? clip_start(layout) : layout.markers[index - 1];
  // Project frames per source frame across the segment that ends here.
  const auto rate = static_cast<long double>(here.output_frame - before.output_frame) /
                    static_cast<long double>(here.source_frame - before.source_frame);
  const auto end_output =
      here.output_frame +
      static_cast<std::uint64_t>(std::llround(
          static_cast<long double>(clip_end(layout).source_frame - here.source_frame) * rate));
  if (end_output <= here.output_frame)
    return Proposal::failure({ErrorCode::invalid_argument, "The straightened clip would be empty"});
  ClipWarpProposal proposal;
  proposal.settings_generation = clip->settings_generation;
  proposal.processing = clip->processing;
  proposal.processing.reviewed_warp_markers.resize(index + 1);
  if (const auto* musical = std::get_if<project::MusicalInterval>(&clip->placement)) {
    auto end = tick_at_output(snapshot, layout, end_output);
    if (!end)
      return Proposal::failure(end.error());
    proposal.placement = project::MusicalInterval{musical->start, {end.value()}};
  } else {
    const auto& elapsed = std::get<project::ElapsedInterval>(clip->placement);
    proposal.placement = project::ElapsedInterval{
        elapsed.start, {elapsed.start.value + static_cast<std::int64_t>(end_output)}};
  }
  if (const auto grid = source_beat_grid(snapshot, clip->source))
    proposal.source_bpm = grid->bpm;
  return Proposal::success(std::move(proposal));
}

Result<double> warp_segment_source_bpm(const project::Snapshot& snapshot, project::ClipId id,
                                       std::size_t index) {
  auto marked = marked_clip(snapshot, id, index);
  if (!marked)
    return Result<double>::failure(marked.error());
  const auto& layout = marked.value().layout;
  const auto& here = layout.markers[index];
  const auto next =
      index + 1 == layout.markers.size() ? clip_end(layout) : layout.markers[index + 1];
  auto from = tick_at_output(snapshot, layout, here.output_frame);
  auto to = tick_at_output(snapshot, layout, next.output_frame);
  if (!from || !to)
    return Result<double>::failure(!from ? from.error() : to.error());
  const auto quarters = static_cast<double>(to.value() - from.value()) /
                        static_cast<double>(timeline::ticks_per_quarter);
  const auto minutes = static_cast<double>(next.source_frame - here.source_frame) /
                       (60.0 * static_cast<double>(layout.source_rate));
  const auto bpm = quarters / minutes;
  if (!std::isfinite(bpm) || bpm < timeline::min_detected_bpm || bpm > timeline::max_detected_bpm)
    return Result<double>::failure(
        {ErrorCode::invalid_argument,
         "This segment's tempo is outside the range a track tempo can take"});
  return Result<double>::success(bpm);
}

Result<TransitionRamp> plan_transition_ramp(const project::Snapshot& snapshot, project::ClipId id) {
  using Planned = Result<TransitionRamp>;
  const auto* clip = find_audio_clip(snapshot, id);
  if (clip == nullptr)
    return Planned::failure({ErrorCode::not_found, "That clip is no longer in the project"});
  const auto grid = source_beat_grid(snapshot, clip->source);
  if (!grid)
    return Planned::failure(
        {ErrorCode::unavailable, "No tempo for this track's audio yet; it is still analysing"});
  auto incoming = tick_span(snapshot, *clip);
  if (!incoming)
    return Planned::failure(incoming.error());
  // The latest end among the audio already playing when this clip starts.
  std::optional<std::int64_t> outgoing_end;
  for (const auto& track : snapshot.tracks) {
    if (std::ranges::find(track.clips, id, &project::Clip::id) != track.clips.end())
      continue;
    for (const auto& other : track.clips) {
      auto span = tick_span(snapshot, other);
      if (!span)
        return Planned::failure(span.error());
      if (span.value().start < incoming.value().start && span.value().end > incoming.value().start)
        outgoing_end = std::max(outgoing_end.value_or(span.value().end), span.value().end);
    }
  }
  if (!outgoing_end)
    return Planned::failure({ErrorCode::invalid_argument,
                             "No other track plays into this clip: there is no transition"});
  auto from = nearest_bar_line(snapshot, incoming.value().start);
  auto to = nearest_bar_line(snapshot, std::min(*outgoing_end, incoming.value().end));
  if (!from || !to)
    return Planned::failure(!from ? from.error() : to.error());
  auto end = to.value();
  if (end <= from.value()) {
    auto next = next_bar_line(snapshot, from.value());
    if (!next)
      return Planned::failure(next.error());
    end = next.value();
  }
  return Planned::success({{from.value()}, {end}, grid->bpm});
}

} // namespace oscillate::application
