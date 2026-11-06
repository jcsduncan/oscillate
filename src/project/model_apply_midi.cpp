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

template <typename T> Result<void> apply_midi_clip_edit(ApplyContext& context, const T& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  const ClipId clip_id = [&] {
    if constexpr (std::is_same_v<T, SetMidiClipPlacement> || std::is_same_v<T, SetMidiPattern>) {
      return command.id;
    } else {
      return command.clip;
    }
  }();
  auto match = find_mutable_midi_clip(candidate, clip_id);
  if (!match) {
    return Result<void>::failure(
        {ErrorCode::not_found, "MIDI clip not found: " + clip_id.to_string()});
  }
  if (match->clip->settings_generation == std::numeric_limits<std::uint64_t>::max()) {
    return Result<void>::failure(exhausted("MIDI clip generation exhausted"));
  }
  if constexpr (std::is_same_v<T, SetMidiClipPlacement>) {
    if (match->clip->placement != command.placement) {
      match->clip->placement = command.placement;
      ++match->clip->settings_generation;
    }
  } else if constexpr (std::is_same_v<T, SetMidiPattern>) {
    if (match->clip->pattern_length != command.length || match->clip->loop != command.loop) {
      match->clip->pattern_length = command.length;
      match->clip->loop = command.loop;
      ++match->clip->settings_generation;
    }
  } else if constexpr (std::is_same_v<T, AddMidiNote>) {
    match->clip->notes.push_back(command.note);
    ++match->clip->settings_generation;
  } else if constexpr (std::is_same_v<T, SetMidiPedalLane>) {
    auto& pedals = match->clip->pedals;
    auto it = std::ranges::find_if(
        pedals, [&](const MidiPedalLane& lane) { return lane.pedal == command.pedal; });
    if (command.points.empty()) {
      if (it != pedals.end()) {
        pedals.erase(it);
        ++match->clip->settings_generation;
      }
    } else if (it == pedals.end()) {
      // Lanes stay in pedal order, so equal clips compare equal.
      const auto at = std::ranges::find_if(
          pedals, [&](const MidiPedalLane& lane) { return lane.pedal > command.pedal; });
      pedals.insert(at, MidiPedalLane{command.pedal, command.points});
      ++match->clip->settings_generation;
    } else if (it->points != command.points) {
      it->points = command.points;
      ++match->clip->settings_generation;
    }
  } else if constexpr (std::is_same_v<T, RemoveMidiNote>) {
    const auto removed = std::erase_if(
        match->clip->notes, [&](const MidiNote& note) { return note.id == command.note; });
    if (removed == 0) {
      return Result<void>::failure(
          {ErrorCode::not_found, "MIDI note not found: " + command.note.to_string()});
    }
    ++match->clip->settings_generation;
  } else {
    auto it = std::ranges::find_if(
        match->clip->notes, [&](const MidiNote& note) { return note.id == command.note.id; });
    if (it == match->clip->notes.end()) {
      return Result<void>::failure(
          {ErrorCode::not_found, "MIDI note not found: " + command.note.id.to_string()});
    }
    if (*it != command.note) {
      *it = command.note;
      ++match->clip->settings_generation;
    }
  }
  add_unique(changes.tracks, match->track->id);
  add_unique(changes.clips, clip_id);
  return Result<void>::success();
}

} // namespace

Result<void> apply_command(ApplyContext& context, const AddMidiClip& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto* track = find_track(candidate, command.track);
  if (track == nullptr) {
    return Result<void>::failure(
        {ErrorCode::not_found, "track not found: " + command.track.to_string()});
  }
  if (track->kind != TrackKind::instrument) {
    return Result<void>::failure(invalid("MIDI clip requires instrument track"));
  }
  if (has_clip_id(candidate, command.clip.id)) {
    return Result<void>::failure(conflict("clip already exists: " + command.clip.id.to_string()));
  }
  track->midi_clips.push_back(command.clip);
  add_unique(changes.tracks, track->id);
  add_unique(changes.clips, command.clip.id);
  for (const auto& lane : command.clip.automation)
    add_unique(changes.automation_lanes, lane.id);
  changes.automation |= !command.clip.automation.empty();
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const SetMidiClipPlacement& command) {
  return apply_midi_clip_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const SetMidiPattern& command) {
  return apply_midi_clip_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const AddMidiNote& command) {
  return apply_midi_clip_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const RemoveMidiNote& command) {
  return apply_midi_clip_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const SetMidiNote& command) {
  return apply_midi_clip_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const SetMidiPedalLane& command) {
  return apply_midi_clip_edit(context, command);
}

} // namespace oscillate::project::detail
