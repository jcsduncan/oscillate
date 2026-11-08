#include "oscillate/application/session/effect_service.hpp"

#include "edit_support.hpp"
#include "midi_clips.hpp"
#include "oscillate/application/key_cache.hpp"
#include "oscillate/application/midi_note_tools.hpp"
#include "oscillate/application/tempo_cache.hpp"
#include "oscillate/midi_file/smf.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "session_state.hpp"

#include <algorithm>
#include <cstdint>
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

// A built-in effect anywhere in the project: its path and saved patch.
struct NativeEffectPlace {
  project::SignalPathId path;
  project::InsertSlotId slot;
  const project::NativeEffectPatch* patch;
};

std::vector<NativeEffectPlace> native_effects(const project::Snapshot& snapshot) {
  std::vector<NativeEffectPlace> found;
  const auto collect = [&](const project::SignalPathId& path,
                           const std::vector<project::EffectInsert>& effects) {
    for (const auto& effect : effects) {
      if (const auto* patch = std::get_if<project::NativeEffectPatch>(&effect.processor))
        found.push_back({path, effect.id, patch});
    }
  };
  for (const auto& track : snapshot.tracks)
    collect(track.id, track.effects);
  collect(project::MasterSignalPath{}, snapshot.master_effects);
  return found;
}

// The other members of a linked effect's group (none when unlinked).
std::vector<NativeEffectPlace> link_partners(const project::Snapshot& snapshot,
                                             const project::NativeEffectPatch& patch,
                                             project::InsertSlotId self) {
  std::vector<NativeEffectPlace> partners;
  if (patch.link.empty())
    return partners;
  for (auto& place : native_effects(snapshot)) {
    if (place.slot != self && place.patch->engine == patch.engine &&
        place.patch->link == patch.link)
      partners.push_back(place);
  }
  return partners;
}

// Settings for `patch` and every partner whose document differs from it,
// so a linked group's documents move in one command.
std::vector<project::Operation> with_partners(const project::Snapshot& snapshot,
                                              project::SignalPathId path,
                                              project::InsertSlotId slot,
                                              project::NativeEffectPatch patch) {
  std::vector<project::Operation> operations;
  for (const auto& partner : link_partners(snapshot, patch, slot)) {
    if (partner.patch->document == patch.document)
      continue;
    auto copy = *partner.patch;
    copy.document = patch.document;
    operations.emplace_back(project::SetNativeEffectSettings{partner.path, partner.slot, copy});
  }
  operations.emplace(operations.begin(),
                     project::SetNativeEffectSettings{path, slot, std::move(patch)});
  return operations;
}

// A fresh link, unique for the life of the project: the founding slot and
// the revision that founds the group.
std::string new_link(project::InsertSlotId founder, std::uint64_t revision) {
  return founder.to_string() + "-" + std::to_string(revision + 1);
}

[[nodiscard]] const std::vector<project::EffectInsert>*
chain_of(const project::Snapshot& snapshot, const project::SignalPathId& path) {
  if (const auto* track = std::get_if<project::TrackId>(&path)) {
    const auto found = std::ranges::find(snapshot.tracks, *track, &project::Track::id);
    return found == snapshot.tracks.end() ? nullptr : &found->effects;
  }
  return &snapshot.master_effects;
}

} // namespace

foundation::Result<project::InsertSlotId>
EffectService::add_effect_engine(project::SignalPathId path, std::size_t index,
                                 std::string_view engine, std::optional<std::size_t> preset) {
  using Result = foundation::Result<project::InsertSlotId>;
  const auto* found = native_dsp::effects::find_engine(engine);
  if (found == nullptr)
    return Result::failure({foundation::ErrorCode::not_found, "Built-in effect not found"});
  auto patch = native_inserts::native_effect_patch_for_preset(*found, preset);
  if (!patch)
    return Result::failure(patch.error());
  return add_native_effect(path, index, std::move(patch).value());
}

foundation::Result<std::vector<double>>
EffectService::native_effect_values(project::SignalPathId path, project::InsertSlotId slot) const {
  using Result = foundation::Result<std::vector<double>>;
  auto found = native_effect_slot(snapshot(), path, slot);
  if (!found)
    return Result::failure(found.error());
  return Result::success(std::move(found.value().resolved.values));
}

foundation::Result<void>
EffectService::set_native_effect_values(project::SignalPathId path, project::InsertSlotId slot,
                                        std::span<const double> values,
                                        std::optional<std::uint64_t> gesture) {
  using Result = foundation::Result<void>;
  if (gesture && *gesture == 0)
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Effect gesture ID must be nonzero"});
  auto found = native_effect_slot(snapshot(), path, slot);
  if (!found)
    return Result::failure(found.error());
  auto patch = native_inserts::make_native_effect_patch(*found.value().resolved.engine, values,
                                                        found.value().patch->document);
  if (!patch)
    return Result::failure(patch.error());
  if (patch.value().parameters == found.value().patch->parameters)
    return Result::success();
  patch.value().link = found.value().patch->link;
  auto applied = model().apply(
      {project::SetNativeEffectSettings{path, slot, std::move(patch).value()}}, gesture);
  return applied ? Result::success() : Result::failure(applied.error());
}

foundation::Result<void>
EffectService::set_native_effect_preset(project::SignalPathId path, project::InsertSlotId slot,
                                        std::size_t preset, std::optional<std::uint64_t> gesture) {
  using Result = foundation::Result<void>;
  auto found = native_effect_slot(snapshot(), path, slot);
  if (!found)
    return Result::failure(found.error());
  auto patch =
      native_inserts::native_effect_patch_for_preset(*found.value().resolved.engine, preset);
  if (!patch)
    return Result::failure(patch.error());
  if (patch.value().parameters == found.value().patch->parameters &&
      patch.value().document == found.value().patch->document)
    return Result::success();
  // The preset's values are this instance's; its shape is the group's.
  patch.value().link = found.value().patch->link;
  auto applied =
      model().apply(with_partners(snapshot(), path, slot, std::move(patch).value()), gesture);
  return applied ? Result::success() : Result::failure(applied.error());
}

foundation::Result<project::NativeSynthDocument>
EffectService::native_effect_document(project::SignalPathId path,
                                      project::InsertSlotId slot) const {
  using Result = foundation::Result<project::NativeSynthDocument>;
  auto found = native_effect_slot(snapshot(), path, slot);
  if (!found)
    return Result::failure(found.error());
  return Result::success(found.value().patch->document);
}

foundation::Result<void>
EffectService::set_native_effect_document(project::SignalPathId path, project::InsertSlotId slot,
                                          project::NativeSynthDocument document,
                                          std::optional<std::uint64_t> gesture) {
  using Result = foundation::Result<void>;
  if (gesture && *gesture == 0)
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Effect gesture ID must be nonzero"});
  auto found = native_effect_slot(snapshot(), path, slot);
  if (!found)
    return Result::failure(found.error());
  auto patch = *found.value().patch;
  if (patch.document == document)
    return Result::success();
  patch.document = std::move(document);
  if (auto resolved = native_inserts::resolve_native_effect(patch); !resolved)
    return Result::failure(resolved.error());
  auto applied = model().apply(with_partners(snapshot(), path, slot, std::move(patch)), gesture);
  return applied ? Result::success() : Result::failure(applied.error());
}

foundation::Result<void> EffectService::set_native_effect_state(
    project::SignalPathId path, project::InsertSlotId slot, std::span<const double> values,
    project::NativeSynthDocument document, std::optional<std::uint64_t> gesture) {
  using Result = foundation::Result<void>;
  if (gesture && *gesture == 0)
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Effect gesture ID must be nonzero"});
  auto found = native_effect_slot(snapshot(), path, slot);
  if (!found)
    return Result::failure(found.error());
  auto patch = native_inserts::make_native_effect_patch(*found.value().resolved.engine, values,
                                                        std::move(document));
  if (!patch)
    return Result::failure(patch.error());
  if (patch.value().parameters == found.value().patch->parameters &&
      patch.value().document == found.value().patch->document)
    return Result::success();
  patch.value().link = found.value().patch->link;
  auto applied =
      model().apply(with_partners(snapshot(), path, slot, std::move(patch).value()), gesture);
  return applied ? Result::success() : Result::failure(applied.error());
}

foundation::Result<void>
EffectService::link_native_effects(std::span<const project::InsertSlotId> slots) {
  using Result = foundation::Result<void>;
  const auto all = native_effects(snapshot());
  std::vector<NativeEffectPlace> members;
  for (const auto slot : slots) {
    const auto found = std::ranges::find(all, slot, &NativeEffectPlace::slot);
    if (found == all.end())
      return Result::failure({foundation::ErrorCode::not_found, "Linked effect not found"});
    if (std::ranges::find(members, slot, &NativeEffectPlace::slot) == members.end())
      members.push_back(*found);
  }
  if (members.size() < 2)
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Linking needs two or more effects"});
  const auto& first = *members.front().patch;
  if (!std::ranges::all_of(members, [&](const NativeEffectPlace& member) {
        return member.patch->engine == first.engine &&
               member.patch->engine_version == first.engine_version;
      }))
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Only effects of one engine can be linked"});
  const auto link = new_link(members.front().slot, snapshot().revision);
  std::vector<project::Operation> operations;
  for (const auto& member : members) {
    auto patch = *member.patch;
    patch.link = link;
    patch.document = first.document;
    operations.emplace_back(project::SetNativeEffectSettings{member.path, member.slot, patch});
  }
  auto applied = model().apply(operations);
  return applied ? Result::success() : Result::failure(applied.error());
}

foundation::Result<void> EffectService::unlink_native_effect(project::SignalPathId path,
                                                             project::InsertSlotId slot) {
  using Result = foundation::Result<void>;
  auto found = native_effect_slot(snapshot(), path, slot);
  if (!found)
    return Result::failure(found.error());
  if (found.value().patch->link.empty())
    return Result::success();
  auto patch = *found.value().patch;
  patch.link.clear();
  std::vector<project::Operation> operations{
      project::SetNativeEffectSettings{path, slot, std::move(patch)}};
  // A group of one is no group.
  const auto partners = link_partners(snapshot(), *found.value().patch, slot);
  if (partners.size() == 1) {
    auto last = *partners.front().patch;
    last.link.clear();
    operations.emplace_back(
        project::SetNativeEffectSettings{partners.front().path, partners.front().slot, last});
  }
  auto applied = model().apply(operations);
  return applied ? Result::success() : Result::failure(applied.error());
}

std::size_t EffectService::native_effect_link_count(project::InsertSlotId slot) const {
  const auto all = native_effects(snapshot());
  const auto found = std::ranges::find(all, slot, &NativeEffectPlace::slot);
  if (found == all.end())
    return 0;
  return 1 + link_partners(snapshot(), *found->patch, slot).size();
}

foundation::Result<std::size_t>
EffectService::duck_tracks_to(project::TrackId source, std::span<const project::TrackId> targets) {
  using Result = foundation::Result<std::size_t>;
  const auto& state = snapshot();
  if (std::ranges::find(state.tracks, source, &project::Track::id) == state.tracks.end())
    return Result::failure({foundation::ErrorCode::not_found, "Key track not found"});
  const auto* pump = native_dsp::effects::find_engine("pump");
  if (pump == nullptr)
    return Result::failure({foundation::ErrorCode::unavailable, "Pump is not available"});
  auto fresh = native_inserts::native_effect_patch_for_preset(*pump, std::nullopt);
  if (!fresh)
    return Result::failure(fresh.error());

  // Each target's Pump: its first, or a new one at the end of its chain.
  // New slots take the serials the model will give them, in order.
  struct Member {
    project::TrackId track;
    project::InsertSlotId slot;
    const project::NativeEffectPatch* existing; // Null for a new Pump.
    std::size_t index;                          // Where a new one goes.
  };
  std::vector<Member> members;
  auto serial = state.next_insert_slot_serial;
  for (const auto target : targets) {
    if (target == source || std::ranges::find(members, target, &Member::track) != members.end())
      continue;
    const auto track = std::ranges::find(state.tracks, target, &project::Track::id);
    if (track == state.tracks.end())
      continue;
    const auto reused = std::ranges::find_if(track->effects, [&](const project::EffectInsert& e) {
      const auto* patch = std::get_if<project::NativeEffectPatch>(&e.processor);
      return patch != nullptr && patch->engine == pump->descriptor->id;
    });
    if (reused != track->effects.end()) {
      members.push_back(
          {target, reused->id, &std::get<project::NativeEffectPatch>(reused->processor), 0});
      continue;
    }
    if (track->effects.size() >= project::max_effect_slots_per_path)
      continue;
    auto slot = project::command_insert_slot_id(serial++);
    if (!slot)
      return Result::failure(slot.error());
    members.push_back({target, slot.value(), nullptr, track->effects.size()});
  }
  if (members.empty())
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Choose tracks other than the key to duck"});

  // One shape for the group: the first reused Pump's, else the default.
  const auto shaped = std::ranges::find_if(
      members, [](const Member& member) { return member.existing != nullptr; });
  const auto document =
      shaped != members.end() ? shaped->existing->document : fresh.value().document;
  const auto link = new_link(members.front().slot, state.revision);
  std::vector<project::Operation> operations;
  for (const auto& member : members) {
    auto patch = member.existing != nullptr ? *member.existing : fresh.value();
    patch.document = document;
    patch.link = link;
    if (member.existing == nullptr)
      operations.emplace_back(project::AddNativeEffect{
          member.track, member.index, project::NativeEffectProcessor{std::move(patch)}});
    else if (!(patch == *member.existing))
      operations.emplace_back(
          project::SetNativeEffectSettings{member.track, member.slot, std::move(patch)});
  }

  // Key every member from the source, replacing any other key it had.
  auto send_number = next_send_number();
  for (const auto& member : members) {
    bool keyed = false;
    for (const auto& track : state.tracks) {
      for (const auto& send : track.sends) {
        if (send.purpose != project::SendPurpose::sidechain || send.destination != member.track ||
            send.target_insert != member.slot)
          continue;
        if (track.id == source && !keyed)
          keyed = true;
        else
          operations.emplace_back(project::RemoveSendRoute{send.id});
      }
    }
    if (keyed)
      continue;
    auto id = numbered_id<project::SendId>(send_number);
    if (!id)
      return Result::failure(id.error());
    send_number = next_number(send_number);
    operations.emplace_back(project::AddSendRoute{
        source, {id.value(), member.track, 1.0, project::SendPurpose::sidechain, member.slot}});
  }
  auto applied = model().apply(operations);
  if (!applied)
    return Result::failure(applied.error());
  return Result::success(members.size());
}

foundation::Result<project::PluginInstanceId>
EffectService::insert_vst3_plugin(project::TrackId track, std::size_t index,
                                  Vst3InsertIdentity identity) {
  auto id = numbered_id<project::PluginInstanceId>(next_plugin_number());
  if (!id)
    return foundation::Result<project::PluginInstanceId>::failure(id.error());
  auto plugin = make_vst3_instance(id.value(), std::move(identity));
  auto applied = model().apply({project::InsertVst3Plugin{track, index, std::move(plugin)}});
  if (!applied)
    return foundation::Result<project::PluginInstanceId>::failure(applied.error());
  auto saved = saving_.save();
  if (!saved) {
    return foundation::Result<project::PluginInstanceId>::failure(
        {saved.error().code,
         "VST3 insert accepted in current project, but save failed: " + saved.error().message});
  }
  return foundation::Result<project::PluginInstanceId>::success(id.value());
}

foundation::Result<void> EffectService::set_vst3_editor_state(
    std::uint64_t opened_revision, project::TrackId track, project::InsertSlotId slot,
    const project::Vst3PluginInstance& original, std::vector<std::uint8_t> state,
    std::vector<project::Vst3ParameterReference> parameters, EditorSavePolicy save_policy) {
  return set_vst3_editor_state(opened_revision, track, slot, original, std::move(state),
                               original.controller_state, std::move(parameters), save_policy);
}

foundation::Result<void> EffectService::set_vst3_editor_state(
    std::uint64_t opened_revision, project::TrackId track, project::InsertSlotId slot,
    const project::Vst3PluginInstance& original, std::vector<std::uint8_t> component_state,
    std::optional<std::vector<std::uint8_t>> controller_state,
    std::vector<project::Vst3ParameterReference> parameters, EditorSavePolicy save_policy) {
  const auto& current = snapshot();
  const auto owner = std::ranges::find(current.tracks, track, &project::Track::id);
  if (current.revision != opened_revision || owner == current.tracks.end())
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::conflict, "VST3 editor project revision or track changed"});
  const auto found = std::ranges::find(owner->effects, slot, &project::EffectInsert::id);
  const auto* plugin = found == owner->effects.end()
                           ? nullptr
                           : std::get_if<project::Vst3PluginInstance>(&found->processor);
  if (!plugin || *plugin != original)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::conflict, "VST3 editor insert identity or state changed"});
  auto applied =
      model().apply({project::SetVst3ComponentState{original.id, std::move(component_state)},
                     project::SetVst3ControllerState{original.id, std::move(controller_state)},
                     project::SetVst3ParameterReferences{original.id, std::move(parameters)}});
  if (!applied)
    return foundation::Result<void>::failure(applied.error());
  if (save_policy == EditorSavePolicy::background)
    return foundation::Result<void>::success();
  auto saved = saving_.save();
  if (!saved) {
    return foundation::Result<void>::failure(
        {saved.error().code, "VST3 editor state accepted in current project, but save failed: " +
                                 saved.error().message});
  }
  return foundation::Result<void>::success();
}

foundation::Result<void>
EffectService::relink_vst3_plugin(std::uint64_t opened_revision, project::TrackId track,
                                  std::optional<project::InsertSlotId> slot,
                                  const project::Vst3PluginInstance& original,
                                  Vst3InsertIdentity verified) {
  const auto& current = snapshot();
  const auto owner = std::ranges::find(current.tracks, track, &project::Track::id);
  if (current.revision != opened_revision || owner == current.tracks.end()) {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::conflict, "VST3 relink project revision or track changed"});
  }
  const project::Vst3PluginInstance* saved = nullptr;
  if (slot) {
    const auto found = std::ranges::find(owner->effects, *slot, &project::EffectInsert::id);
    if (found != owner->effects.end())
      saved = std::get_if<project::Vst3PluginInstance>(&found->processor);
  } else if (owner->kind == project::TrackKind::instrument) {
    saved = std::get_if<project::Vst3PluginInstance>(&owner->instrument_source);
  }
  if (!saved || *saved != original) {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::conflict, "VST3 relink slot, source or saved state changed"});
  }
  if (verified.class_id != saved->class_id || verified.vendor != saved->vendor ||
      verified.name != saved->name || verified.version != saved->version ||
      verified.binary_sha256 != saved->binary_sha256) {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::conflict, "VST3 relink requires the original class and binary"});
  }
  auto applied =
      model().apply({project::RelinkVst3Plugin{saved->id, saved->bundle_path, saved->binary_sha256,
                                               std::move(verified.canonical_bundle_path)}});
  if (!applied)
    return foundation::Result<void>::failure(applied.error());
  auto saved_result = saving_.save();
  if (!saved_result) {
    return foundation::Result<void>::failure(
        {saved_result.error().code, "VST3 relink accepted in current project, but save failed: " +
                                        saved_result.error().message});
  }
  return foundation::Result<void>::success();
}

foundation::Result<project::InsertSlotId>
EffectService::add_native_effect(project::SignalPathId path, std::size_t index,
                                 project::NativeEffectProcessor processor) {
  auto applied = model().apply({project::AddNativeEffect{path, index, std::move(processor)}});
  if (!applied)
    return foundation::Result<project::InsertSlotId>::failure(applied.error());
  const auto* effects = &snapshot().master_effects;
  if (const auto* track = std::get_if<project::TrackId>(&path)) {
    const auto found = std::ranges::find(snapshot().tracks, *track, &project::Track::id);
    if (found == snapshot().tracks.end())
      return foundation::Result<project::InsertSlotId>::failure(
          {foundation::ErrorCode::internal, "Inserted effect path disappeared"});
    effects = &found->effects;
  }
  if (index >= effects->size()) {
    return foundation::Result<project::InsertSlotId>::failure(
        {foundation::ErrorCode::internal, "Inserted native effect slot is unavailable"});
  }
  return foundation::Result<project::InsertSlotId>::success((*effects)[index].id);
}

foundation::Result<void> EffectService::set_native_effect(project::SignalPathId path,
                                                          project::InsertSlotId slot,
                                                          project::NativeEffectProcessor processor,
                                                          std::optional<std::uint64_t> gesture) {
  auto applied =
      model().apply({project::SetNativeEffectSettings{path, slot, std::move(processor)}}, gesture);
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

foundation::Result<void> EffectService::move_effect_insert(project::SignalPathId path,
                                                           project::InsertSlotId slot,
                                                           std::size_t index) {
  auto applied = model().apply({project::MoveEffectInsert{path, slot, index}});
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

foundation::Result<void> EffectService::set_effect_insert_enabled(project::SignalPathId path,
                                                                  project::InsertSlotId slot,
                                                                  bool enabled) {
  auto applied = model().apply({project::SetEffectInsertEnabled{path, slot, enabled}});
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

foundation::Result<void> EffectService::remove_effect_insert(project::SignalPathId path,
                                                             project::InsertSlotId slot) {
  // Keys aimed at the effect go with it, in the same undo step.
  std::vector<project::Operation> operations;
  if (const auto* owner = std::get_if<project::TrackId>(&path)) {
    for (const auto& track : snapshot().tracks) {
      for (const auto& send : track.sends) {
        if (send.purpose == project::SendPurpose::sidechain && send.destination == *owner &&
            send.target_insert == slot)
          operations.push_back(project::RemoveSendRoute{send.id});
      }
    }
  }
  operations.push_back(project::RemoveEffectInsert{path, slot});
  auto applied = model().apply(operations);
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

Result<std::vector<project::InsertSlotId>>
EffectService::apply_effect_edits(std::span<const EffectEdit> edits) {
  using R = Result<std::vector<project::InsertSlotId>>;
  if (edits.empty())
    return R::failure({ErrorCode::invalid_argument, "Nothing to change"});
  const auto& state = snapshot();
  std::vector<project::Operation> operations;
  std::vector<project::InsertSlotId> slots;
  auto serial = state.next_insert_slot_serial;
  // New slots per path, so several additions to one chain stay in range.
  std::vector<std::pair<project::SignalPathId, std::size_t>> added;
  for (const auto& edit : edits) {
    const auto* chain = chain_of(state, edit.path);
    if (chain == nullptr)
      return R::failure({ErrorCode::not_found, "Effect track not found"});
    const auto* engine = native_dsp::effects::find_engine(edit.engine);
    if (engine == nullptr)
      return R::failure({ErrorCode::not_found, "Built-in effect not found: " + edit.engine});
    project::NativeEffectPatch base;
    if (edit.slot) {
      const auto slot = std::ranges::find(*chain, *edit.slot, &project::EffectInsert::id);
      if (slot == chain->end())
        return R::failure({ErrorCode::not_found, "Effect slot not on signal path"});
      const auto* patch = std::get_if<project::NativeEffectPatch>(&slot->processor);
      if (patch == nullptr || patch->engine != edit.engine)
        return R::failure({ErrorCode::invalid_argument, "The slot is not " + edit.engine});
      base = *patch;
    } else {
      auto fresh = native_inserts::native_effect_patch_for_preset(*engine, std::nullopt);
      if (!fresh)
        return R::failure(fresh.error());
      base = std::move(fresh).value();
    }
    auto resolved = native_inserts::resolve_native_effect(base);
    if (!resolved)
      return R::failure(resolved.error());
    auto values = std::move(resolved.value().values);
    for (const auto& [id, value] : edit.values) {
      const auto index = native_dsp::find_parameter(*engine->descriptor, id);
      if (!index)
        return R::failure({ErrorCode::not_found, "No parameter " + id + " on " + edit.engine});
      values[*index] = value;
    }
    auto patch = native_inserts::make_native_effect_patch(*engine, values, base.document);
    if (!patch)
      return R::failure(patch.error());
    patch.value().link = base.link;
    if (const auto slot = edit.slot) {
      slots.push_back(*slot);
      if (!(patch.value().parameters == base.parameters))
        operations.emplace_back(
            project::SetNativeEffectSettings{edit.path, *slot, std::move(patch).value()});
      continue;
    }
    auto entry =
        std::ranges::find(added, edit.path, &std::pair<project::SignalPathId, std::size_t>::first);
    if (entry == added.end()) {
      added.emplace_back(edit.path, 0);
      entry = added.end() - 1;
    }
    const auto length = chain->size() + entry->second;
    if (length >= project::max_effect_slots_per_path)
      return R::failure({ErrorCode::resource_exhausted, "The effect chain is full"});
    ++entry->second;
    auto slot = project::command_insert_slot_id(serial++);
    if (!slot)
      return R::failure(slot.error());
    slots.push_back(slot.value());
    operations.emplace_back(
        project::AddNativeEffect{edit.path, std::min(edit.index.value_or(length), length),
                                 project::NativeEffectProcessor{std::move(patch).value()}});
  }
  if (operations.empty())
    return R::success(std::move(slots));
  auto applied = model().apply(operations);
  if (!applied)
    return R::failure(applied.error());
  return R::success(std::move(slots));
}

} // namespace oscillate::application
