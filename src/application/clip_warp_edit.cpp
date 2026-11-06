#include "oscillate/application/clip_warp_edit.hpp"

#include "oscillate/processing/clip_frame_plan.hpp"
#include "oscillate/processing/verified_source_view.hpp"
#include "oscillate/processing/warp_render_cache.hpp"
#include "oscillate/warp_dsp/analysis.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>
#include <variant>

namespace oscillate::application {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

struct Located {
  const project::Clip* clip;
  const project::SourceRecord* source;
};

[[nodiscard]] Result<Located> locate(const project::Snapshot& snapshot, project::ClipId id) {
  for (const auto& track : snapshot.tracks) {
    const auto clip = std::ranges::find(track.clips, id, &project::Clip::id);
    if (clip == track.clips.end())
      continue;
    const auto source =
        std::ranges::find(snapshot.sources, clip->source, &project::SourceRecord::id);
    if (source == snapshot.sources.end())
      return Result<Located>::failure({ErrorCode::not_found, "Warp clip source is missing"});
    return Result<Located>::success({&*clip, &*source});
  }
  return Result<Located>::failure({ErrorCode::not_found, "Warp clip is no longer in the project"});
}

// The marker's target offset in the clip's placement timebase for a
// clip-local output frame.
[[nodiscard]] Result<std::variant<timeline::TickPosition, timeline::SamplePosition>>
target_offset(const project::Snapshot& snapshot, const project::Clip& clip,
              const ClipWarpLayout& layout, std::uint64_t output_frame) {
  using Offset = Result<std::variant<timeline::TickPosition, timeline::SamplePosition>>;
  if (output_frame == 0 || output_frame >= layout.output_frames)
    return Offset::failure(
        {ErrorCode::invalid_argument, "A warp marker must lie strictly inside the clip"});
  if (const auto* musical = std::get_if<project::MusicalInterval>(&clip.placement)) {
    auto tick = snapshot.tempo.sample_to_tick(
        {static_cast<std::int64_t>(layout.project_start + output_frame)}, snapshot.sample_rate);
    if (!tick)
      return Offset::failure(tick.error());
    return Offset::success(timeline::TickPosition{tick.value().value - musical->start.value});
  }
  return Offset::success(timeline::SamplePosition{static_cast<std::int64_t>(output_frame)});
}

void sort_markers(std::vector<project::ClipWarpMarker>& markers) {
  std::ranges::sort(
      markers, {}, [](const project::ClipWarpMarker& marker) { return marker.source_frame.value; });
}

} // namespace

Result<ClipWarpLayout> clip_warp_layout(const project::Snapshot& snapshot, project::ClipId id) {
  auto located = locate(snapshot, id);
  if (!located)
    return Result<ClipWarpLayout>::failure(located.error());
  const auto& [clip, source] = located.value();
  auto plan =
      processing::prepare_clip_frame_plan(*clip, *source, snapshot.tempo, snapshot.sample_rate);
  if (!plan)
    return Result<ClipWarpLayout>::failure(plan.error());
  ClipWarpLayout layout;
  layout.project_start = plan.value().project_start_frame;
  layout.output_frames = plan.value().warp.output_frames();
  layout.source_first = plan.value().source_start_frame;
  layout.source_frames = plan.value().warp.source_frames();
  layout.source_rate = source->sample_rate;
  layout.project_rate = snapshot.sample_rate;
  layout.musical = std::holds_alternative<project::MusicalInterval>(clip->placement);
  // Anchors are clip start, the reviewed markers in order, then clip end.
  const auto segments = plan.value().warp.segments();
  for (std::size_t index = 0; index + 1 < segments.size(); ++index)
    layout.markers.push_back(
        {layout.source_first + segments[index].end.source_frame, segments[index].end.output_frame});
  return Result<ClipWarpLayout>::success(std::move(layout));
}

Result<project::ClipProcessing> add_warp_marker(const project::Snapshot& snapshot,
                                                project::ClipId id, std::uint64_t output_frame) {
  using Edited = Result<project::ClipProcessing>;
  auto located = locate(snapshot, id);
  auto layout = clip_warp_layout(snapshot, id);
  if (!located || !layout)
    return Edited::failure(!located ? located.error() : layout.error());
  const auto& clip = *located.value().clip;
  auto plan = processing::prepare_clip_frame_plan(clip, *located.value().source, snapshot.tempo,
                                                  snapshot.sample_rate);
  if (!plan)
    return Edited::failure(plan.error());
  const auto local = plan.value().warp.source_at_output(output_frame);
  if (!local || *local == 0 || *local >= layout.value().source_frames)
    return Edited::failure(
        {ErrorCode::invalid_argument, "A warp marker must lie strictly inside the clip"});
  const auto source_frame = layout.value().source_first + *local;
  if (std::ranges::any_of(layout.value().markers, [&](const WarpMarkerPosition& marker) {
        return marker.source_frame == source_frame || marker.output_frame == output_frame;
      }))
    return Edited::failure({ErrorCode::conflict, "A warp marker already exists at this position"});
  auto offset = target_offset(snapshot, clip, layout.value(), output_frame);
  if (!offset)
    return Edited::failure(offset.error());
  auto processing = clip.processing;
  processing.reviewed_warp_markers.push_back(
      {{static_cast<std::int64_t>(source_frame)}, offset.value()});
  sort_markers(processing.reviewed_warp_markers);
  return Edited::success(std::move(processing));
}

Result<project::ClipProcessing> move_warp_marker(const project::Snapshot& snapshot,
                                                 project::ClipId id, std::size_t index,
                                                 std::uint64_t output_frame) {
  using Edited = Result<project::ClipProcessing>;
  auto located = locate(snapshot, id);
  auto layout = clip_warp_layout(snapshot, id);
  if (!located || !layout)
    return Edited::failure(!located ? located.error() : layout.error());
  const auto& clip = *located.value().clip;
  if (index >= clip.processing.reviewed_warp_markers.size())
    return Edited::failure({ErrorCode::not_found, "Warp marker no longer exists"});
  // Neighbouring markers bound the move so the map stays monotone.
  const auto& markers = layout.value().markers;
  const auto lower = index == 0 ? 0 : markers[index - 1].output_frame;
  const auto upper =
      index + 1 == markers.size() ? layout.value().output_frames : markers[index + 1].output_frame;
  if (output_frame <= lower || output_frame >= upper)
    return Edited::failure(
        {ErrorCode::invalid_argument, "A warp marker cannot pass its neighbours or the clip ends"});
  auto offset = target_offset(snapshot, clip, layout.value(), output_frame);
  if (!offset)
    return Edited::failure(offset.error());
  auto processing = clip.processing;
  processing.reviewed_warp_markers[index].target_offset = offset.value();
  return Edited::success(std::move(processing));
}

Result<project::ClipProcessing> remove_warp_marker(const project::Snapshot& snapshot,
                                                   project::ClipId id, std::size_t index) {
  using Edited = Result<project::ClipProcessing>;
  auto located = locate(snapshot, id);
  if (!located)
    return Edited::failure(located.error());
  auto processing = located.value().clip->processing;
  if (index >= processing.reviewed_warp_markers.size())
    return Edited::failure({ErrorCode::not_found, "Warp marker no longer exists"});
  processing.reviewed_warp_markers.erase(processing.reviewed_warp_markers.begin() +
                                         static_cast<std::ptrdiff_t>(index));
  return Edited::success(std::move(processing));
}

Result<project::ClipProcessing> apply_warp_suggestions(const project::Snapshot& snapshot,
                                                       project::ClipId id,
                                                       std::span<const std::uint64_t> source_frames,
                                                       std::uint64_t minimum_gap) {
  using Edited = Result<project::ClipProcessing>;
  auto located = locate(snapshot, id);
  auto layout = clip_warp_layout(snapshot, id);
  if (!located || !layout)
    return Edited::failure(!located ? located.error() : layout.error());
  const auto& clip = *located.value().clip;
  auto plan = processing::prepare_clip_frame_plan(clip, *located.value().source, snapshot.tempo,
                                                  snapshot.sample_rate);
  if (!plan)
    return Edited::failure(plan.error());
  std::vector<std::uint64_t> kept;
  std::vector<std::uint64_t> kept_outputs;
  for (const auto& marker : layout.value().markers)
    kept.push_back(marker.source_frame);
  for (const auto& marker : layout.value().markers)
    kept_outputs.push_back(marker.output_frame);
  auto processing = clip.processing;
  std::size_t added = 0;
  std::vector<std::uint64_t> sorted(source_frames.begin(), source_frames.end());
  std::ranges::sort(sorted);
  for (const auto source_frame : sorted) {
    if (source_frame <= layout.value().source_first ||
        source_frame >= layout.value().source_first + layout.value().source_frames)
      continue;
    if (std::ranges::any_of(kept, [&](std::uint64_t existing) {
          const auto distance =
              existing > source_frame ? existing - source_frame : source_frame - existing;
          return distance < std::max<std::uint64_t>(minimum_gap, 1);
        }))
      continue;
    const auto output =
        plan.value().warp.output_at_source(source_frame - layout.value().source_first);
    if (!output || *output == 0 || *output >= layout.value().output_frames ||
        std::ranges::find(kept_outputs, *output) != kept_outputs.end())
      continue;
    auto offset = target_offset(snapshot, clip, layout.value(), *output);
    if (!offset)
      continue;
    processing.reviewed_warp_markers.push_back(
        {{static_cast<std::int64_t>(source_frame)}, offset.value()});
    kept.push_back(source_frame);
    kept_outputs.push_back(*output);
    ++added;
  }
  if (added == 0)
    return Edited::failure({ErrorCode::invalid_argument, "No suggested marker can be applied"});
  if (processing.reviewed_warp_markers.size() > project::max_clip_warp_markers)
    return Edited::failure({ErrorCode::resource_exhausted, "Too many warp markers for one clip"});
  sort_markers(processing.reviewed_warp_markers);
  return Edited::success(std::move(processing));
}

Result<SourceOnsets> analyse_source_onsets(const std::filesystem::path& project_root,
                                           const project::Snapshot& snapshot, project::SourceId id,
                                           const std::atomic<bool>* cancelled) {
  const auto source = std::ranges::find(snapshot.sources, id, &project::SourceRecord::id);
  if (source == snapshot.sources.end())
    return Result<SourceOnsets>::failure({ErrorCode::not_found, "Onset source is missing"});
  auto view = processing::ProjectRateSourceView::open(project_root, *source, snapshot.sample_rate);
  if (!view)
    return Result<SourceOnsets>::failure(view.error());
  auto found =
      warp_dsp::analysis::detect_transients(*view.value(), {snapshot.sample_rate}, cancelled);
  if (!found) {
    if (view.value()->status() != processing::SourceReadStatus::ok)
      return Result<SourceOnsets>::failure(
          {view.value()->error_code(), "Onset analysis source read failed"});
    return Result<SourceOnsets>::failure(found.error());
  }
  SourceOnsets onsets;
  for (const auto& transient : found.value()) {
    const auto frame =
        source_frame_from_view(transient.frame, source->sample_rate, snapshot.sample_rate);
    if (frame >= source->frames || (!onsets.frames.empty() && onsets.frames.back() >= frame))
      continue;
    if (onsets.frames.size() == project::max_tempo_markers_per_source) {
      onsets.truncated = true;
      break;
    }
    onsets.frames.push_back(frame);
  }
  return Result<SourceOnsets>::success(std::move(onsets));
}

std::vector<std::uint64_t> clip_onset_suggestions(const project::Snapshot& snapshot,
                                                  project::ClipId id, std::uint64_t minimum_gap) {
  auto located = locate(snapshot, id);
  if (!located)
    return {};
  const auto& clip = *located.value().clip;
  const auto review = std::ranges::find(snapshot.source_tempo_reviews, clip.source,
                                        &project::SourceTempoReview::source);
  if (review == snapshot.source_tempo_reviews.end())
    return {};
  const auto first = static_cast<std::uint64_t>(clip.source_range.start.value);
  const auto last = static_cast<std::uint64_t>(clip.source_range.end.value);
  std::vector<std::uint64_t> frames;
  for (const auto frame : review->generated_onset_frames) {
    if (frame <= first || frame >= last)
      continue;
    const bool near_marker =
        std::ranges::any_of(clip.processing.reviewed_warp_markers, [&](const auto& marker) {
          const auto at = static_cast<std::uint64_t>(marker.source_frame.value);
          return (at > frame ? at - frame : frame - at) < minimum_gap;
        });
    if (!near_marker)
      frames.push_back(frame);
  }
  return frames;
}

std::uint64_t source_frame_from_view(std::uint64_t view_frame, std::uint32_t source_rate,
                                     std::uint32_t project_rate) noexcept {
  if (source_rate == project_rate || project_rate == 0)
    return view_frame;
  const auto whole = view_frame / project_rate;
  const auto remainder = view_frame % project_rate;
  return whole * source_rate + (remainder * source_rate + project_rate / 2) / project_rate;
}

namespace {

// The clip's map with linear extension beyond both ends, in absolute source
// frames against clip-relative positions in the placement timebase.
class ExtendedClipMap {
public:
  static Result<ExtendedClipMap> open(const project::Snapshot& snapshot, project::ClipId id) {
    auto located = locate(snapshot, id);
    if (!located)
      return Result<ExtendedClipMap>::failure(located.error());
    const auto& [clip, source] = located.value();
    auto plan =
        processing::prepare_clip_frame_plan(*clip, *source, snapshot.tempo, snapshot.sample_rate);
    if (!plan)
      return Result<ExtendedClipMap>::failure(plan.error());
    ExtendedClipMap map(snapshot, *clip, *source, std::move(plan).value());
    if (const auto* musical = std::get_if<project::MusicalInterval>(&clip->placement)) {
      map.start_ = musical->start.value;
      map.length_ = musical->end.value - musical->start.value;
    } else {
      const auto& elapsed = std::get<project::ElapsedInterval>(clip->placement);
      map.start_ = elapsed.start.value;
      map.length_ = elapsed.end.value - elapsed.start.value;
    }
    return Result<ExtendedClipMap>::success(std::move(map));
  }

  [[nodiscard]] const project::Clip& clip() const noexcept { return *clip_; }
  [[nodiscard]] const project::SourceRecord& source() const noexcept { return *source_; }
  [[nodiscard]] bool musical() const noexcept {
    return std::holds_alternative<project::MusicalInterval>(clip_->placement);
  }
  [[nodiscard]] std::int64_t start() const noexcept { return start_; }
  [[nodiscard]] std::int64_t length() const noexcept { return length_; }

  // Clip-local output frame (may be negative or past the end) of a position.
  [[nodiscard]] Result<std::int64_t> output_at(std::int64_t position) const {
    const auto first = static_cast<std::int64_t>(plan_.project_start_frame);
    if (!musical())
      return Result<std::int64_t>::success(position);
    if (start_ + position >= 0) {
      auto sample = snapshot_->tempo.tick_to_sample({start_ + position}, snapshot_->sample_rate);
      if (!sample)
        return Result<std::int64_t>::failure(sample.error());
      return Result<std::int64_t>::success(sample.value().value - first);
    }
    // Before tick 0 the tempo map has no time: mirror the tempo after the
    // clip start, the only musical time the clip has there.
    auto mirrored = snapshot_->tempo.tick_to_sample({start_ - position}, snapshot_->sample_rate);
    if (!mirrored)
      return Result<std::int64_t>::failure(mirrored.error());
    return Result<std::int64_t>::success(first - mirrored.value().value);
  }

  // Absolute source frame the extended map plays at a clip-local output frame.
  [[nodiscard]] Result<std::int64_t> source_at_output(std::int64_t output) const {
    const auto output_frames = static_cast<std::int64_t>(plan_.warp.output_frames());
    std::int64_t local = 0;
    if (output >= 0 && output <= output_frames) {
      const auto mapped = plan_.warp.source_at_output(static_cast<std::uint64_t>(output));
      if (!mapped)
        return Result<std::int64_t>::failure(
            {ErrorCode::invalid_argument, "Clip position is outside its warp map"});
      local = static_cast<std::int64_t>(*mapped);
    } else {
      const auto [a, b] = edge(output < 0);
      local = a.source + std::llround(static_cast<long double>(output - a.output) *
                                      static_cast<long double>(b.source - a.source) /
                                      static_cast<long double>(b.output - a.output));
    }
    const auto absolute = static_cast<std::int64_t>(plan_.source_start_frame) + local;
    if (absolute < 0 || static_cast<std::uint64_t>(absolute) > source_->frames)
      return Result<std::int64_t>::failure(
          {ErrorCode::invalid_argument, "The clip would leave the source audio"});
    return Result<std::int64_t>::success(absolute);
  }

  // Clip-local output frame at which the extended map plays an absolute
  // source frame.
  [[nodiscard]] std::int64_t output_at_source(std::int64_t absolute) const {
    const auto local = absolute - static_cast<std::int64_t>(plan_.source_start_frame);
    const auto source_frames = static_cast<std::int64_t>(plan_.warp.source_frames());
    if (local >= 0 && local <= source_frames) {
      if (const auto mapped = plan_.warp.output_at_source(static_cast<std::uint64_t>(local)))
        return static_cast<std::int64_t>(*mapped);
    }
    const auto [a, b] = edge(local < 0);
    return a.output + std::llround(static_cast<long double>(local - a.source) *
                                   static_cast<long double>(b.output - a.output) /
                                   static_cast<long double>(b.source - a.source));
  }

  // Position (placement timebase) of a clip-local output frame, rounded to
  // the nearest tick; exact for elapsed clips.
  [[nodiscard]] Result<std::int64_t> position_at(std::int64_t output) const {
    if (!musical())
      return Result<std::int64_t>::success(output);
    const auto first = static_cast<std::int64_t>(plan_.project_start_frame);
    const bool before = first + output < 0;
    const auto sample = before ? first - output : first + output;
    auto tick = snapshot_->tempo.sample_to_tick({sample}, snapshot_->sample_rate);
    if (!tick)
      return Result<std::int64_t>::failure(tick.error());
    // Mirrored before tick 0, as in output_at().
    return Result<std::int64_t>::success(before ? start_ - tick.value().value
                                                : tick.value().value - start_);
  }

private:
  struct Point {
    std::int64_t source; // clip-local source frame
    std::int64_t output; // clip-local output frame
  };
  ExtendedClipMap(const project::Snapshot& snapshot, const project::Clip& clip,
                  const project::SourceRecord& source, processing::ClipFramePlan plan)
      : snapshot_(&snapshot), clip_(&clip), source_(&source), plan_(std::move(plan)) {}

  // The first (or last) segment, whose slope extends the map.
  [[nodiscard]] std::pair<Point, Point> edge(bool first) const noexcept {
    const auto segments = plan_.warp.segments();
    const auto& segment = first ? segments.front() : segments.back();
    return {{static_cast<std::int64_t>(segment.begin.source_frame),
             static_cast<std::int64_t>(segment.begin.output_frame)},
            {static_cast<std::int64_t>(segment.end.source_frame),
             static_cast<std::int64_t>(segment.end.output_frame)}};
  }

  const project::Snapshot* snapshot_;
  const project::Clip* clip_;
  const project::SourceRecord* source_;
  processing::ClipFramePlan plan_;
  std::int64_t start_ = 0;
  std::int64_t length_ = 0;
};

[[nodiscard]] std::int64_t marker_position(const project::ClipWarpMarker& marker) noexcept {
  if (const auto* tick = std::get_if<timeline::TickPosition>(&marker.target_offset))
    return tick->value;
  return std::get<timeline::SamplePosition>(marker.target_offset).value;
}

} // namespace

Result<ProcessedClipWindow> rewindow_processed_clip(const project::Snapshot& snapshot,
                                                    project::ClipId id, std::int64_t first,
                                                    std::int64_t last, bool keep_placement) {
  using Window = Result<ProcessedClipWindow>;
  auto opened = ExtendedClipMap::open(snapshot, id);
  if (!opened)
    return Window::failure(opened.error());
  const auto& map = opened.value();
  if (last <= first)
    return Window::failure({ErrorCode::invalid_argument, "The clip would be empty"});
  if (keep_placement && last - first != map.length())
    return Window::failure({ErrorCode::invalid_argument, "A slip must keep the clip's length"});
  if (!keep_placement && map.start() + first < 0)
    return Window::failure({ErrorCode::invalid_argument, "The clip would start before zero"});
  const auto first_output = map.output_at(first);
  const auto last_output = map.output_at(last);
  if (!first_output || !last_output)
    return Window::failure(!first_output ? first_output.error() : last_output.error());
  const auto source_first = map.source_at_output(first_output.value());
  const auto source_last = map.source_at_output(last_output.value());
  if (!source_first || !source_last)
    return Window::failure(!source_first ? source_first.error() : source_last.error());
  if (source_last.value() <= source_first.value())
    return Window::failure({ErrorCode::invalid_argument, "The clip would play no source audio"});

  ProcessedClipWindow window;
  const auto& clip = map.clip();
  window.processing = clip.processing;
  window.processing.reviewed_warp_markers.clear();
  for (const auto& marker : clip.processing.reviewed_warp_markers) {
    const auto position = marker_position(marker);
    if (position <= first || position >= last) {
      ++window.removed_markers;
      continue;
    }
    if (marker.source_frame.value <= source_first.value() ||
        marker.source_frame.value >= source_last.value())
      return Window::failure(
          {ErrorCode::conflict, "A clip edge would collide with a warp marker; move the edge"});
    auto rebased = marker;
    if (std::holds_alternative<timeline::TickPosition>(marker.target_offset))
      rebased.target_offset = timeline::TickPosition{position - first};
    else
      rebased.target_offset = timeline::SamplePosition{position - first};
    window.processing.reviewed_warp_markers.push_back(rebased);
  }
  window.source_range = {{source_first.value()}, {source_last.value()}};
  if (keep_placement) {
    window.placement = clip.placement;
  } else if (map.musical()) {
    window.placement = project::MusicalInterval{{map.start() + first}, {map.start() + last}};
  } else {
    window.placement = project::ElapsedInterval{{map.start() + first}, {map.start() + last}};
  }
  return Window::success(std::move(window));
}

Result<std::int64_t> processed_slip_offset(const project::Snapshot& snapshot, project::ClipId id,
                                           std::int64_t source_frame_delta) {
  auto opened = ExtendedClipMap::open(snapshot, id);
  if (!opened)
    return Result<std::int64_t>::failure(opened.error());
  const auto& map = opened.value();
  const auto start = map.clip().source_range.start.value;
  if ((source_frame_delta > 0 &&
       start > std::numeric_limits<std::int64_t>::max() - source_frame_delta) ||
      start + source_frame_delta < 0)
    return Result<std::int64_t>::failure(
        {ErrorCode::invalid_argument, "Slip would leave the source audio"});
  return map.position_at(map.output_at_source(start + source_frame_delta));
}

namespace {

[[nodiscard]] const project::SourceTempoReview* tempo_review(const project::Snapshot& snapshot,
                                                             project::SourceId source) {
  const auto found =
      std::ranges::find(snapshot.source_tempo_reviews, source, &project::SourceTempoReview::source);
  return found == snapshot.source_tempo_reviews.end() ? nullptr : &*found;
}

[[nodiscard]] double reviewed_bpm(const project::SourceTempoReview& review) noexcept {
  return review.corrected_bpm.value_or(review.detected_bpm);
}

} // namespace

Result<ClipWarpProposal> propose_auto_warp(const project::Snapshot& snapshot, project::ClipId id) {
  using Proposal = Result<ClipWarpProposal>;
  auto opened = ExtendedClipMap::open(snapshot, id);
  if (!opened)
    return Proposal::failure(opened.error());
  const auto& map = opened.value();
  const auto& clip = map.clip();
  if (!map.musical())
    return Proposal::failure({ErrorCode::invalid_argument,
                              "Auto-warp follows the beat grid: switch the clip to musical time"});
  const auto* review = tempo_review(snapshot, clip.source);
  if (review == nullptr ||
      (review->generated_beat_frames.empty() && review->user_beat_frames.empty()))
    return Proposal::failure(
        {ErrorCode::unavailable, "No reviewed beats for this source yet; analyse its tempo"});
  const auto first = static_cast<std::uint64_t>(clip.source_range.start.value);
  const auto last = static_cast<std::uint64_t>(clip.source_range.end.value);
  std::vector<std::uint64_t> beats;
  std::ranges::merge(review->user_beat_frames, review->generated_beat_frames,
                     std::back_inserter(beats));
  std::erase_if(beats, [&](std::uint64_t beat) { return beat <= first || beat >= last; });
  if (beats.size() < 2)
    return Proposal::failure(
        {ErrorCode::unavailable, "Auto-warp needs at least two reviewed beats inside the clip"});

  constexpr auto quarter = timeline::ticks_per_quarter;
  auto now = map.position_at(map.output_at_source(static_cast<std::int64_t>(beats.front())));
  if (!now)
    return Proposal::failure(now.error());
  // The first beat snaps to the nearest grid beat from where it plays now.
  const auto absolute = std::max<std::int64_t>(0, map.start() + now.value());
  auto first_target = (absolute + quarter / 2) / quarter * quarter - map.start();
  while (first_target <= 0)
    first_target += quarter;

  const auto gap = static_cast<std::uint64_t>(map.source().sample_rate) * 30 / 1'000;
  const auto& manual = clip.processing.reviewed_warp_markers;
  const auto near = [gap](std::uint64_t a, std::int64_t b) {
    const auto other = static_cast<std::uint64_t>(b);
    return (a > other ? a - other : other - a) < gap;
  };
  ClipWarpProposal proposal;
  proposal.settings_generation = clip.settings_generation;
  proposal.source_bpm = reviewed_bpm(*review);
  std::vector<project::ClipWarpMarker> markers;
  std::vector<std::uint64_t> kept_beats;
  std::int64_t previous_target = 0;
  std::size_t next = 0;
  bool last_is_beat = false;
  for (std::size_t index = 0; index < beats.size(); ++index) {
    const auto beat = beats[index];
    const auto target = first_target + static_cast<std::int64_t>(index) * quarter;
    // Reviewed markers before this beat stay exactly where they are.
    while (next < manual.size() &&
           static_cast<std::uint64_t>(manual[next].source_frame.value) < beat) {
      markers.push_back(manual[next]);
      previous_target = marker_position(manual[next]);
      last_is_beat = false;
      ++next;
    }
    const auto following = next < manual.size() ? marker_position(manual[next])
                                                : std::numeric_limits<std::int64_t>::max();
    const bool crowded = (next < manual.size() && near(beat, manual[next].source_frame.value)) ||
                         (next > 0 && near(beat, manual[next - 1].source_frame.value));
    if (crowded || target <= previous_target || target >= following) {
      ++proposal.skipped_beats;
      continue;
    }
    markers.push_back({{static_cast<std::int64_t>(beat)}, timeline::TickPosition{target}});
    kept_beats.push_back(beat);
    previous_target = target;
    last_is_beat = true;
    ++proposal.beat_markers;
  }
  for (; next < manual.size(); ++next) {
    markers.push_back(manual[next]);
    last_is_beat = false;
  }
  if (proposal.beat_markers == 0)
    return Proposal::failure(
        {ErrorCode::conflict, "No reviewed beat fits between the clip's warp markers"});

  auto length = map.length();
  if (last_is_beat) {
    // The clip end follows the last beat interval (or the reviewed BPM when
    // only one beat was kept).
    const auto beat = kept_beats.back();
    long double ticks_per_frame = 0.0L;
    if (kept_beats.size() >= 2)
      ticks_per_frame = static_cast<long double>(quarter) /
                        static_cast<long double>(beat - kept_beats[kept_beats.size() - 2]);
    else
      ticks_per_frame = static_cast<long double>(quarter) * proposal.source_bpm /
                        (60.0L * map.source().sample_rate);
    const auto tail = std::max<std::int64_t>(
        1, std::llround(static_cast<long double>(last - beat) * ticks_per_frame));
    length = previous_target + tail;
  }
  proposal.placement = project::MusicalInterval{{map.start()}, {map.start() + length}};
  proposal.processing = clip.processing;
  proposal.processing.reviewed_warp_markers = std::move(markers);
  return Proposal::success(std::move(proposal));
}

Result<ClipWarpProposal> propose_tempo_conform(const project::Snapshot& snapshot,
                                               project::ClipId id) {
  using Proposal = Result<ClipWarpProposal>;
  auto opened = ExtendedClipMap::open(snapshot, id);
  if (!opened)
    return Proposal::failure(opened.error());
  const auto& map = opened.value();
  const auto& clip = map.clip();
  if (!map.musical())
    return Proposal::failure({ErrorCode::invalid_argument,
                              "Tempo conform follows the beat grid: switch the clip to musical "
                              "time"});
  const auto* review = tempo_review(snapshot, clip.source);
  const auto bpm = review == nullptr ? 0.0 : reviewed_bpm(*review);
  if (bpm <= 0.0)
    return Proposal::failure(
        {ErrorCode::unavailable, "No reviewed tempo for this source yet; analyse its tempo"});
  const auto frames =
      static_cast<long double>(clip.source_range.end.value - clip.source_range.start.value);
  const auto length =
      std::llround(frames * bpm * timeline::ticks_per_quarter / (60.0L * map.source().sample_rate));
  if (length <= 0)
    return Proposal::failure({ErrorCode::invalid_argument, "The conformed clip would be empty"});
  ClipWarpProposal proposal;
  proposal.settings_generation = clip.settings_generation;
  proposal.source_bpm = bpm;
  proposal.placement = project::MusicalInterval{{map.start()}, {map.start() + length}};
  proposal.processing = clip.processing;
  std::int64_t previous = 0;
  for (auto& marker : proposal.processing.reviewed_warp_markers) {
    const auto scaled = std::llround(static_cast<long double>(marker_position(marker)) * length /
                                     static_cast<long double>(map.length()));
    if (scaled <= previous || scaled >= length)
      return Proposal::failure(
          {ErrorCode::conflict, "Warp markers are too close together to conform"});
    marker.target_offset = timeline::TickPosition{scaled};
    previous = scaled;
  }
  return Proposal::success(std::move(proposal));
}

namespace {

// The bar line nearest `tick` under the project's meter map.
[[nodiscard]] Result<std::int64_t> nearest_bar(const project::Snapshot& snapshot,
                                               std::int64_t tick) {
  auto located = snapshot.meter.locate({std::max<std::int64_t>(0, tick)});
  if (!located)
    return Result<std::int64_t>::failure(located.error());
  const auto bar = located.value().bar;
  auto here = snapshot.meter.tick_at({bar, 1, 0});
  auto next = snapshot.meter.tick_at({bar + 1, 1, 0});
  if (!here || !next)
    return Result<std::int64_t>::failure(!here ? here.error() : next.error());
  return Result<std::int64_t>::success(tick - here.value().value <= next.value().value - tick
                                           ? here.value().value
                                           : next.value().value);
}

// The first downbeat at or after the clip's source start.
[[nodiscard]] std::uint64_t first_downbeat(const SourceBeatGrid& grid, std::uint64_t start) {
  const auto bar = source_beats_per_bar * grid.period();
  const auto anchor = static_cast<double>(grid.downbeat);
  const auto at = anchor + std::ceil((static_cast<double>(start) - anchor) / bar) * bar;
  return at <= 0.0 ? 0 : static_cast<std::uint64_t>(std::llround(at));
}

} // namespace

std::optional<SourceBeatGrid> source_beat_grid(const project::Snapshot& snapshot,
                                               project::SourceId id) {
  const auto* review = tempo_review(snapshot, id);
  const auto source = std::ranges::find(snapshot.sources, id, &project::SourceRecord::id);
  if (review == nullptr || source == snapshot.sources.end())
    return std::nullopt;
  const auto bpm = reviewed_bpm(*review);
  if (bpm <= 0.0)
    return std::nullopt;
  SourceBeatGrid grid{bpm, source->sample_rate, 0};
  const auto bars = static_cast<double>(source->frames) / grid.period() / source_beats_per_bar;
  const bool loop = bars >= 1.0 && std::abs(bars - std::round(bars)) * source_beats_per_bar <=
                                       loop_bar_tolerance_beats;
  if (review->downbeat_frame)
    grid.downbeat = *review->downbeat_frame;
  else if (!loop && !review->generated_beat_frames.empty())
    grid.downbeat = review->generated_beat_frames.front();
  return grid;
}

namespace {

// The absolute project tick at which the clip plays an absolute source frame.
[[nodiscard]] Result<std::int64_t> tick_of_source(const project::Snapshot& snapshot,
                                                  const ExtendedClipMap& map, std::uint64_t frame) {
  auto position = map.position_at(map.output_at_source(static_cast<std::int64_t>(frame)));
  if (!position)
    return Result<std::int64_t>::failure(position.error());
  if (map.musical())
    return Result<std::int64_t>::success(map.start() + position.value());
  auto tick = snapshot.tempo.sample_to_tick(
      {std::max<std::int64_t>(0, map.start() + position.value())}, snapshot.sample_rate);
  if (!tick)
    return Result<std::int64_t>::failure(tick.error());
  return Result<std::int64_t>::success(tick.value().value);
}

} // namespace

Result<std::int64_t> clip_downbeat_tick(const project::Snapshot& snapshot, project::ClipId id) {
  auto opened = ExtendedClipMap::open(snapshot, id);
  if (!opened)
    return Result<std::int64_t>::failure(opened.error());
  const auto& map = opened.value();
  const auto grid = source_beat_grid(snapshot, map.clip().source);
  if (!grid)
    return Result<std::int64_t>::failure(
        {ErrorCode::unavailable, "No tempo for this clip's audio yet; it is still analysing"});
  return tick_of_source(
      snapshot, map,
      first_downbeat(*grid, static_cast<std::uint64_t>(map.clip().source_range.start.value)));
}

Result<ClipWarpProposal> propose_follow(const project::Snapshot& snapshot, project::ClipId id,
                                        std::optional<double> bpm_override,
                                        std::optional<std::uint64_t> downbeat_override,
                                        std::optional<std::int64_t> downbeat_tick) {
  using Proposal = Result<ClipWarpProposal>;
  auto opened = ExtendedClipMap::open(snapshot, id);
  if (!opened)
    return Proposal::failure(opened.error());
  const auto& map = opened.value();
  const auto& clip = map.clip();
  auto grid = source_beat_grid(snapshot, clip.source);
  if (!grid && !bpm_override)
    return Proposal::failure(
        {ErrorCode::unavailable, "No tempo for this clip's audio yet; it is still analysing"});
  if (!grid) {
    const auto source =
        std::ranges::find(snapshot.sources, clip.source, &project::SourceRecord::id);
    grid = SourceBeatGrid{0.0, source->sample_rate, 0};
  }
  if (bpm_override)
    grid->bpm = *bpm_override;
  if (downbeat_override)
    grid->downbeat = *downbeat_override;
  if (!(grid->bpm > 0.0) || !std::isfinite(grid->bpm))
    return Proposal::failure({ErrorCode::invalid_argument, "A tempo must be above 0 BPM"});
  const auto first = static_cast<std::uint64_t>(clip.source_range.start.value);
  const auto last = static_cast<std::uint64_t>(clip.source_range.end.value);
  const auto frames = static_cast<long double>(last - first);
  const auto length =
      std::llround(frames * grid->bpm * timeline::ticks_per_quarter / (60.0L * grid->sample_rate));
  if (length <= 0)
    return Proposal::failure({ErrorCode::invalid_argument, "The clip would be empty"});

  // Where the downbeat sits within the clip, as a share of its length: the
  // warp map's, so reviewed markers keep the audio they hold.
  const auto downbeat = first_downbeat(*grid, first);
  const auto output_frames = static_cast<long double>(
      std::max<std::int64_t>(1, map.output_at_source(static_cast<std::int64_t>(last)) -
                                    map.output_at_source(static_cast<std::int64_t>(first))));
  const auto share =
      static_cast<long double>(map.output_at_source(static_cast<std::int64_t>(downbeat)) -
                               map.output_at_source(static_cast<std::int64_t>(first))) /
      output_frames;
  const auto offset = std::llround(share * static_cast<long double>(length));

  // The downbeat lands on the given tick, or on the bar nearest where it
  // plays now.
  std::int64_t target = 0;
  if (downbeat_tick) {
    target = *downbeat_tick;
  } else {
    auto now = tick_of_source(snapshot, map, downbeat);
    if (!now)
      return Proposal::failure(now.error());
    auto bar = nearest_bar(snapshot, now.value());
    if (!bar)
      return Proposal::failure(bar.error());
    target = bar.value();
  }
  // May be before zero: the caller then adds bars before bar 1.
  const auto start = target - offset;

  ClipWarpProposal proposal;
  proposal.settings_generation = clip.settings_generation;
  proposal.source_bpm = grid->bpm;
  proposal.placement = project::MusicalInterval{{start}, {start + length}};
  proposal.processing = clip.processing;
  std::int64_t previous = 0;
  for (auto& marker : proposal.processing.reviewed_warp_markers) {
    const auto at = map.output_at_source(marker.source_frame.value) -
                    map.output_at_source(static_cast<std::int64_t>(first));
    const auto scaled = std::llround(static_cast<long double>(at) * length / output_frames);
    if (scaled <= previous || scaled >= length)
      return Proposal::failure(
          {ErrorCode::conflict, "Warp markers are too close together to follow the tempo"});
    marker.target_offset = timeline::TickPosition{scaled};
    previous = scaled;
  }
  return Proposal::success(std::move(proposal));
}

Result<std::uint64_t> source_frame_at_fraction(const project::Snapshot& snapshot,
                                               project::ClipId id, double fraction) {
  auto opened = ExtendedClipMap::open(snapshot, id);
  if (!opened)
    return Result<std::uint64_t>::failure(opened.error());
  const auto& map = opened.value();
  const auto first = map.output_at_source(map.clip().source_range.start.value);
  const auto last = map.output_at_source(map.clip().source_range.end.value);
  const auto output =
      first + std::llround(std::clamp(fraction, 0.0, 1.0) * static_cast<double>(last - first));
  auto source = map.source_at_output(output);
  if (!source)
    return Result<std::uint64_t>::failure(source.error());
  return Result<std::uint64_t>::success(static_cast<std::uint64_t>(source.value()));
}

Result<double> fraction_of_source_frame(const project::Snapshot& snapshot, project::ClipId id,
                                        std::uint64_t source_frame) {
  auto opened = ExtendedClipMap::open(snapshot, id);
  if (!opened)
    return Result<double>::failure(opened.error());
  const auto& map = opened.value();
  const auto first = map.output_at_source(map.clip().source_range.start.value);
  const auto last = map.output_at_source(map.clip().source_range.end.value);
  if (last <= first)
    return Result<double>::failure({ErrorCode::invalid_argument, "The clip is empty"});
  return Result<double>::success(
      static_cast<double>(map.output_at_source(static_cast<std::int64_t>(source_frame)) - first) /
      static_cast<double>(last - first));
}

Result<ClipWarpProposal> propose_own_tempo(const project::Snapshot& snapshot, project::ClipId id) {
  using Proposal = Result<ClipWarpProposal>;
  auto opened = ExtendedClipMap::open(snapshot, id);
  if (!opened)
    return Proposal::failure(opened.error());
  const auto& map = opened.value();
  const auto& clip = map.clip();
  std::int64_t start = map.start();
  if (map.musical()) {
    auto sample = snapshot.tempo.tick_to_sample({map.start()}, snapshot.sample_rate);
    if (!sample)
      return Proposal::failure(sample.error());
    start = sample.value().value;
  }
  const auto& source = map.source();
  const auto frames =
      static_cast<long double>(clip.source_range.end.value - clip.source_range.start.value);
  const auto length = std::llround(frames * snapshot.sample_rate / source.sample_rate);
  if (length <= 0)
    return Proposal::failure({ErrorCode::invalid_argument, "The clip would be empty"});
  const auto first = clip.source_range.start.value;
  const auto output_frames = static_cast<long double>(std::max<std::int64_t>(
      1, map.output_at_source(clip.source_range.end.value) - map.output_at_source(first)));
  ClipWarpProposal proposal;
  proposal.settings_generation = clip.settings_generation;
  proposal.placement = project::ElapsedInterval{{start}, {start + length}};
  proposal.processing = clip.processing;
  if (const auto grid = source_beat_grid(snapshot, clip.source))
    proposal.source_bpm = grid->bpm;
  std::int64_t previous = 0;
  for (auto& marker : proposal.processing.reviewed_warp_markers) {
    const auto at = map.output_at_source(marker.source_frame.value) - map.output_at_source(first);
    const auto scaled = std::llround(static_cast<long double>(at) * length / output_frames);
    if (scaled <= previous || scaled >= length)
      return Proposal::failure(
          {ErrorCode::conflict, "Warp markers are too close together to play in real time"});
    marker.target_offset = timeline::SamplePosition{scaled};
    previous = scaled;
  }
  return Proposal::success(std::move(proposal));
}

TransposeParts split_transpose(double semitones) noexcept {
  // Nearest semitone with ties toward zero, so cents stay within -50..50
  // and +/-50 cents is shown as 0 semitones rather than +/-1 semitone.
  const auto cents = processing::clip_transpose_cents(semitones);
  int whole = cents / 100;
  int rest = cents - whole * 100;
  if (rest > 50) {
    ++whole;
    rest -= 100;
  } else if (rest < -50) {
    --whole;
    rest += 100;
  }
  return {whole, rest};
}

double join_transpose(int semitones, int cents) noexcept {
  return static_cast<double>(semitones * 100 + cents) / 100.0;
}

} // namespace oscillate::application
