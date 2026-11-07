#include "oscillate/application/session/track_service.hpp"

#include "edit_support.hpp"
#include "midi_clips.hpp"
#include "oscillate/application/key_cache.hpp"
#include "oscillate/application/midi_note_tools.hpp"
#include "oscillate/application/tempo_cache.hpp"
#include "oscillate/midi_file/smf.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/project_binding.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "session_state.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::application {
namespace {

using ChannelKey = project::TrackControlTarget::Key;
using detail::find_midi_clip;
using detail::midi_anchor_tick;

} // namespace

std::optional<std::size_t> TrackService::track_index(project::TrackId track) const {
  const auto& tracks = snapshot().tracks;
  const auto found = std::ranges::find(tracks, track, &project::Track::id);
  return found == tracks.end()
             ? std::nullopt
             : std::optional<std::size_t>{static_cast<std::size_t>(found - tracks.begin())};
}

foundation::Result<project::TrackId>
TrackService::add_instrument_track(std::optional<std::size_t> insert_index) {
  using Result = foundation::Result<project::TrackId>;
  auto id = numbered_id<project::TrackId>(next_track_number());
  if (!id) {
    return Result::failure(id.error());
  }
  auto applied =
      model().apply({project::AddTrack{id.value(), project::TrackKind::instrument, insert_index}});
  if (!applied) {
    return Result::failure(applied.error());
  }
  return Result::success(id.value());
}

foundation::Result<void> TrackService::remove_track(project::TrackId track) {
  auto applied = model().apply({project::RemoveTrack{track}});
  if (!applied) {
    return foundation::Result<void>::failure(applied.error());
  }
  return foundation::Result<void>::success();
}

foundation::Result<void> TrackService::move_track(project::TrackId track, std::size_t to_index) {
  auto applied = model().apply({project::MoveTrack{track, to_index}});
  if (!applied) {
    return foundation::Result<void>::failure(applied.error());
  }
  return foundation::Result<void>::success();
}

foundation::Result<void> TrackService::set_track_muted(project::TrackId track, bool muted) {
  auto applied = model().apply({project::SetTrackMuted{track, muted}});
  if (!applied) {
    return foundation::Result<void>::failure(applied.error());
  }
  return foundation::Result<void>::success();
}

foundation::Result<void> TrackService::set_track_soloed(project::TrackId track, bool soloed) {
  auto applied = model().apply({project::SetTrackSoloed{track, soloed}});
  if (!applied) {
    return foundation::Result<void>::failure(applied.error());
  }
  return foundation::Result<void>::success();
}

foundation::Result<void> TrackService::set_track_level_pan(project::TrackId track, double level,
                                                           double pan,
                                                           std::optional<std::uint64_t> gesture) {
  auto applied = model().apply({project::SetTrackLevelPan{track, level, pan}}, gesture);
  if (!applied)
    return foundation::Result<void>::failure(applied.error());
  return foundation::Result<void>::success();
}

foundation::Result<void> TrackService::set_master_level(double level,
                                                        std::optional<std::uint64_t> gesture) {
  auto applied = model().apply({project::SetMasterLevel{level}}, gesture);
  if (!applied)
    return foundation::Result<void>::failure(applied.error());
  return foundation::Result<void>::success();
}

foundation::Result<project::TrackId>
TrackService::add_bus_track(project::TrackKind kind, std::optional<std::size_t> insert_index) {
  if (kind != project::TrackKind::group_bus && kind != project::TrackKind::return_bus) {
    return foundation::Result<project::TrackId>::failure(
        {foundation::ErrorCode::invalid_argument, "Only group and return buses can be added"});
  }
  auto id = numbered_id<project::TrackId>(next_track_number());
  if (!id) {
    return foundation::Result<project::TrackId>::failure(id.error());
  }
  auto applied = model().apply({project::AddTrack{id.value(), kind, insert_index}});
  if (!applied) {
    return foundation::Result<project::TrackId>::failure(applied.error());
  }
  return foundation::Result<project::TrackId>::success(id.value());
}

foundation::Result<project::TrackId>
TrackService::add_audio_track(std::optional<std::size_t> insert_index) {
  auto id = numbered_id<project::TrackId>(next_track_number());
  if (!id)
    return foundation::Result<project::TrackId>::failure(id.error());
  auto applied =
      model().apply({project::AddTrack{id.value(), project::TrackKind::audio, insert_index}});
  if (!applied)
    return foundation::Result<project::TrackId>::failure(applied.error());
  return foundation::Result<project::TrackId>::success(id.value());
}

// ---- Default return tracks ------------------------------------------------

foundation::Result<void> TrackService::seed_default_returns() {
  using Result = foundation::Result<void>;
  struct DefaultReturn {
    std::string_view name;
    std::string_view engine;
    std::string_view preset;
    std::string_view mix;
  };
  // Returns carry only the effect, so it runs fully wet.
  constexpr std::array defaults{
      DefaultReturn{"A · Reverb", "expanse", "Vocal Plate", "expanse.mix"},
      DefaultReturn{"B · Delay", "relay", "Dotted Eighth", "relay.mix"}};
  std::vector<project::Operation> operations;
  auto next_track = next_track_number();
  for (const auto& item : defaults) {
    auto id = numbered_id<project::TrackId>(next_track);
    if (!id)
      return Result::failure(id.error());
    next_track = next_number(next_track);
    const auto* engine = native_dsp::effects::find_engine(item.engine);
    if (engine == nullptr)
      return Result::failure({foundation::ErrorCode::unavailable, "Default return effect missing"});
    std::optional<std::size_t> preset;
    for (std::size_t index = 0; index < engine->descriptor->presets.size(); ++index) {
      if (engine->descriptor->presets[index].name == item.preset)
        preset = index;
    }
    auto patch = native_inserts::native_effect_patch_for_preset(*engine, preset);
    if (!patch)
      return Result::failure(patch.error());
    auto resolved = native_inserts::resolve_native_effect(patch.value());
    if (!resolved)
      return Result::failure(resolved.error());
    const auto mix = native_dsp::synth::find_parameter(*engine->descriptor, item.mix);
    if (!mix)
      return Result::failure({foundation::ErrorCode::internal, "Default return has no mix"});
    resolved.value().values[*mix] = 1.0;
    auto wet = native_inserts::make_native_effect_patch(*engine, resolved.value().values,
                                                        patch.value().document);
    if (!wet)
      return Result::failure(wet.error());
    operations.emplace_back(project::AddTrack{id.value(), project::TrackKind::return_bus});
    operations.emplace_back(project::SetTrackName{id.value(), std::string(item.name)});
    operations.emplace_back(
        project::AddNativeEffect{project::SignalPathId{id.value()}, 0,
                                 project::NativeEffectProcessor{std::move(wet).value()}});
  }
  auto applied = model().apply(operations);
  return applied ? Result::success() : Result::failure(applied.error());
}

// ---- Presentation: names, colours, markers ---------------------------------

foundation::Result<void> TrackService::set_track_name(project::TrackId track, std::string name) {
  const auto found = std::ranges::find(snapshot().tracks, track, &project::Track::id);
  if (found != snapshot().tracks.end() && found->name == name)
    return foundation::Result<void>::success();
  auto applied = model().apply({project::SetTrackName{track, std::move(name)}});
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

foundation::Result<void> TrackService::set_track_color(project::TrackId track,
                                                       std::optional<std::uint32_t> color) {
  const auto found = std::ranges::find(snapshot().tracks, track, &project::Track::id);
  if (found != snapshot().tracks.end() && found->color == color)
    return foundation::Result<void>::success();
  auto applied = model().apply({project::SetTrackColor{track, color}});
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

foundation::Result<project::TrackId> TrackService::duplicate_track(project::TrackId track) {
  using foundation::ErrorCode;
  using Result = foundation::Result<project::TrackId>;
  const auto& state = snapshot();
  const auto found = std::ranges::find(state.tracks, track, &project::Track::id);
  if (found == state.tracks.end())
    return Result::failure({ErrorCode::not_found, "Track to duplicate not found"});
  const auto& original = *found;
  if (original.kind != project::TrackKind::audio && original.kind != project::TrackKind::instrument)
    return Result::failure(
        {ErrorCode::unsupported, "Duplicate works on audio and instrument tracks"});
  auto id = numbered_id<project::TrackId>(next_track_number());
  if (!id)
    return Result::failure(id.error());
  const auto copy = id.value();
  auto ids = fresh_identities(state.revision);
  if (!ids)
    return Result::failure(ids.error());
  const auto row = static_cast<std::size_t>(found - state.tracks.begin());
  std::vector<project::Operation> operations;
  operations.emplace_back(project::AddTrack{copy, original.kind, row + 1});

  // The copy's plugins, effect slots and sends get fresh identities; its
  // lanes follow them.
  std::map<project::PluginInstanceId, project::PluginInstanceId> plugins;
  std::map<project::InsertSlotId, project::InsertSlotId> slots;
  std::map<project::SendId, project::SendId> sends;
  auto next_plugin = next_plugin_number();
  const auto fresh_plugin = [&](const project::Vst3PluginInstance& plugin)
      -> foundation::Result<project::Vst3PluginInstance> {
    auto plugin_id = numbered_id<project::PluginInstanceId>(next_plugin);
    if (!plugin_id)
      return foundation::Result<project::Vst3PluginInstance>::failure(plugin_id.error());
    next_plugin = next_number(next_plugin);
    auto renamed = plugin;
    renamed.id = plugin_id.value();
    plugins.emplace(plugin.id, renamed.id);
    return foundation::Result<project::Vst3PluginInstance>::success(std::move(renamed));
  };

  const bool vst3_effects =
      std::ranges::any_of(original.effects, [](const project::EffectInsert& slot) {
        return std::holds_alternative<project::Vst3PluginInstance>(slot.processor);
      });
  // Without an instrument, the model takes a VST3 inserted into an empty chain
  // as the instrument. A placeholder synth holds that role while the chain is
  // rebuilt and is cleared in the same command.
  const bool placeholder = original.kind == project::TrackKind::instrument &&
                           std::holds_alternative<std::monostate>(original.instrument_source) &&
                           vst3_effects;
  if (const auto* native =
          std::get_if<project::NativeInstrumentPatch>(&original.instrument_source)) {
    operations.emplace_back(project::SetNativeInstrumentPatch{copy, *native});
  } else if (const auto* instrument =
                 std::get_if<project::Vst3PluginInstance>(&original.instrument_source)) {
    auto renamed = fresh_plugin(*instrument);
    if (!renamed)
      return Result::failure(renamed.error());
    operations.emplace_back(project::InsertVst3Plugin{copy, 0, std::move(renamed).value()});
  } else if (placeholder) {
    auto patch = native_dsp::synth::native_patch_for_preset(native_dsp::synth::default_engine(),
                                                            std::nullopt);
    if (!patch)
      return Result::failure(patch.error());
    operations.emplace_back(project::SetNativeInstrumentPatch{copy, std::move(patch).value()});
  }
  // Each new slot takes the next allocator serial in operation order.
  auto serial = state.next_insert_slot_serial;
  for (std::size_t index = 0; index < original.effects.size(); ++index) {
    const auto& effect = original.effects[index];
    auto slot = project::command_insert_slot_id(serial++);
    if (!slot)
      return Result::failure(slot.error());
    slots.emplace(effect.id, slot.value());
    auto added = std::visit(
        [&](const auto& processor) -> foundation::Result<void> {
          using Processor = std::decay_t<decltype(processor)>;
          if constexpr (std::is_same_v<Processor, project::Vst3PluginInstance>) {
            auto renamed = fresh_plugin(processor);
            if (!renamed)
              return foundation::Result<void>::failure(renamed.error());
            operations.emplace_back(
                project::InsertVst3Plugin{copy, index, std::move(renamed).value()});
          } else {
            operations.emplace_back(project::AddNativeEffect{copy, index, processor});
          }
          return foundation::Result<void>::success();
        },
        effect.processor);
    if (!added)
      return Result::failure(added.error());
    if (!effect.enabled)
      operations.emplace_back(project::SetEffectInsertEnabled{copy, slot.value(), false});
  }
  if (placeholder)
    operations.emplace_back(project::SetNativeInstrumentPatch{copy, std::nullopt});
  // Note effects take the following serials, in order (ADR-0059).
  for (std::size_t index = 0; index < original.note_effects.size(); ++index) {
    const auto& effect = original.note_effects[index];
    auto slot = project::command_insert_slot_id(serial++);
    if (!slot)
      return Result::failure(slot.error());
    slots.emplace(effect.id, slot.value());
    operations.emplace_back(project::AddNoteEffect{copy, index, effect.engine,
                                                   effect.engine_version, effect.parameters});
    if (!effect.enabled)
      operations.emplace_back(project::SetNoteEffectEnabled{copy, slot.value(), false});
  }
  if (!original.macros.empty()) {
    auto macros = original.macros;
    for (auto& macro : macros) {
      for (auto& mapping : macro.mappings) {
        if (auto* native = std::get_if<project::NativeParameterTarget>(&mapping.destination)) {
          native->track = copy;
        } else {
          auto& effect = std::get<project::NativeEffectParameterTarget>(mapping.destination);
          const auto renamed = slots.find(effect.slot);
          if (renamed == slots.end())
            return Result::failure(
                {ErrorCode::internal, "A duplicated macro names a slot outside its track"});
          effect.slot = renamed->second;
        }
      }
    }
    operations.emplace_back(project::SetTrackMacros{copy, std::move(macros)});
  }

  if (original.main_output)
    operations.emplace_back(project::SetTrackMainOutput{copy, original.main_output});
  auto next_send = next_send_number();
  for (const auto& send : original.sends) {
    auto send_id = numbered_id<project::SendId>(next_send);
    if (!send_id)
      return Result::failure(send_id.error());
    next_send = next_number(next_send);
    sends.emplace(send.id, send_id.value());
    auto route = send;
    route.id = send_id.value();
    operations.emplace_back(project::AddSendRoute{copy, route});
  }

  const auto follow = [&](std::vector<project::AutomationLane>& lanes) -> foundation::Result<void> {
    const auto rename = [](const auto& renamed, auto& target_id) {
      const auto entry = renamed.find(target_id);
      if (entry == renamed.end())
        return false;
      target_id = entry->second;
      return true;
    };
    for (auto& lane : lanes) {
      const bool followed = std::visit(
          [&](auto& target) {
            using Target = std::decay_t<decltype(target)>;
            if constexpr (std::is_same_v<Target, project::TrackControlTarget> ||
                          std::is_same_v<Target, project::NativeParameterTarget> ||
                          std::is_same_v<Target, project::MacroTarget>) {
              target.track = copy;
              return true;
            } else if constexpr (std::is_same_v<Target, project::NativeEffectParameterTarget>) {
              return rename(slots, target.slot);
            } else if constexpr (std::is_same_v<Target, project::Vst3ParameterTarget>) {
              return rename(plugins, target.plugin);
            } else {
              return rename(sends, target.send);
            }
          },
          lane.target);
      if (!followed)
        return foundation::Result<void>::failure(
            {ErrorCode::internal, "Duplicated automation names a target outside its track"});
    }
    refresh_lanes(lanes, ids.value());
    return foundation::Result<void>::success();
  };

  auto next_clip = next_clip_number();
  const auto fresh_clip = [&]() {
    auto clip_id = numbered_id<project::ClipId>(next_clip);
    next_clip = next_number(next_clip);
    return clip_id;
  };
  for (const auto& clip : original.clips) {
    auto clip_id = fresh_clip();
    if (!clip_id)
      return Result::failure(clip_id.error());
    project::Clip copied = clip;
    copied.id = clip_id.value();
    if (auto followed = follow(copied.automation); !followed)
      return Result::failure(followed.error());
    operations.emplace_back(project::AddClip{copy, std::move(copied)});
  }
  for (const auto& clip : original.midi_clips) {
    auto clip_id = fresh_clip();
    if (!clip_id)
      return Result::failure(clip_id.error());
    project::MidiClip copied = clip;
    copied.id = clip_id.value();
    for (auto& note : copied.notes)
      note.id = project::NoteId::from_bits({ids.value().space, ids.value().next_note++}).value();
    if (auto followed = follow(copied.automation); !followed)
      return Result::failure(followed.error());
    operations.emplace_back(project::AddMidiClip{copy, std::move(copied)});
  }
  auto lanes = original.automation;
  if (auto followed = follow(lanes); !followed)
    return Result::failure(followed.error());
  for (auto& lane : lanes)
    operations.emplace_back(project::AddAutomationLane{copy, std::move(lane)});

  if (original.muted)
    operations.emplace_back(project::SetTrackMuted{copy, true});
  if (original.soloed)
    operations.emplace_back(project::SetTrackSoloed{copy, true});
  if (original.level != 1.0 || original.pan != 0.0)
    operations.emplace_back(project::SetTrackLevelPan{copy, original.level, original.pan});
  if (!original.name.empty())
    operations.emplace_back(project::SetTrackName{copy, original.name});
  if (original.color)
    operations.emplace_back(project::SetTrackColor{copy, original.color});
  // The copy sounds the same, so it plays the same render.
  if (original.frozen)
    operations.emplace_back(project::SetTrackFrozen{copy, original.frozen});
  auto applied = model().apply(operations);
  return applied ? Result::success(copy) : Result::failure(applied.error());
}

} // namespace oscillate::application
