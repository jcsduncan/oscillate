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

// Built-in synth lanes belong to one engine's parameters. Changing or removing
// the engine removes them, like removing a VST3 plugin removes its lanes, and
// macro mappings to them.
void remove_native_automation(Track& track, ChangeSet& changes) {
  remove_macro_mappings(track, [](const MacroDestination& destination) {
    return std::holds_alternative<NativeParameterTarget>(destination);
  });
  const auto native_lane = [&](const AutomationLane& lane) {
    if (std::holds_alternative<NativeParameterTarget>(lane.target)) {
      add_unique(changes.automation_lanes, lane.id);
      return true;
    }
    return false;
  };
  std::erase_if(track.automation, native_lane);
  for (auto& clip : track.clips) {
    std::erase_if(clip.automation, native_lane);
  }
  for (auto& clip : track.midi_clips) {
    std::erase_if(clip.automation, native_lane);
  }
}

} // namespace

namespace {

template <typename T> Result<void> apply_note_effect_edit(ApplyContext& context, const T& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto* track = find_track(candidate, command.track);
  if (track == nullptr) {
    return Result<void>::failure(
        {ErrorCode::not_found, "track not found: " + command.track.to_string()});
  }
  auto& effects = track->note_effects;
  const auto found = std::ranges::find(effects, command.id, &NoteEffectInsert::id);
  if (found == effects.end()) {
    return Result<void>::failure({ErrorCode::not_found, "note effect not on track"});
  }
  if constexpr (std::is_same_v<T, SetNoteEffectSettings>) {
    if (found->parameters == command.parameters) {
      return Result<void>::failure(invalid("note effect settings are unchanged"));
    }
    found->parameters = command.parameters;
  } else if constexpr (std::is_same_v<T, MoveNoteEffect>) {
    const auto previous = static_cast<std::size_t>(found - effects.begin());
    if (command.index >= effects.size()) {
      return Result<void>::failure(invalid("invalid note effect move position"));
    }
    if (command.index == previous) {
      return Result<void>::failure(invalid("note effect is already at destination"));
    }
    auto slot = std::move(*found);
    effects.erase(found);
    effects.insert(effects.begin() + static_cast<std::ptrdiff_t>(command.index), std::move(slot));
  } else if constexpr (std::is_same_v<T, SetNoteEffectEnabled>) {
    if (found->enabled == command.enabled) {
      return Result<void>::failure(invalid("note effect already has requested state"));
    }
    found->enabled = command.enabled;
  } else {
    const auto id = found->id;
    effects.erase(found);
    remove_effect_automation(*track, id, changes);
  }
  add_unique(changes.tracks, track->id);
  return Result<void>::success();
}

} // namespace

Result<void> apply_command(ApplyContext& context, const SetNativeInstrumentPatch& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto* track = find_track(candidate, command.track);
  if (track == nullptr) {
    return Result<void>::failure(
        {ErrorCode::not_found, "track not found: " + command.track.to_string()});
  }
  if (track->kind != TrackKind::instrument) {
    return Result<void>::failure(invalid("native instrument requires instrument track"));
  }
  if (std::holds_alternative<Vst3PluginInstance>(track->instrument_source)) {
    return Result<void>::failure(
        conflict("remove the VST3 instrument before selecting a built-in synth"));
  }
  const auto* current = native_source(*track);
  if (current != nullptr && (!command.patch || command.patch->engine != current->engine)) {
    remove_native_automation(*track, changes);
  }
  track->instrument_source =
      command.patch ? InstrumentSource{*command.patch} : InstrumentSource{std::monostate{}};
  add_unique(changes.tracks, track->id);
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const AddNoteEffect& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto* track = find_track(candidate, command.track);
  if (track == nullptr) {
    return Result<void>::failure(
        {ErrorCode::not_found, "track not found: " + command.track.to_string()});
  }
  if (command.index > track->note_effects.size()) {
    return Result<void>::failure(invalid("invalid note effect insertion position"));
  }
  if (track->note_effects.size() >= max_note_effects_per_track) {
    return Result<void>::failure(exhausted("note effect limit exceeded"));
  }
  auto slot = allocate_insert_slot(candidate);
  if (!slot)
    return Result<void>::failure(slot.error());
  track->note_effects.insert(track->note_effects.begin() +
                                 static_cast<std::ptrdiff_t>(command.index),
                             NoteEffectInsert{slot.value(), true, command.engine,
                                              command.engine_version, command.parameters});
  add_unique(changes.tracks, track->id);
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const SetNoteEffectSettings& command) {
  return apply_note_effect_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const MoveNoteEffect& command) {
  return apply_note_effect_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const SetNoteEffectEnabled& command) {
  return apply_note_effect_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const RemoveNoteEffect& command) {
  return apply_note_effect_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const SetTrackMacros& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto* track = find_track(candidate, command.track);
  if (track == nullptr) {
    return Result<void>::failure(
        {ErrorCode::not_found, "track not found: " + command.track.to_string()});
  }
  if (track->macros == command.macros) {
    return Result<void>::failure(invalid("macros are unchanged"));
  }
  // Lanes of macros that no longer exist go with them.
  const auto removed_macro = [&](const AutomationLane& lane) {
    const auto* macro = std::get_if<MacroTarget>(&lane.target);
    if (macro != nullptr && macro->index >= command.macros.size()) {
      add_unique(changes.automation_lanes, lane.id);
      return true;
    }
    return false;
  };
  std::erase_if(track->automation, removed_macro);
  for (auto& clip : track->clips)
    std::erase_if(clip.automation, removed_macro);
  for (auto& clip : track->midi_clips)
    std::erase_if(clip.automation, removed_macro);
  changes.automation |= !changes.automation_lanes.empty();
  track->macros = command.macros;
  add_unique(changes.tracks, track->id);
  return Result<void>::success();
}

} // namespace oscillate::project::detail
