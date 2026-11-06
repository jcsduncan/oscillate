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

namespace {

struct MutableLane {
  Track* track;
  std::optional<ClipId> clip;
  std::vector<AutomationLane>* owner;
  AutomationLane* lane;
};

[[nodiscard]] std::optional<MutableLane> find_mutable_lane(Snapshot& state, AutomationLaneId id) {
  for (auto& track : state.tracks) {
    auto find_in = [&](std::vector<AutomationLane>& lanes,
                       std::optional<ClipId> clip) -> std::optional<MutableLane> {
      auto found =
          std::ranges::find_if(lanes, [&](const AutomationLane& lane) { return lane.id == id; });
      if (found != lanes.end()) {
        return MutableLane{&track, clip, &lanes, &*found};
      }
      return {};
    };
    if (auto found = find_in(track.automation, {})) {
      return found;
    }
    for (auto& clip : track.clips) {
      if (auto found = find_in(clip.automation, clip.id)) {
        return found;
      }
    }
    for (auto& clip : track.midi_clips) {
      if (auto found = find_in(clip.automation, clip.id)) {
        return found;
      }
    }
  }
  return {};
}

} // namespace

namespace {

template <typename T> Result<void> apply_lane_edit(ApplyContext& context, const T& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto found = find_mutable_lane(candidate, command.id);
  if (!found) {
    return Result<void>::failure({ErrorCode::not_found, "automation lane not found"});
  }
  if constexpr (std::is_same_v<T, ReplaceAutomationLane>) {
    if (command.lane.id != command.id ||
        found->lane->settings_generation == std::numeric_limits<std::uint64_t>::max() ||
        command.lane.settings_generation != found->lane->settings_generation + 1) {
      return Result<void>::failure(invalid("invalid automation lane replacement"));
    }
    *found->lane = command.lane;
  } else {
    std::erase_if(*found->owner, [&](const AutomationLane& lane) { return lane.id == command.id; });
  }
  add_unique(changes.tracks, found->track->id);
  if (found->clip) {
    add_unique(changes.clips, *found->clip);
  }
  add_unique(changes.automation_lanes, command.id);
  changes.automation = true;
  return Result<void>::success();
}

} // namespace

Result<void> apply_command(ApplyContext& context, const AddAutomationLane& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  std::vector<AutomationLane>* lanes = nullptr;
  Track* owner = nullptr;
  std::optional<ClipId> clip_id;
  if (const auto* track_id = std::get_if<TrackId>(&command.owner)) {
    owner = find_track(candidate, *track_id);
    if (owner) {
      lanes = &owner->automation;
    }
  } else {
    clip_id = std::get<ClipId>(command.owner);
    if (auto audio = find_mutable_clip(candidate, *clip_id)) {
      owner = audio->track;
      lanes = &audio->clip->automation;
    } else if (auto midi = find_mutable_midi_clip(candidate, *clip_id)) {
      owner = midi->track;
      lanes = &midi->clip->automation;
    }
  }
  if (!owner || !lanes) {
    return Result<void>::failure({ErrorCode::not_found, "automation owner not found"});
  }
  if (find_mutable_lane(candidate, command.lane.id)) {
    return Result<void>::failure(conflict("automation lane ID already exists"));
  }
  lanes->push_back(command.lane);
  add_unique(changes.tracks, owner->id);
  if (clip_id) {
    add_unique(changes.clips, *clip_id);
  }
  add_unique(changes.automation_lanes, command.lane.id);
  changes.automation = true;
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const ReplaceAutomationLane& command) {
  return apply_lane_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const RemoveAutomationLane& command) {
  return apply_lane_edit(context, command);
}

} // namespace oscillate::project::detail
