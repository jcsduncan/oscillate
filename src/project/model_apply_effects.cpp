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

[[nodiscard]] Vst3PluginInstance* find_plugin(Track& track, PluginInstanceId id) {
  if (auto* source = std::get_if<Vst3PluginInstance>(&track.instrument_source);
      source && source->id == id) {
    return source;
  }
  for (auto& insert : track.effects) {
    if (auto* plugin = std::get_if<Vst3PluginInstance>(&insert.processor);
        plugin && plugin->id == id) {
      return plugin;
    }
  }
  return nullptr;
}

[[nodiscard]] std::vector<EffectInsert>* effect_path(Snapshot& snapshot, const SignalPathId& path) {
  if (std::holds_alternative<MasterSignalPath>(path)) {
    return &snapshot.master_effects;
  }
  const auto track = std::ranges::find_if(snapshot.tracks, [&](const Track& candidate) {
    return candidate.id == std::get<TrackId>(path);
  });
  return track != snapshot.tracks.end() ? &track->effects : nullptr;
}

void mark_effect_path_changed(const SignalPathId& path, ChangeSet& changes) {
  if (const auto* track = std::get_if<TrackId>(&path)) {
    add_unique(changes.tracks, *track);
  } else {
    changes.master_effects = true;
  }
}

void remove_plugin_automation(Track& track, PluginInstanceId plugin, ChangeSet& changes) {
  const auto refers_to_plugin = [&](const AutomationLane& lane) {
    const auto* target = std::get_if<Vst3ParameterTarget>(&lane.target);
    if (target && target->plugin == plugin) {
      add_unique(changes.automation_lanes, lane.id);
      return true;
    }
    return false;
  };
  std::erase_if(track.automation, refers_to_plugin);
  for (auto& clip : track.clips) {
    std::erase_if(clip.automation, refers_to_plugin);
  }
  for (auto& clip : track.midi_clips) {
    std::erase_if(clip.automation, refers_to_plugin);
  }
  changes.automation |= !changes.automation_lanes.empty();
}

} // namespace

namespace {

template <typename T> Result<void> apply_effect_slot_edit(ApplyContext& context, const T& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto* effects = effect_path(candidate, command.path);
  if (!effects) {
    return Result<void>::failure({ErrorCode::not_found, "effect signal path not found"});
  }
  const auto found = std::ranges::find_if(
      *effects, [&](const EffectInsert& slot) { return slot.id == command.id; });
  if (found == effects->end()) {
    return Result<void>::failure({ErrorCode::not_found, "effect slot not on signal path"});
  }
  if constexpr (std::is_same_v<T, MoveEffectInsert>) {
    const auto previous = static_cast<std::size_t>(found - effects->begin());
    if (command.index >= effects->size()) {
      return Result<void>::failure(invalid("invalid effect move position"));
    }
    if (command.index == previous) {
      return Result<void>::failure(invalid("effect slot is already at destination"));
    }
    auto slot = std::move(*found);
    effects->erase(found);
    effects->insert(effects->begin() + static_cast<std::ptrdiff_t>(command.index), std::move(slot));
  } else if constexpr (std::is_same_v<T, SetEffectInsertEnabled>) {
    if (found->enabled == command.enabled) {
      return Result<void>::failure(invalid("effect slot already has requested state"));
    }
    found->enabled = command.enabled;
  } else {
    if (const auto* plugin = std::get_if<Vst3PluginInstance>(&found->processor)) {
      add_unique(changes.plugins, plugin->id);
      if (auto* track_id = std::get_if<TrackId>(&command.path)) {
        auto* track = find_track(candidate, *track_id);
        remove_plugin_automation(*track, plugin->id, changes);
      }
    }
    if (std::holds_alternative<NativeEffectPatch>(found->processor)) {
      if (auto* track_id = std::get_if<TrackId>(&command.path)) {
        remove_effect_automation(*find_track(candidate, *track_id), found->id, changes);
      }
    }
    // A key into the removed effect leaves with it.
    const InsertSlotId slot = found->id;
    remove_sends(
        candidate, [&](const SendRoute& send) { return send.target_insert == slot; }, changes);
    effects = effect_path(candidate, command.path);
    effects->erase(std::ranges::find(*effects, slot, &EffectInsert::id));
  }
  mark_effect_path_changed(command.path, changes);
  return Result<void>::success();
}

template <typename T> Result<void> apply_vst3_plugin_edit(ApplyContext& context, const T& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  Track* owner = nullptr;
  Vst3PluginInstance* plugin = nullptr;
  std::optional<std::size_t> effect_index;
  for (auto& track : candidate.tracks) {
    if (auto* found = find_plugin(track, command.id)) {
      owner = &track;
      plugin = found;
      for (std::size_t index = 0; index < track.effects.size(); ++index) {
        if (const auto* effect = std::get_if<Vst3PluginInstance>(&track.effects[index].processor);
            effect && effect->id == command.id) {
          effect_index = index;
          break;
        }
      }
      break;
    }
  }
  if (plugin == nullptr) {
    return Result<void>::failure({ErrorCode::not_found, "VST3 plugin instance not found"});
  }
  if constexpr (std::is_same_v<T, RemoveVst3Plugin>) {
    remove_plugin_automation(*owner, command.id, changes);
    if (effect_index) {
      owner->effects.erase(owner->effects.begin() + static_cast<std::ptrdiff_t>(*effect_index));
    } else {
      // find_plugin found it outside the effects, so it is the instrument.
      owner->instrument_source = std::monostate{};
    }
  } else if constexpr (std::is_same_v<T, MoveVst3Plugin>) {
    if (!effect_index || owner->id != command.track || command.index >= owner->effects.size()) {
      return Result<void>::failure(invalid("invalid VST3 effect move position or track"));
    }
    auto slot = std::move(owner->effects[*effect_index]);
    owner->effects.erase(owner->effects.begin() + static_cast<std::ptrdiff_t>(*effect_index));
    owner->effects.insert(owner->effects.begin() + static_cast<std::ptrdiff_t>(command.index),
                          std::move(slot));
  } else if constexpr (std::is_same_v<T, SetVst3ComponentState>) {
    if (command.state.size() > max_vst3_state_bytes) {
      return Result<void>::failure(exhausted("VST3 component state limit exceeded"));
    }
    plugin->component_state = command.state;
  } else if constexpr (std::is_same_v<T, SetVst3ControllerState>) {
    if (command.state && command.state->size() > max_vst3_state_bytes)
      return Result<void>::failure(exhausted("VST3 controller state limit exceeded"));
    plugin->controller_state = command.state;
  } else if constexpr (std::is_same_v<T, SetVst3ParameterReferences>) {
    auto valid = validate_parameters(command.parameters);
    if (!valid)
      return valid;
    plugin->parameters = command.parameters;
  } else if constexpr (std::is_same_v<T, SetVst3Missing>) {
    plugin->missing = command.missing;
  } else {
    if (plugin->bundle_path != command.expected_bundle_path ||
        plugin->binary_sha256 != command.expected_binary_sha256 ||
        (plugin->bundle_path == command.canonical_bundle_path && !plugin->missing)) {
      return Result<void>::failure(conflict("VST3 relink target or saved binary identity changed"));
    }
    plugin->bundle_path = command.canonical_bundle_path;
    plugin->missing = false;
    if (auto valid = validate_plugin(*plugin); !valid)
      return valid;
  }
  add_unique(changes.tracks, owner->id);
  add_unique(changes.plugins, command.id);
  return Result<void>::success();
}

} // namespace

Result<void> apply_command(ApplyContext& context, const InsertVst3Plugin& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto* track = find_track(candidate, command.track);
  if (track == nullptr) {
    return Result<void>::failure({ErrorCode::not_found, "VST3 parent track not found"});
  }
  auto valid = validate_plugin(command.plugin);
  if (!valid)
    return valid;
  for (const auto& existing_track : candidate.tracks) {
    for (const auto* plugin : track_plugins(existing_track)) {
      if (plugin->id == command.plugin.id) {
        return Result<void>::failure(conflict("VST3 plugin instance already exists"));
      }
    }
  }
  for (const auto& slot : candidate.master_effects) {
    if (const auto* plugin = std::get_if<Vst3PluginInstance>(&slot.processor);
        plugin && plugin->id == command.plugin.id) {
      return Result<void>::failure(conflict("VST3 plugin instance already exists"));
    }
  }
  if (track->kind == TrackKind::instrument &&
      std::holds_alternative<std::monostate>(track->instrument_source) && track->effects.empty() &&
      command.index == 0) {
    track->instrument_source = command.plugin;
  } else {
    if (command.index > track->effects.size()) {
      return Result<void>::failure(invalid("invalid VST3 insertion position"));
    }
    auto slot = allocate_insert_slot(candidate);
    if (!slot)
      return Result<void>::failure(slot.error());
    track->effects.insert(track->effects.begin() + static_cast<std::ptrdiff_t>(command.index),
                          EffectInsert{slot.value(), true, command.plugin});
  }
  add_unique(changes.tracks, track->id);
  add_unique(changes.plugins, command.plugin.id);
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const AddNativeEffect& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto* effects = effect_path(candidate, command.path);
  if (!effects) {
    return Result<void>::failure({ErrorCode::not_found, "effect signal path not found"});
  }
  if (command.index > effects->size()) {
    return Result<void>::failure(invalid("invalid native effect insertion position"));
  }
  if (effects->size() >= max_effect_slots_per_path) {
    return Result<void>::failure(exhausted("effect slot limit exceeded"));
  }
  auto slot = allocate_insert_slot(candidate);
  if (!slot)
    return Result<void>::failure(slot.error());
  auto processor =
      std::visit([](const auto& native) -> EffectProcessor { return native; }, command.processor);
  effects->insert(effects->begin() + static_cast<std::ptrdiff_t>(command.index),
                  {slot.value(), true, std::move(processor)});
  mark_effect_path_changed(command.path, changes);
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const SetNativeEffectSettings& command) {
  auto& candidate = context.candidate;
  auto& changes = context.changes;
  auto* effects = effect_path(candidate, command.path);
  if (!effects) {
    return Result<void>::failure({ErrorCode::not_found, "effect signal path not found"});
  }
  const auto found = std::ranges::find_if(
      *effects, [&](const EffectInsert& slot) { return slot.id == command.id; });
  if (found == effects->end()) {
    return Result<void>::failure({ErrorCode::not_found, "effect slot not on signal path"});
  }
  auto replaced = std::visit(
      [&](auto replacement) -> Result<void> {
        using Native = std::decay_t<decltype(replacement)>;
        auto* saved = std::get_if<Native>(&found->processor);
        if (!saved) {
          return Result<void>::failure(
              conflict("effect settings must preserve the native processor kind"));
        }
        if constexpr (std::is_same_v<Native, NativeEffectPatch>) {
          // A slot keeps its engine; changing effect is remove + add.
          if (saved->engine != replacement.engine ||
              saved->engine_version != replacement.engine_version) {
            return Result<void>::failure(
                conflict("effect settings must preserve the built-in effect engine"));
          }
        }
        const bool same_settings = [&] {
          if constexpr (std::is_same_v<Native, NativeEffectPatch>) {
            return saved->parameters == replacement.parameters &&
                   saved->document == replacement.document && saved->link == replacement.link;
          } else {
            return saved->ir == replacement.ir && saved->wet == replacement.wet;
          }
        }();
        if (same_settings) {
          return Result<void>::failure(invalid("native effect settings are unchanged"));
        }
        if (saved->generation == std::numeric_limits<std::uint64_t>::max()) {
          return Result<void>::failure(exhausted("native effect generation exhausted"));
        }
        replacement.generation = saved->generation + 1;
        auto valid = [&] {
          if constexpr (std::is_same_v<Native, NativeEffectPatch>) {
            return validate_native_effect_patch(replacement, candidate);
          } else {
            return validate_native_convolution(replacement, candidate);
          }
        }();
        if (!valid) {
          return valid;
        }
        found->processor = std::move(replacement);
        return Result<void>::success();
      },
      command.processor);
  if (!replaced) {
    return replaced;
  }
  mark_effect_path_changed(command.path, changes);
  return Result<void>::success();
}

Result<void> apply_command(ApplyContext& context, const MoveEffectInsert& command) {
  return apply_effect_slot_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const SetEffectInsertEnabled& command) {
  return apply_effect_slot_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const RemoveEffectInsert& command) {
  return apply_effect_slot_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const RemoveVst3Plugin& command) {
  return apply_vst3_plugin_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const MoveVst3Plugin& command) {
  return apply_vst3_plugin_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const SetVst3ComponentState& command) {
  return apply_vst3_plugin_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const SetVst3ControllerState& command) {
  return apply_vst3_plugin_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const SetVst3ParameterReferences& command) {
  return apply_vst3_plugin_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const SetVst3Missing& command) {
  return apply_vst3_plugin_edit(context, command);
}

Result<void> apply_command(ApplyContext& context, const RelinkVst3Plugin& command) {
  return apply_vst3_plugin_edit(context, command);
}

} // namespace oscillate::project::detail
