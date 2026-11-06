#include "oscillate/application/session/audio_clip_service.hpp"

#include "edit_support.hpp"
#include "midi_clips.hpp"
#include "oscillate/application/clip_warp_edit.hpp"
#include "oscillate/application/key_cache.hpp"
#include "oscillate/application/midi_note_tools.hpp"
#include "oscillate/application/tempo_cache.hpp"
#include "oscillate/automation/curve.hpp"
#include "oscillate/media/source_rate_reader.hpp"
#include "oscillate/midi_file/smf.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/processing/warp_render_cache.hpp"
#include "oscillate/project/clip_fade.hpp"
#include "oscillate/project/lookup.hpp"
#include "session_state.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::application {
namespace {

using ChannelKey = project::TrackControlTarget::Key;
using detail::find_midi_clip;
using detail::midi_anchor_tick;
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;
using project::find_clip;
using Follow = std::pair<const project::Clip*, ClipWarpProposal>;

[[nodiscard]] foundation::Result<std::int64_t>
source_frame_at_project_frame(const project::SourceRecord& source, std::uint32_t project_rate,
                              std::uint64_t target) {
  using foundation::ErrorCode;
  auto total = media::source_rate_frame_count(source.frames, source.sample_rate, project_rate);
  if (!total)
    return foundation::Result<std::int64_t>::failure(total.error());
  if (target > total.value())
    return foundation::Result<std::int64_t>::failure(
        {ErrorCode::invalid_argument, "Audio trim would leave the source media"});
  const auto whole = target / project_rate;
  const auto remainder = target % project_rate;
  if (whole <= std::numeric_limits<std::uint64_t>::max() / source.sample_rate) {
    const auto base = whole * source.sample_rate;
    const auto fraction = (remainder * source.sample_rate + project_rate / 2U) / project_rate;
    if (fraction <= std::numeric_limits<std::uint64_t>::max() - base) {
      const auto nearest = base + fraction;
      if (nearest <= source.frames &&
          nearest <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        auto mapped = media::source_rate_frame_count(nearest, source.sample_rate, project_rate);
        if (!mapped)
          return foundation::Result<std::int64_t>::failure(mapped.error());
        if (mapped.value() == target)
          return foundation::Result<std::int64_t>::success(static_cast<std::int64_t>(nearest));
      }
    }
  }
  std::uint64_t low = 0;
  std::uint64_t high = source.frames;
  while (low < high) {
    const auto middle = low + (high - low) / 2;
    auto mapped = media::source_rate_frame_count(middle, source.sample_rate, project_rate);
    if (!mapped)
      return foundation::Result<std::int64_t>::failure(mapped.error());
    if (mapped.value() < target)
      low = middle + 1;
    else
      high = middle;
  }
  auto mapped = media::source_rate_frame_count(low, source.sample_rate, project_rate);
  if (!mapped)
    return foundation::Result<std::int64_t>::failure(mapped.error());
  if (mapped.value() != target)
    return foundation::Result<std::int64_t>::failure(
        {ErrorCode::unsupported, "Audio trim edge falls between source-rate frames"});
  if (low > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return foundation::Result<std::int64_t>::failure(
        {ErrorCode::resource_exhausted, "Audio source frame exceeds clip range"});
  return foundation::Result<std::int64_t>::success(static_cast<std::int64_t>(low));
}

// Placement length in its own timebase (ticks or project samples).
[[nodiscard]] std::int64_t placement_length(const project::Placement& placement) noexcept {
  if (const auto* musical = std::get_if<project::MusicalInterval>(&placement))
    return musical->end.value - musical->start.value;
  const auto& elapsed = std::get<project::ElapsedInterval>(placement);
  return elapsed.end.value - elapsed.start.value;
}

void apply_window(std::vector<project::Operation>& operations, const project::Clip& clip,
                  const ProcessedClipWindow& window) {
  if (window.placement != clip.placement)
    operations.emplace_back(project::SetClipPlacement{clip.id, window.placement});
  if (window.source_range != clip.source_range)
    operations.emplace_back(project::SetClipSourceRange{clip.id, window.source_range});
  if (window.processing != clip.processing)
    operations.emplace_back(project::SetClipProcessing{clip.id, window.processing});
}

[[nodiscard]] Result<void> keeps_markers(const ProcessedClipWindow& window, std::string_view edit) {
  if (window.removed_markers == 0)
    return Result<void>::success();
  return Result<void>::failure(
      {ErrorCode::conflict,
       std::string(edit) + " would leave " + std::to_string(window.removed_markers) +
           " reviewed warp marker(s) outside the clip; move or delete them first"});
}

} // namespace

foundation::Result<void>
AudioClipService::move_audio_clip_to_track(project::ClipId clip, project::TrackId destination,
                                           timeline::TickPosition new_start) {
  const auto& state = snapshot();
  const auto target = std::ranges::find(state.tracks, destination, &project::Track::id);
  if (target == state.tracks.end())
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::not_found, "Destination track not found"});
  if (target->kind != project::TrackKind::audio)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::invalid_argument, "Audio clips go on audio tracks"});
  for (const auto& track : state.tracks) {
    const auto found = std::ranges::find(track.clips, clip, &project::Clip::id);
    if (found == track.clips.end())
      continue;
    if (track.id == destination)
      return move_audio_clip(clip, new_start);
    if (new_start.value < 0)
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::invalid_argument,
           "Move the clip before bar 1 on its own track first"});
    auto placement = audio_placement_at(found->placement, new_start, state);
    if (!placement)
      return foundation::Result<void>::failure(placement.error());
    auto moved = *found;
    moved.placement = std::move(placement).value();
    auto applied =
        model().apply({project::RemoveClip{clip}, project::AddClip{destination, std::move(moved)}});
    if (!applied)
      return foundation::Result<void>::failure(applied.error());
    return foundation::Result<void>::success();
  }
  return foundation::Result<void>::failure(
      {foundation::ErrorCode::not_found, "Audio clip not found: " + clip.to_string()});
}

foundation::Result<void> AudioClipService::move_audio_clip(project::ClipId clip,
                                                           timeline::TickPosition new_start) {
  if (new_start.value < 0)
    return move_audio_clip_before_zero(clip, new_start);
  for (const auto& track : snapshot().tracks) {
    const auto found =
        std::ranges::find_if(track.clips, [clip](const auto& item) { return item.id == clip; });
    if (found == track.clips.end()) {
      continue;
    }
    auto placement = audio_placement_at(found->placement, new_start, snapshot());
    if (!placement) {
      return foundation::Result<void>::failure(placement.error());
    }
    if (placement.value() == found->placement) {
      return foundation::Result<void>::success();
    }
    auto applied = model().apply({project::SetClipPlacement{clip, std::move(placement).value()}});
    if (!applied) {
      return foundation::Result<void>::failure(applied.error());
    }
    return foundation::Result<void>::success();
  }
  return foundation::Result<void>::failure(
      {foundation::ErrorCode::not_found, "Audio clip not found: " + clip.to_string()});
}

foundation::Result<project::ClipId>
AudioClipService::duplicate_audio_clip(project::ClipId clip, timeline::TickPosition new_start) {
  for (const auto& track : snapshot().tracks) {
    const auto found =
        std::ranges::find_if(track.clips, [clip](const auto& item) { return item.id == clip; });
    if (found == track.clips.end()) {
      continue;
    }
    auto id = numbered_id<project::ClipId>(next_clip_number());
    if (!id) {
      return foundation::Result<project::ClipId>::failure(id.error());
    }
    auto placement = audio_placement_at(found->placement, new_start, snapshot());
    if (!placement) {
      return foundation::Result<project::ClipId>::failure(placement.error());
    }
    project::Clip copy = *found;
    copy.id = id.value();
    copy.placement = std::move(placement).value();
    auto identities = assign_fresh_automation_ids(copy.automation, snapshot().revision);
    if (!identities)
      return foundation::Result<project::ClipId>::failure(identities.error());
    auto applied = model().apply({project::AddClip{track.id, copy}});
    if (!applied) {
      return foundation::Result<project::ClipId>::failure(applied.error());
    }
    return foundation::Result<project::ClipId>::success(id.value());
  }
  return foundation::Result<project::ClipId>::failure(
      {foundation::ErrorCode::not_found, "Audio clip not found: " + clip.to_string()});
}

foundation::Result<project::ClipId>
AudioClipService::duplicate_audio_clip_after(project::ClipId clip) {
  for (const auto& track : snapshot().tracks) {
    const auto found =
        std::ranges::find_if(track.clips, [clip](const auto& item) { return item.id == clip; });
    if (found == track.clips.end())
      continue;
    auto id = numbered_id<project::ClipId>(next_clip_number());
    if (!id)
      return foundation::Result<project::ClipId>::failure(id.error());
    project::Clip copy = *found;
    copy.id = id.value();
    auto identities = assign_fresh_automation_ids(copy.automation, snapshot().revision);
    if (!identities)
      return foundation::Result<project::ClipId>::failure(identities.error());
    if (const auto* musical = std::get_if<project::MusicalInterval>(&found->placement)) {
      const auto length = musical->end.value - musical->start.value;
      if (musical->end.value > std::numeric_limits<std::int64_t>::max() - length)
        return foundation::Result<project::ClipId>::failure(
            {foundation::ErrorCode::invalid_argument, "Duplicated clip exceeds the timeline"});
      copy.placement = project::MusicalInterval{musical->end, {musical->end.value + length}};
    } else {
      const auto& elapsed = std::get<project::ElapsedInterval>(found->placement);
      const auto length = elapsed.end.value - elapsed.start.value;
      if (elapsed.end.value > std::numeric_limits<std::int64_t>::max() - length)
        return foundation::Result<project::ClipId>::failure(
            {foundation::ErrorCode::invalid_argument, "Duplicated clip exceeds the timeline"});
      copy.placement = project::ElapsedInterval{elapsed.end, {elapsed.end.value + length}};
    }
    auto applied = model().apply({project::AddClip{track.id, std::move(copy)}});
    if (!applied)
      return foundation::Result<project::ClipId>::failure(applied.error());
    return foundation::Result<project::ClipId>::success(id.value());
  }
  return foundation::Result<project::ClipId>::failure(
      {foundation::ErrorCode::not_found, "Audio clip not found: " + clip.to_string()});
}

foundation::Result<project::ClipId>
AudioClipService::split_audio_clip_at_sample(project::ClipId clip,
                                             timeline::SamplePosition boundary) {
  using foundation::ErrorCode;
  const auto& state = snapshot();
  for (const auto& track : state.tracks) {
    const auto found = std::ranges::find(track.clips, clip, &project::Clip::id);
    if (found == track.clips.end())
      continue;
    const auto processed = audio_clip_is_processed(clip);
    if (!processed)
      return foundation::Result<project::ClipId>::failure(processed.error());
    std::int64_t split_offset = 0; // in the placement's timebase

    timeline::SamplePosition first;
    timeline::SamplePosition last;
    project::Placement left_placement = found->placement;
    project::Placement right_placement = found->placement;
    if (const auto* musical = std::get_if<project::MusicalInterval>(&found->placement)) {
      auto start = state.tempo.tick_to_sample(musical->start, state.sample_rate);
      auto end = state.tempo.tick_to_sample(musical->end, state.sample_rate);
      if (!start || !end)
        return foundation::Result<project::ClipId>::failure(!start ? start.error() : end.error());
      first = start.value();
      last = end.value();
      if (boundary <= first || boundary >= last)
        return foundation::Result<project::ClipId>::failure(
            {ErrorCode::invalid_argument, "Split must be inside the audio clip"});
      auto tick = state.tempo.sample_to_tick(boundary, state.sample_rate);
      if (!tick)
        return foundation::Result<project::ClipId>::failure(tick.error());
      auto round_trip = state.tempo.tick_to_sample(tick.value(), state.sample_rate);
      if (!round_trip || round_trip.value() != boundary)
        return foundation::Result<project::ClipId>::failure(
            {ErrorCode::invalid_argument, "Split must align exactly with a tempo-map tick"});
      std::get<project::MusicalInterval>(left_placement).end = tick.value();
      std::get<project::MusicalInterval>(right_placement).start = tick.value();
      split_offset = tick.value().value - musical->start.value;
    } else {
      const auto& elapsed = std::get<project::ElapsedInterval>(found->placement);
      first = elapsed.start;
      last = elapsed.end;
      if (boundary <= first || boundary >= last)
        return foundation::Result<project::ClipId>::failure(
            {ErrorCode::invalid_argument, "Split must be inside the audio clip"});
      std::get<project::ElapsedInterval>(left_placement).end = boundary;
      std::get<project::ElapsedInterval>(right_placement).start = boundary;
      split_offset = boundary.value - first.value;
    }
    const auto duration = last.value - first.value;
    const auto source = std::ranges::find(state.sources, found->source, &project::SourceRecord::id);
    std::uint64_t source_first_project = 0;
    if (processed.value()) {
      // The warp map, not the source rate, decides the source boundary.
    } else if (source != state.sources.end()) {
      auto mapped_first = media::source_rate_frame_count(
          static_cast<std::uint64_t>(found->source_range.start.value), source->sample_rate,
          state.sample_rate);
      auto mapped_last =
          media::source_rate_frame_count(static_cast<std::uint64_t>(found->source_range.end.value),
                                         source->sample_rate, state.sample_rate);
      if (!mapped_first || !mapped_last)
        return foundation::Result<project::ClipId>::failure(!mapped_first ? mapped_first.error()
                                                                          : mapped_last.error());
      source_first_project = mapped_first.value();
      if (mapped_last.value() <= source_first_project ||
          mapped_last.value() - source_first_project != static_cast<std::uint64_t>(duration))
        return foundation::Result<project::ClipId>::failure(
            {ErrorCode::unsupported, "Split requires an unstretched clip"});
    } else if (found->source_range.end.value - found->source_range.start.value != duration) {
      return foundation::Result<project::ClipId>::failure(
          {ErrorCode::unsupported, "Split requires source metadata for rate-converted media"});
    }
    const auto left_frames = static_cast<std::uint64_t>(boundary.value - first.value);
    const auto right_frames = static_cast<std::uint64_t>(last.value - boundary.value);
    auto original_fades =
        project::prepare_clip_fades(found->fades, static_cast<std::uint64_t>(duration));
    if (!original_fades)
      return foundation::Result<project::ClipId>::failure(original_fades.error());
    if (left_frames < original_fades.value().in_frames ||
        right_frames < original_fades.value().out_frames)
      return foundation::Result<project::ClipId>::failure(
          {ErrorCode::unsupported, "Split inside an active fade would change its sound"});

    auto left_fades = found->fades;
    auto right_fades = found->fades;
    left_fades.in_fraction =
        static_cast<double>(original_fades.value().in_frames) / static_cast<double>(left_frames);
    left_fades.out_fraction = 0.0;
    right_fades.in_fraction = 0.0;
    right_fades.out_fraction =
        static_cast<double>(original_fades.value().out_frames) / static_cast<double>(right_frames);
    auto prepared_left = project::prepare_clip_fades(left_fades, left_frames);
    auto prepared_right = project::prepare_clip_fades(right_fades, right_frames);
    if (!prepared_left || !prepared_right ||
        prepared_left.value().in_frames != original_fades.value().in_frames ||
        prepared_right.value().out_frames != original_fades.value().out_frames)
      return foundation::Result<project::ClipId>::failure(
          {ErrorCode::unsupported, "Split cannot retain exact fade frame boundaries"});

    auto id = numbered_id<project::ClipId>(next_clip_number());
    if (!id)
      return foundation::Result<project::ClipId>::failure(id.error());
    const auto first_tick = state.tempo.sample_to_tick(first, state.sample_rate);
    const auto boundary_tick = state.tempo.sample_to_tick(boundary, state.sample_rate);
    if (!first_tick || !boundary_tick)
      return foundation::Result<project::ClipId>::failure(!first_tick ? first_tick.error()
                                                                      : boundary_tick.error());
    auto automation = split_clip_automation(state, found->automation,
                                            boundary_tick.value().value - first_tick.value().value,
                                            boundary.value - first.value);
    if (!automation)
      return foundation::Result<project::ClipId>::failure(automation.error());
    if (processed.value())
      return split_processed_audio_clip(clip, split_offset, left_fades, right_fades, id.value(),
                                        std::move(automation.value().left),
                                        std::move(automation.value().right));
    std::int64_t source_mid = 0;
    if (source != state.sources.end()) {
      if (left_frames > std::numeric_limits<std::uint64_t>::max() - source_first_project)
        return foundation::Result<project::ClipId>::failure(
            {ErrorCode::resource_exhausted, "Split source boundary exceeds frame range"});
      auto mapped = source_frame_at_project_frame(*source, state.sample_rate,
                                                  source_first_project + left_frames);
      if (!mapped)
        return foundation::Result<project::ClipId>::failure(mapped.error());
      source_mid = mapped.value();
    } else {
      source_mid = found->source_range.start.value + static_cast<std::int64_t>(left_frames);
    }
    if (source_mid <= found->source_range.start.value ||
        source_mid >= found->source_range.end.value)
      return foundation::Result<project::ClipId>::failure(
          {ErrorCode::unsupported, "Split would create an empty source segment"});
    auto left_range = found->source_range;
    left_range.end.value = source_mid;
    project::Clip right = *found;
    right.id = id.value();
    right.source_range.start.value = source_mid;
    right.placement = right_placement;
    right.fades = right_fades;
    right.automation = std::move(automation.value().right);
    std::vector<project::Operation> operations;
    operations.emplace_back(project::SetClipPlacement{clip, left_placement});
    operations.emplace_back(project::SetClipSourceRange{clip, left_range});
    if (left_fades != found->fades)
      operations.emplace_back(project::SetClipFades{clip, left_fades});
    for (std::size_t index = 0; index < found->automation.size(); ++index) {
      auto& lane = automation.value().left[index];
      if (lane != found->automation[index])
        operations.emplace_back(project::ReplaceAutomationLane{lane.id, std::move(lane)});
    }
    operations.emplace_back(project::AddClip{track.id, std::move(right)});
    auto applied = model().apply(operations);
    if (!applied)
      return foundation::Result<project::ClipId>::failure(applied.error());
    return foundation::Result<project::ClipId>::success(id.value());
  }
  return foundation::Result<project::ClipId>::failure(
      {foundation::ErrorCode::not_found, "Audio clip not found: " + clip.to_string()});
}

foundation::Result<void> AudioClipService::slip_audio_clip(project::ClipId clip,
                                                           std::int64_t source_frame_delta) {
  using foundation::ErrorCode;
  const auto& state = snapshot();
  for (const auto& track : state.tracks) {
    const auto found = std::ranges::find(track.clips, clip, &project::Clip::id);
    if (found == track.clips.end())
      continue;
    if (source_frame_delta == 0)
      return foundation::Result<void>::success();
    const auto processed = audio_clip_is_processed(clip);
    if (!processed)
      return foundation::Result<void>::failure(processed.error());
    if (processed.value())
      return slip_processed_audio_clip(clip, source_frame_delta);
    const auto source = std::ranges::find(state.sources, found->source, &project::SourceRecord::id);
    if (source == state.sources.end())
      return foundation::Result<void>::failure(
          {ErrorCode::unavailable, "Cannot slip audio without source metadata"});
    if (source_frame_delta == std::numeric_limits<std::int64_t>::min())
      return foundation::Result<void>::failure(
          {ErrorCode::invalid_argument, "Slip would leave the source audio"});
    const auto first = found->source_range.start.value;
    const auto last = found->source_range.end.value;
    if ((source_frame_delta < 0 && first < -source_frame_delta) ||
        (source_frame_delta > 0 &&
         last > std::numeric_limits<std::int64_t>::max() - source_frame_delta))
      return foundation::Result<void>::failure(
          {ErrorCode::invalid_argument, "Slip would leave the source audio"});
    const auto moved_first = first + source_frame_delta;
    const auto moved_last = last + source_frame_delta;
    if (moved_first < 0 || static_cast<std::uint64_t>(moved_last) > source->frames)
      return foundation::Result<void>::failure(
          {ErrorCode::invalid_argument, "Slip would leave the source audio"});
    timeline::SamplePosition placed_first;
    timeline::SamplePosition placed_last;
    if (const auto* musical = std::get_if<project::MusicalInterval>(&found->placement)) {
      auto start = state.tempo.tick_to_sample(musical->start, state.sample_rate);
      auto end = state.tempo.tick_to_sample(musical->end, state.sample_rate);
      if (!start || !end)
        return foundation::Result<void>::failure(!start ? start.error() : end.error());
      placed_first = start.value();
      placed_last = end.value();
    } else {
      const auto& elapsed = std::get<project::ElapsedInterval>(found->placement);
      placed_first = elapsed.start;
      placed_last = elapsed.end;
    }
    auto mapped_first = media::source_rate_frame_count(static_cast<std::uint64_t>(moved_first),
                                                       source->sample_rate, state.sample_rate);
    auto mapped_last = media::source_rate_frame_count(static_cast<std::uint64_t>(moved_last),
                                                      source->sample_rate, state.sample_rate);
    if (!mapped_first || !mapped_last)
      return foundation::Result<void>::failure(!mapped_first ? mapped_first.error()
                                                             : mapped_last.error());
    if (mapped_last.value() <= mapped_first.value() ||
        mapped_last.value() - mapped_first.value() !=
            static_cast<std::uint64_t>(placed_last.value - placed_first.value))
      return foundation::Result<void>::failure(
          {ErrorCode::unsupported, "Slip would change rendered duration at this source rate"});
    auto applied = model().apply(
        {project::SetClipSourceRange{clip, project::SourceRange{{moved_first}, {moved_last}}}});
    if (!applied)
      return foundation::Result<void>::failure(applied.error());
    return foundation::Result<void>::success();
  }
  return foundation::Result<void>::failure(
      {foundation::ErrorCode::not_found, "Audio clip not found: " + clip.to_string()});
}

foundation::Result<void> AudioClipService::toggle_audio_clip_timebase(project::ClipId clip) {
  const auto& state = snapshot();
  for (const auto& track : state.tracks) {
    const auto found = std::ranges::find(track.clips, clip, &project::Clip::id);
    if (found == track.clips.end())
      continue;
    auto placement = convert_clip_timebase(state, found->placement);
    if (!placement)
      return foundation::Result<void>::failure(placement.error());
    auto applied = model().apply({project::SetClipPlacement{clip, placement.value()}});
    if (!applied)
      return foundation::Result<void>::failure(applied.error());
    return foundation::Result<void>::success();
  }
  return foundation::Result<void>::failure(
      {foundation::ErrorCode::not_found, "Audio clip not found: " + clip.to_string()});
}

foundation::Result<void> AudioClipService::remove_audio_clip(project::ClipId clip) {
  const auto found = std::ranges::any_of(snapshot().tracks, [clip](const auto& track) {
    return std::ranges::any_of(track.clips, [clip](const auto& item) { return item.id == clip; });
  });
  if (!found)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::not_found, "Audio clip not found: " + clip.to_string()});
  auto applied = model().apply({project::RemoveClip{clip}});
  if (!applied)
    return foundation::Result<void>::failure(applied.error());
  return foundation::Result<void>::success();
}

foundation::Result<void> AudioClipService::trim_audio_clip(project::ClipId clip, AudioClipEdge edge,
                                                           timeline::SamplePosition new_edge) {
  const foundation::Error invalid{foundation::ErrorCode::invalid_argument,
                                  "Audio trim edge is out of range"};
  if ((edge != AudioClipEdge::left && edge != AudioClipEdge::right) || new_edge.value < 0) {
    return foundation::Result<void>::failure(invalid);
  }
  const auto& state = snapshot();
  for (const auto& track : state.tracks) {
    const auto found =
        std::ranges::find_if(track.clips, [clip](const auto& item) { return item.id == clip; });
    if (found == track.clips.end()) {
      continue;
    }
    const auto processed = audio_clip_is_processed(clip);
    if (!processed)
      return foundation::Result<void>::failure(processed.error());
    if (processed.value())
      return trim_processed_audio_clip(clip, edge, new_edge);
    auto placement = found->placement;
    timeline::SamplePosition first;
    timeline::SamplePosition last;
    if (const auto* musical = std::get_if<project::MusicalInterval>(&placement)) {
      auto start = state.tempo.tick_to_sample(musical->start, state.sample_rate);
      auto end = state.tempo.tick_to_sample(musical->end, state.sample_rate);
      if (!start || !end) {
        return foundation::Result<void>::failure(!start ? start.error() : end.error());
      }
      first = start.value();
      last = end.value();
    } else {
      const auto& elapsed = std::get<project::ElapsedInterval>(placement);
      first = elapsed.start;
      last = elapsed.end;
    }
    const auto source = std::ranges::find_if(
        state.sources, [found](const auto& item) { return item.id == found->source; });
    std::uint64_t source_first_project = 0;
    std::uint64_t source_last_project = 0;
    if (source != state.sources.end()) {
      auto mapped_first = media::source_rate_frame_count(
          static_cast<std::uint64_t>(found->source_range.start.value), source->sample_rate,
          state.sample_rate);
      auto mapped_last =
          media::source_rate_frame_count(static_cast<std::uint64_t>(found->source_range.end.value),
                                         source->sample_rate, state.sample_rate);
      if (!mapped_first || !mapped_last)
        return foundation::Result<void>::failure(!mapped_first ? mapped_first.error()
                                                               : mapped_last.error());
      source_first_project = mapped_first.value();
      source_last_project = mapped_last.value();
      if (source_last_project <= source_first_project ||
          source_last_project - source_first_project !=
              static_cast<std::uint64_t>(last.value - first.value))
        return foundation::Result<void>::failure(
            {foundation::ErrorCode::unsupported, "Audio trim requires an unstretched clip"});
    } else if (found->source_range.end.value - found->source_range.start.value !=
               last.value - first.value) {
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::unsupported,
           "Audio trim requires a source record for rate-converted media"});
    }
    const auto original_edge = edge == AudioClipEdge::left ? first : last;
    if (new_edge == original_edge) {
      return foundation::Result<void>::success();
    }
    if ((edge == AudioClipEdge::left && new_edge >= last) ||
        (edge == AudioClipEdge::right && new_edge <= first)) {
      return foundation::Result<void>::failure(invalid);
    }
    if (auto* musical = std::get_if<project::MusicalInterval>(&placement)) {
      auto tick = state.tempo.sample_to_tick(new_edge, state.sample_rate);
      if (!tick) {
        return foundation::Result<void>::failure(tick.error());
      }
      auto round_trip = state.tempo.tick_to_sample(tick.value(), state.sample_rate);
      if (!round_trip || round_trip.value() != new_edge) {
        return foundation::Result<void>::failure(
            {foundation::ErrorCode::invalid_argument,
             "Musical audio trim edge must align exactly with a tempo-map tick"});
      }
      if (edge == AudioClipEdge::left) {
        musical->start = tick.value();
      } else {
        musical->end = tick.value();
      }
    } else {
      auto& elapsed = std::get<project::ElapsedInterval>(placement);
      if (edge == AudioClipEdge::left) {
        elapsed.start = new_edge;
      } else {
        elapsed.end = new_edge;
      }
    }
    auto range = found->source_range;
    const auto original_source_edge =
        edge == AudioClipEdge::left ? range.start.value : range.end.value;
    std::int64_t source_edge = 0;
    if (source != state.sources.end()) {
      std::uint64_t target = 0;
      if (edge == AudioClipEdge::left) {
        const auto remaining = static_cast<std::uint64_t>(last.value - new_edge.value);
        if (remaining > source_last_project)
          return foundation::Result<void>::failure(invalid);
        target = source_last_project - remaining;
      } else {
        const auto duration = static_cast<std::uint64_t>(new_edge.value - first.value);
        if (duration > std::numeric_limits<std::uint64_t>::max() - source_first_project)
          return foundation::Result<void>::failure(invalid);
        target = source_first_project + duration;
      }
      auto mapped = source_frame_at_project_frame(*source, state.sample_rate, target);
      if (!mapped)
        return foundation::Result<void>::failure(mapped.error());
      source_edge = mapped.value();
    } else {
      const auto delta = new_edge.value - original_edge.value;
      if ((delta < 0 && original_source_edge < -delta) ||
          (delta > 0 && original_source_edge > std::numeric_limits<std::int64_t>::max() - delta))
        return foundation::Result<void>::failure(invalid);
      source_edge = original_source_edge + delta;
    }
    if (edge == AudioClipEdge::left) {
      range.start.value = source_edge;
    } else {
      range.end.value = source_edge;
    }
    if (range.start.value < 0 || range.end <= range.start) {
      return foundation::Result<void>::failure(invalid);
    }
    if (source != state.sources.end()) {
      if (static_cast<std::uint64_t>(range.end.value) > source->frames) {
        return foundation::Result<void>::failure(invalid);
      }
    } else if ((edge == AudioClipEdge::left && source_edge < original_source_edge) ||
               (edge == AudioClipEdge::right && source_edge > original_source_edge)) {
      // A missing source may be narrowed but has no trusted outer bound to reveal.
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::unavailable, "Cannot extend audio trim without source metadata"});
    }
    auto applied = model().apply(
        {project::SetClipPlacement{clip, placement}, project::SetClipSourceRange{clip, range}});
    if (!applied) {
      return foundation::Result<void>::failure(applied.error());
    }
    return foundation::Result<void>::success();
  }
  return foundation::Result<void>::failure(
      {foundation::ErrorCode::not_found, "Audio clip not found: " + clip.to_string()});
}

foundation::Result<void> AudioClipService::set_audio_clip_fades(project::ClipId clip,
                                                                project::ClipFades fades) {
  auto applied = model().apply({project::SetClipFades{clip, fades}});
  if (!applied)
    return foundation::Result<void>::failure(applied.error());
  return foundation::Result<void>::success();
}

foundation::Result<void>
AudioClipService::set_audio_clip_processing(project::ClipId clip,
                                            project::ClipProcessing processing) {
  auto applied = model().apply({project::SetClipProcessing{clip, std::move(processing)}});
  if (!applied)
    return foundation::Result<void>::failure(applied.error());
  return foundation::Result<void>::success();
}

foundation::Result<void> AudioClipService::set_clip_color(project::ClipId clip,
                                                          std::optional<std::uint32_t> color) {
  for (const auto& track : snapshot().tracks) {
    for (const auto& item : track.clips) {
      if (item.id == clip && item.color == color)
        return foundation::Result<void>::success();
    }
    for (const auto& item : track.midi_clips) {
      if (item.id == clip && item.color == color)
        return foundation::Result<void>::success();
    }
  }
  auto applied = model().apply({project::SetClipColor{clip, color}});
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

foundation::Result<void> AudioClipService::set_audio_clip_name(project::ClipId clip,
                                                               std::string name) {
  name = presentable_name(name);
  for (const auto& track : snapshot().tracks)
    for (const auto& item : track.clips)
      if (item.id == clip && item.name == name)
        return foundation::Result<void>::success();
  auto applied = model().apply({project::SetClipName{clip, std::move(name)}});
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

Result<void> AudioClipService::set_audio_clips_fades(
    std::span<const std::pair<project::ClipId, project::ClipFades>> fades) {
  if (fades.empty())
    return Result<void>::failure({ErrorCode::invalid_argument, "No clip fades to set"});
  std::vector<project::Operation> operations;
  for (const auto& [clip, fade] : fades)
    operations.emplace_back(project::SetClipFades{clip, fade});
  auto applied = model().apply(std::move(operations));
  return applied ? Result<void>::success() : Result<void>::failure(applied.error());
}

Result<bool> AudioClipService::audio_clip_is_processed(project::ClipId clip) const {
  const auto& state = snapshot();
  const auto* found = find_clip(state, clip);
  if (found == nullptr)
    return Result<bool>::failure(
        {ErrorCode::not_found, "Audio clip not found: " + clip.to_string()});
  // A placeholder for a missing source keeps the raw edit rules; its warp
  // map cannot be resolved, so saved processing intent cannot be re-windowed.
  if (std::ranges::find(state.sources, found->source, &project::SourceRecord::id) ==
      state.sources.end()) {
    if (found->processing.requires_rendering())
      return Result<bool>::failure(
          {ErrorCode::unavailable, "Cannot edit a processed clip while its source is missing"});
    return Result<bool>::success(false);
  }
  auto request = processing::processed_clip_request(state, clip);
  if (!request)
    return Result<bool>::failure(request.error());
  return Result<bool>::success(request.value().has_value());
}

Result<project::ClipId> AudioClipService::split_processed_audio_clip(
    project::ClipId clip, std::int64_t offset, project::ClipFades left_fades,
    project::ClipFades right_fades, project::ClipId right_id,
    std::vector<project::AutomationLane> left_automation,
    std::vector<project::AutomationLane> right_automation) {
  const auto& state = snapshot();
  const auto* found = find_clip(state, clip);
  if (found == nullptr)
    return Result<project::ClipId>::failure(
        {ErrorCode::not_found, "Audio clip not found: " + clip.to_string()});
  auto left = rewindow_processed_clip(state, clip, 0, offset, false);
  auto right =
      rewindow_processed_clip(state, clip, offset, placement_length(found->placement), false);
  if (!left || !right)
    return Result<project::ClipId>::failure(!left ? left.error() : right.error());
  std::vector<project::Operation> operations;
  apply_window(operations, *found, left.value());
  if (left_fades != found->fades)
    operations.emplace_back(project::SetClipFades{clip, left_fades});
  for (std::size_t index = 0; index < found->automation.size(); ++index) {
    auto& lane = left_automation[index];
    if (lane != found->automation[index])
      operations.emplace_back(project::ReplaceAutomationLane{lane.id, std::move(lane)});
  }
  project::Clip copy = *found;
  copy.id = right_id;
  copy.placement = right.value().placement;
  copy.source_range = right.value().source_range;
  copy.processing = std::move(right.value().processing);
  copy.fades = right_fades;
  copy.automation = std::move(right_automation);
  const auto& track = *std::ranges::find_if(state.tracks, [clip](const auto& item) {
    return std::ranges::find(item.clips, clip, &project::Clip::id) != item.clips.end();
  });
  operations.emplace_back(project::AddClip{track.id, std::move(copy)});
  auto applied = model().apply(operations);
  if (!applied)
    return Result<project::ClipId>::failure(applied.error());
  return Result<project::ClipId>::success(right_id);
}

Result<void> AudioClipService::slip_processed_audio_clip(project::ClipId clip,
                                                         std::int64_t source_frame_delta) {
  const auto& state = snapshot();
  const auto* found = find_clip(state, clip);
  if (found == nullptr)
    return Result<void>::failure(
        {ErrorCode::not_found, "Audio clip not found: " + clip.to_string()});
  auto offset = processed_slip_offset(state, clip, source_frame_delta);
  if (!offset)
    return Result<void>::failure(offset.error());
  if (offset.value() == 0)
    return Result<void>::failure(
        {ErrorCode::invalid_argument, "Slip is smaller than one step of this clip's timeline"});
  auto window = rewindow_processed_clip(state, clip, offset.value(),
                                        offset.value() + placement_length(found->placement), true);
  if (!window)
    return Result<void>::failure(window.error());
  if (auto kept = keeps_markers(window.value(), "Slip"); !kept)
    return kept;
  std::vector<project::Operation> operations;
  apply_window(operations, *found, window.value());
  auto applied = model().apply(operations);
  if (!applied)
    return Result<void>::failure(applied.error());
  return Result<void>::success();
}

Result<void> AudioClipService::trim_processed_audio_clip(project::ClipId clip, AudioClipEdge edge,
                                                         timeline::SamplePosition new_edge) {
  const auto& state = snapshot();
  const auto* found = find_clip(state, clip);
  if (found == nullptr)
    return Result<void>::failure(
        {ErrorCode::not_found, "Audio clip not found: " + clip.to_string()});
  std::int64_t position = 0;
  if (const auto* musical = std::get_if<project::MusicalInterval>(&found->placement)) {
    auto tick = state.tempo.sample_to_tick(new_edge, state.sample_rate);
    if (!tick)
      return Result<void>::failure(tick.error());
    auto round_trip = state.tempo.tick_to_sample(tick.value(), state.sample_rate);
    if (!round_trip || round_trip.value() != new_edge)
      return Result<void>::failure(
          {ErrorCode::invalid_argument,
           "Musical audio trim edge must align exactly with a tempo-map tick"});
    position = tick.value().value - musical->start.value;
  } else {
    position = new_edge.value - std::get<project::ElapsedInterval>(found->placement).start.value;
  }
  const auto length = placement_length(found->placement);
  if ((edge == AudioClipEdge::left && position == 0) ||
      (edge == AudioClipEdge::right && position == length))
    return Result<void>::success();
  auto window = edge == AudioClipEdge::left
                    ? rewindow_processed_clip(state, clip, position, length, false)
                    : rewindow_processed_clip(state, clip, 0, position, false);
  if (!window)
    return Result<void>::failure(window.error());
  if (auto kept = keeps_markers(window.value(), "Trim"); !kept)
    return kept;
  std::vector<project::Operation> operations;
  apply_window(operations, *found, window.value());
  auto applied = model().apply(operations);
  if (!applied)
    return Result<void>::failure(applied.error());
  return Result<void>::success();
}

Result<void> AudioClipService::move_audio_clip_before_zero(project::ClipId clip,
                                                           timeline::TickPosition start) {
  const auto& state = snapshot();
  const auto* found = find_clip(state, clip);
  if (found == nullptr)
    return Result<void>::failure(
        {ErrorCode::not_found, "Audio clip not found: " + clip.to_string()});
  const auto bar = opening_bar_ticks(state);
  const auto bars = static_cast<std::uint32_t>((-start.value + bar - 1) / bar);
  std::vector<project::Operation> operations;
  const std::array placed{clip};
  auto shift = lead_bar_operations(state, bars, placed, operations);
  if (!shift)
    return Result<void>::failure(shift.error());
  const auto moved = start.value + shift.value().ticks;
  if (const auto* musical = std::get_if<project::MusicalInterval>(&found->placement)) {
    operations.emplace_back(project::SetClipPlacement{
        clip,
        project::MusicalInterval{{moved}, {moved + musical->end.value - musical->start.value}}});
  } else {
    // Inside the new bars the opening tempo holds, so time is proportional.
    const auto& elapsed = std::get<project::ElapsedInterval>(found->placement);
    const auto sample = std::llround(static_cast<long double>(moved) * shift.value().samples /
                                     static_cast<long double>(shift.value().ticks));
    operations.emplace_back(project::SetClipPlacement{
        clip,
        project::ElapsedInterval{{sample}, {sample + elapsed.end.value - elapsed.start.value}}});
  }
  auto applied = model().apply(operations);
  if (!applied)
    return Result<void>::failure(applied.error());
  return Result<void>::success();
}

} // namespace oscillate::application
