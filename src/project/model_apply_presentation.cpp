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

template <typename T>
Result<void> apply_track_presentation(ApplyContext& context, const T& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto* track = find_track(candidate, command.id);
  if (track == nullptr) {
    return Result<void>::failure(
        {ErrorCode::not_found, "track not found: " + command.id.to_string()});
  }
  if constexpr (std::is_same_v<T, SetTrackName>) {
    if (track->name == command.name) {
      return Result<void>::failure(invalid("track name is unchanged"));
    }
    track->name = command.name;
  } else {
    if (track->color == command.color) {
      return Result<void>::failure(invalid("track colour is unchanged"));
    }
    track->color = command.color;
  }
  changes.presentation = true;
  return Result<void>::success();
}

template <typename T> Result<void> apply_marker_edit(ApplyContext& context, const T& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  const auto found = std::ranges::find(candidate.markers, command.id, &Marker::id);
  if (found == candidate.markers.end()) {
    return Result<void>::failure({ErrorCode::not_found, "arrangement marker not found"});
  }
  if constexpr (std::is_same_v<T, MoveMarker>) {
    if (found->position == command.position) {
      return Result<void>::failure(invalid("arrangement marker is already there"));
    }
    found->position = command.position;
    std::ranges::sort(candidate.markers, {},
                      [](const Marker& marker) { return marker.position.value; });
  } else if constexpr (std::is_same_v<T, RenameMarker>) {
    if (found->name == command.name) {
      return Result<void>::failure(invalid("arrangement marker name is unchanged"));
    }
    found->name = command.name;
  } else {
    candidate.markers.erase(found);
  }
  changes.presentation = true;
  return Result<void>::success();
}

} // namespace

Result<void> apply_command(ApplyContext& context, const SetTrackName& command) {
  return apply_track_presentation(context, command);
}

Result<void> apply_command(ApplyContext& context, const SetTrackColor& command) {
  return apply_track_presentation(context, command);
}

Result<void> apply_command(ApplyContext& context, const SetClipName& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  if (!valid_text(command.name, max_track_name_bytes, false))
    return Result<void>::failure(invalid("invalid clip name"));
  std::string* name = nullptr;
  for (auto& track : candidate.tracks)
    for (auto& clip : track.clips)
      if (clip.id == command.id)
        name = &clip.name;
  if (name == nullptr)
    return Result<void>::failure({ErrorCode::not_found, "clip not found"});
  if (*name == command.name)
    return Result<void>::failure(invalid("clip name is unchanged"));
  *name = command.name;
  changes.presentation = true;
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const SetClipColor& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  std::optional<std::uint32_t>* color = nullptr;
  for (auto& track : candidate.tracks) {
    for (auto& clip : track.clips) {
      if (clip.id == command.id)
        color = &clip.color;
    }
    for (auto& clip : track.midi_clips) {
      if (clip.id == command.id)
        color = &clip.color;
    }
  }
  if (color == nullptr) {
    return Result<void>::failure({ErrorCode::not_found, "clip not found"});
  }
  if (*color == command.color) {
    return Result<void>::failure(invalid("clip colour is unchanged"));
  }
  *color = command.color;
  changes.presentation = true;
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const SetLeadBars& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  if (command.bars > max_lead_bars)
    return Result<void>::failure(invalid("too many bars before bar 1"));
  if (candidate.lead_bars == command.bars)
    return Result<void>::failure(invalid("bars before bar 1 are unchanged"));
  candidate.lead_bars = command.bars;
  changes.presentation = true;
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const SetProjectKey& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  if (command.key && !valid_key(*command.key))
    return Result<void>::failure(invalid("project key is not a valid key"));
  if (candidate.key == command.key)
    return Result<void>::failure(invalid("project key is unchanged"));
  candidate.key = command.key;
  changes.key = true;
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const AddMarker& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  if (std::ranges::any_of(candidate.markers,
                          [&](const Marker& marker) { return marker.id == command.marker.id; })) {
    return Result<void>::failure(conflict("arrangement marker already exists"));
  }
  const auto at =
      std::ranges::upper_bound(candidate.markers, command.marker.position.value, {},
                               [](const Marker& marker) { return marker.position.value; });
  candidate.markers.insert(at, command.marker);
  changes.presentation = true;
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const MoveMarker& command) {
  return apply_marker_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const RenameMarker& command) {
  return apply_marker_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const RemoveMarker& command) {
  return apply_marker_edit(context, command);
}

} // namespace oscillate::project::detail
