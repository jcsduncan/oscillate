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

Result<void> apply_command(ApplyContext& context, const AddTrack& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  if (find_track(candidate, command.id) != nullptr) {
    return Result<void>::failure(conflict("track already exists: " + command.id.to_string()));
  }
  // Return buses sit below every other track: by default a return
  // goes last and any other track goes in above the first return.
  const auto first_return =
      std::ranges::find(candidate.tracks, TrackKind::return_bus, &Track::kind);
  const auto index = command.insert_index.value_or(
      command.kind == TrackKind::return_bus
          ? candidate.tracks.size()
          : static_cast<std::size_t>(first_return - candidate.tracks.begin()));
  if (index > candidate.tracks.size()) {
    return Result<void>::failure(
        {ErrorCode::invalid_argument, "track insertion index is outside the project"});
  }
  candidate.tracks.emplace(candidate.tracks.begin() + static_cast<std::ptrdiff_t>(index),
                           command.id, std::vector<Clip>{}, command.kind);
  add_unique(changes.tracks, command.id);
  changes.routing = true;
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const RemoveTrack& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto it = std::ranges::find_if(candidate.tracks,
                                 [&](const Track& track) { return track.id == command.id; });
  if (it == candidate.tracks.end()) {
    return Result<void>::failure(
        {ErrorCode::not_found, "track not found: " + command.id.to_string()});
  }
  for (const auto& clip : it->clips) {
    add_unique(changes.clips, clip.id);
  }
  for (const auto& clip : it->midi_clips) {
    add_unique(changes.clips, clip.id);
  }
  for (const auto* plugin : track_plugins(*it)) {
    add_unique(changes.plugins, plugin->id);
  }
  for (const auto& lane : it->automation) {
    add_unique(changes.automation_lanes, lane.id);
  }
  for (const auto& clip : it->clips) {
    for (const auto& lane : clip.automation) {
      add_unique(changes.automation_lanes, lane.id);
    }
  }
  for (const auto& clip : it->midi_clips) {
    for (const auto& lane : clip.automation) {
      add_unique(changes.automation_lanes, lane.id);
    }
  }
  changes.automation |= !changes.automation_lanes.empty();
  add_unique(changes.tracks, command.id);
  candidate.tracks.erase(it);
  changes.routing = true;
  // Routes into the removed track go with it: other tracks' sends to
  // it (a sidechain keying its effects, an audible send to a bus)
  // with their gain automation, and outputs into a removed bus fall
  // back to the master. Undo restores them all.
  remove_sends(
      candidate, [&](const SendRoute& send) { return send.destination == command.id; }, changes);
  for (auto& other : candidate.tracks) {
    if (other.main_output == command.id) {
      other.main_output.reset();
      add_unique(changes.tracks, other.id);
    }
  }
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const MoveTrack& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  const auto found = std::ranges::find(candidate.tracks, command.id, &Track::id);
  if (found == candidate.tracks.end()) {
    return Result<void>::failure({ErrorCode::not_found, "track to move was not found"});
  }
  const auto from = static_cast<std::size_t>(found - candidate.tracks.begin());
  if (command.to_index >= candidate.tracks.size() || command.to_index == from) {
    return Result<void>::failure(
        {ErrorCode::invalid_argument, "track destination is invalid or unchanged"});
  }
  Track moved = std::move(*found);
  candidate.tracks.erase(found);
  candidate.tracks.insert(candidate.tracks.begin() + static_cast<std::ptrdiff_t>(command.to_index),
                          std::move(moved));
  add_unique(changes.tracks, command.id);
  changes.routing = true;
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const SetTrackMuted& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto* track = find_track(candidate, command.id);
  if (track == nullptr) {
    return Result<void>::failure(
        {ErrorCode::not_found, "track not found: " + command.id.to_string()});
  }
  if (track->muted == command.muted) {
    return Result<void>::failure(invalid("track mute is unchanged"));
  }
  track->muted = command.muted;
  add_unique(changes.tracks, command.id);
  changes.routing = true;
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const SetTrackSoloed& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto* track = find_track(candidate, command.id);
  if (track == nullptr) {
    return Result<void>::failure(
        {ErrorCode::not_found, "track not found: " + command.id.to_string()});
  }
  if (track->soloed == command.soloed) {
    return Result<void>::failure(invalid("track solo is unchanged"));
  }
  track->soloed = command.soloed;
  add_unique(changes.tracks, command.id);
  changes.routing = true;
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const SetTrackLevelPan& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto* track = find_track(candidate, command.id);
  if (track == nullptr)
    return Result<void>::failure(
        {ErrorCode::not_found, "track not found: " + command.id.to_string()});
  if (!std::isfinite(command.level) || command.level < 0.0 || command.level > 4.0 ||
      !std::isfinite(command.pan) || command.pan < -1.0 || command.pan > 1.0)
    return Result<void>::failure(invalid("track level or pan is outside its range"));
  if (track->level == command.level && track->pan == command.pan)
    return Result<void>::failure(invalid("track level and pan are unchanged"));
  track->level = command.level;
  track->pan = command.pan;
  add_unique(changes.tracks, command.id);
  changes.routing = true;
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const SetMasterLevel& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  if (!std::isfinite(command.level) || command.level < 0.0 || command.level > 4.0)
    return Result<void>::failure(invalid("master level is outside its range"));
  if (candidate.master_level == command.level)
    return Result<void>::failure(invalid("master level is unchanged"));
  candidate.master_level = command.level;
  changes.routing = true;
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const SetTrackFrozen& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto* track = find_track(candidate, command.track);
  if (track == nullptr) {
    return Result<void>::failure(
        {ErrorCode::not_found, "track not found: " + command.track.to_string()});
  }
  if (track->frozen == command.frozen) {
    return Result<void>::failure(
        invalid(command.frozen ? "track is already frozen" : "track is not frozen"));
  }
  track->frozen = command.frozen;
  add_unique(changes.tracks, track->id);
  return Result<void>::success();
}

} // namespace oscillate::project::detail
