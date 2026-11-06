#include "model_internal.hpp"

#include "oscillate/foundation/midi.hpp"
#include "oscillate/foundation/trace.hpp"
#include "oscillate/project/clay_limits.hpp"
#include "oscillate/project/insert_projection.hpp"
#include "oscillate/project/lookup.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace oscillate::project::detail {

Result<void> apply_command(ApplyContext& context, const AddClip& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto* track = find_track(candidate, command.track);
  if (track == nullptr) {
    return Result<void>::failure(
        {ErrorCode::not_found, "track not found: " + command.track.to_string()});
  }
  if (track->kind != TrackKind::audio) {
    return Result<void>::failure(invalid("audio clip requires audio track"));
  }
  if (has_clip_id(candidate, command.clip.id)) {
    return Result<void>::failure(conflict("clip already exists: " + command.clip.id.to_string()));
  }
  track->clips.push_back(command.clip);
  add_unique(changes.tracks, command.track);
  add_unique(changes.clips, command.clip.id);
  for (const auto& lane : command.clip.automation)
    add_unique(changes.automation_lanes, lane.id);
  changes.automation |= !command.clip.automation.empty();
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const RemoveClip& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  if (auto match = find_mutable_clip(candidate, command.id)) {
    for (const auto& lane : match->clip->automation) {
      add_unique(changes.automation_lanes, lane.id);
    }
    std::erase_if(match->track->clips, [&](const Clip& clip) { return clip.id == command.id; });
    add_unique(changes.tracks, match->track->id);
  } else if (auto midi = find_mutable_midi_clip(candidate, command.id)) {
    for (const auto& lane : midi->clip->automation) {
      add_unique(changes.automation_lanes, lane.id);
    }
    std::erase_if(midi->track->midi_clips,
                  [&](const MidiClip& clip) { return clip.id == command.id; });
    add_unique(changes.tracks, midi->track->id);
  } else {
    return Result<void>::failure(
        {ErrorCode::not_found, "clip not found: " + command.id.to_string()});
  }
  add_unique(changes.clips, command.id);
  changes.automation |= !changes.automation_lanes.empty();
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const SetClipPlacement& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto match = find_mutable_clip(candidate, command.id);
  if (!match) {
    return Result<void>::failure(
        {ErrorCode::not_found, "clip not found: " + command.id.to_string()});
  }
  if (match->clip->placement != command.placement) {
    if (match->clip->settings_generation == std::numeric_limits<std::uint64_t>::max()) {
      return Result<void>::failure(exhausted("clip settings generation exhausted"));
    }
    match->clip->placement = command.placement;
    ++match->clip->settings_generation;
  } else {
    return Result<void>::failure(invalid("clip placement is unchanged"));
  }
  add_unique(changes.tracks, match->track->id);
  add_unique(changes.clips, command.id);
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const SetClipSourceRange& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto match = find_mutable_clip(candidate, command.id);
  if (!match) {
    return Result<void>::failure(
        {ErrorCode::not_found, "clip not found: " + command.id.to_string()});
  }
  if (match->clip->source_range != command.range) {
    if (match->clip->settings_generation == std::numeric_limits<std::uint64_t>::max()) {
      return Result<void>::failure(exhausted("clip settings generation exhausted"));
    }
    match->clip->source_range = command.range;
    ++match->clip->settings_generation;
  } else {
    return Result<void>::failure(invalid("clip source range is unchanged"));
  }
  add_unique(changes.tracks, match->track->id);
  add_unique(changes.clips, command.id);
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const SetClipFades& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto match = find_mutable_clip(candidate, command.id);
  if (!match)
    return Result<void>::failure(
        {ErrorCode::not_found, "clip not found: " + command.id.to_string()});
  if (match->clip->fades == command.fades)
    return Result<void>::failure(invalid("clip fades are unchanged"));
  if (match->clip->settings_generation == std::numeric_limits<std::uint64_t>::max())
    return Result<void>::failure(exhausted("clip settings generation exhausted"));
  match->clip->fades = command.fades;
  ++match->clip->settings_generation;
  add_unique(changes.tracks, match->track->id);
  add_unique(changes.clips, command.id);
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const SetClipProcessing& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto match = find_mutable_clip(candidate, command.id);
  if (!match)
    return Result<void>::failure(
        {ErrorCode::not_found, "clip not found: " + command.id.to_string()});
  if (match->clip->processing == command.processing)
    return Result<void>::failure(invalid("clip processing is unchanged"));
  if (match->clip->settings_generation == std::numeric_limits<std::uint64_t>::max())
    return Result<void>::failure(exhausted("clip settings generation exhausted"));
  match->clip->processing = command.processing;
  ++match->clip->settings_generation;
  add_unique(changes.tracks, match->track->id);
  add_unique(changes.clips, command.id);
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const SetClipClay& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto match = find_mutable_clip(candidate, command.id);
  if (!match)
    return Result<void>::failure(
        {ErrorCode::not_found, "clip not found: " + command.id.to_string()});
  if (match->clip->clay == command.take)
    return Result<void>::failure(invalid("clip Clay take is unchanged"));
  if (match->clip->settings_generation == std::numeric_limits<std::uint64_t>::max())
    return Result<void>::failure(exhausted("clip settings generation exhausted"));
  match->clip->clay = command.take;
  ++match->clip->settings_generation;
  add_unique(changes.tracks, match->track->id);
  add_unique(changes.clips, command.id);
  return Result<void>::success();
}

} // namespace oscillate::project::detail
