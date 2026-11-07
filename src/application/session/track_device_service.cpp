#include "oscillate/application/session/track_device_service.hpp"

#include "edit_support.hpp"
#include "midi_clips.hpp"
#include "oscillate/application/comp_patterns.hpp"
#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/native_dsp/note_effects/note_effect.hpp"
#include "oscillate/native_dsp/synth/project_binding.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "oscillate/persistence/rack_file.hpp"
#include "oscillate/project/lookup.hpp"
#include "session_state.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::application {
namespace {

using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;
using project::find_track;

[[nodiscard]] const project::NoteEffectInsert*
find_comp(const project::Snapshot& state, project::TrackId track, project::InsertSlotId slot) {
  const auto owner = std::ranges::find(state.tracks, track, &project::Track::id);
  if (owner == state.tracks.end())
    return nullptr;
  const auto insert = std::ranges::find(owner->note_effects, slot, &project::NoteEffectInsert::id);
  return insert != owner->note_effects.end() && insert->engine == comp::engine_id ? &*insert
                                                                                  : nullptr;
}

struct Renamed {
  project::InsertSlotId from;
  project::InsertSlotId to;
};

// The macros with every destination moved from one track's slots to
// another's; a mapping to a slot that was not renamed is dropped.
[[nodiscard]] std::vector<project::Macro> retarget(std::vector<project::Macro> macros,
                                                   project::TrackId track,
                                                   const std::vector<Renamed>& renamed) {
  for (auto& macro : macros) {
    std::erase_if(macro.mappings, [&](project::MacroMapping& mapping) {
      if (auto* native = std::get_if<project::NativeParameterTarget>(&mapping.destination)) {
        native->track = track;
        return false;
      }
      auto& target = std::get<project::NativeEffectParameterTarget>(mapping.destination);
      const auto found = std::ranges::find(renamed, target.slot, &Renamed::from);
      if (found == renamed.end())
        return true;
      target.slot = found->to;
      return false;
    });
  }
  return macros;
}

[[nodiscard]] const project::NoteEffectInsert* find_note_effect(const project::Track& track,
                                                                project::InsertSlotId slot) {
  const auto found = std::ranges::find(track.note_effects, slot, &project::NoteEffectInsert::id);
  return found == track.note_effects.end() ? nullptr : &*found;
}

// The track's macros with `index` present: missing macros up to it are added
// at their defaults.
[[nodiscard]] Result<std::vector<project::Macro>> macros_through(const project::Track& track,
                                                                 std::uint32_t index) {
  if (index >= project::max_macros_per_track)
    return Result<std::vector<project::Macro>>::failure(
        {ErrorCode::invalid_argument, "A track has eight macros"});
  auto macros = track.macros;
  if (macros.size() <= index)
    macros.resize(index + 1);
  return Result<std::vector<project::Macro>>::success(std::move(macros));
}

// Where a macro destination lives on its track.
struct DestinationOwner {
  const project::EffectInsert* effect = nullptr;
  const project::NoteEffectInsert* note = nullptr;
};

[[nodiscard]] DestinationOwner owner_of(const project::Track& track,
                                        const project::NativeEffectParameterTarget& target) {
  DestinationOwner owner;
  const auto effect = std::ranges::find(track.effects, target.slot, &project::EffectInsert::id);
  if (effect != track.effects.end())
    owner.effect = &*effect;
  owner.note = find_note_effect(track, target.slot);
  return owner;
}

// The automation domain of a macro destination, from its engine.
[[nodiscard]] Result<project::AutomationDomain>
destination_domain(const project::Track& track, const project::MacroDestination& destination) {
  using R = Result<project::AutomationDomain>;
  if (const auto* native = std::get_if<project::NativeParameterTarget>(&destination)) {
    const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
    if (native->track != track.id || patch == nullptr)
      return R::failure({ErrorCode::unavailable, "The track has no built-in synth to map"});
    auto resolved = native_dsp::synth::resolve_native_instrument(*patch);
    if (!resolved)
      return R::failure(resolved.error());
    return native_dsp::synth::native_automation_domain(*resolved.value().engine,
                                                       native->parameter_id);
  }
  const auto& target = std::get<project::NativeEffectParameterTarget>(destination);
  const auto owner = owner_of(track, target);
  if (owner.effect != nullptr) {
    const auto* patch = std::get_if<project::NativeEffectPatch>(&owner.effect->processor);
    if (patch == nullptr)
      return R::failure({ErrorCode::unavailable, "Only built-in effects can be mapped"});
    auto resolved = native_inserts::resolve_native_effect(*patch);
    if (!resolved)
      return R::failure(resolved.error());
    return native_inserts::native_effect_automation_domain(*resolved.value().engine,
                                                           target.parameter_id);
  }
  if (owner.note != nullptr) {
    auto resolved = native_dsp::note_effects::resolve_note_effect(*owner.note);
    if (!resolved)
      return R::failure(resolved.error());
    return native_dsp::note_effects::note_effect_automation_domain(*resolved.value().engine,
                                                                   target.parameter_id);
  }
  return R::failure({ErrorCode::not_found, "The mapped slot is not on this track"});
}

// Commands that set every parameter a macro maps to its value at `value`,
// grouped per instrument, effect and note effect; unchanged ones are left out.
[[nodiscard]] Result<std::vector<project::Operation>>
mapped_parameter_edits(const project::Track& track, const project::Macro& macro, double value) {
  using R = Result<std::vector<project::Operation>>;
  std::vector<project::Operation> operations;
  std::optional<native_dsp::synth::ResolvedNativeInstrument> instrument;
  std::vector<std::pair<const project::EffectInsert*, native_inserts::ResolvedNativeEffect>>
      effects;
  std::vector<project::NoteEffectInsert> notes;
  for (const auto& mapping : macro.mappings) {
    const auto plain = automation_project::macro_parameter_value(mapping, value);
    if (const auto* native = std::get_if<project::NativeParameterTarget>(&mapping.destination)) {
      if (!instrument) {
        const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
        if (patch == nullptr)
          return R::failure({ErrorCode::unavailable, "The mapped synth is gone"});
        auto resolved = native_dsp::synth::resolve_native_instrument(*patch);
        if (!resolved)
          return R::failure(resolved.error());
        instrument = std::move(resolved).value();
      }
      auto index =
          native_dsp::synth::native_parameter_index(*instrument->engine, native->parameter_id);
      if (!index)
        return R::failure(index.error());
      instrument->values[index.value()] = plain;
      continue;
    }
    const auto& target = std::get<project::NativeEffectParameterTarget>(mapping.destination);
    const auto owner = owner_of(track, target);
    if (owner.effect != nullptr) {
      auto entry =
          std::ranges::find(effects, owner.effect, [](const auto& item) { return item.first; });
      if (entry == effects.end()) {
        const auto* patch = std::get_if<project::NativeEffectPatch>(&owner.effect->processor);
        if (patch == nullptr)
          return R::failure({ErrorCode::unavailable, "Only built-in effects can be mapped"});
        auto resolved = native_inserts::resolve_native_effect(*patch);
        if (!resolved)
          return R::failure(resolved.error());
        effects.emplace_back(owner.effect, std::move(resolved).value());
        entry = std::prev(effects.end());
      }
      auto index =
          native_inserts::native_effect_parameter_index(*entry->second.engine, target.parameter_id);
      if (!index)
        return R::failure(index.error());
      entry->second.values[index.value()] = plain;
      continue;
    }
    if (owner.note == nullptr)
      return R::failure({ErrorCode::not_found, "The mapped slot is not on this track"});
    auto entry = std::ranges::find(notes, owner.note->id, &project::NoteEffectInsert::id);
    if (entry == notes.end()) {
      notes.push_back(*owner.note);
      entry = std::prev(notes.end());
    }
    // Note effect values are sparse; the engine drops defaults when it saves.
    auto parameter = std::ranges::find(entry->parameters, target.parameter_id,
                                       &project::NativeInstrumentParameter::id);
    if (parameter == entry->parameters.end())
      entry->parameters.push_back({target.parameter_id, plain});
    else
      parameter->value = plain;
  }
  if (instrument) {
    auto patch = native_dsp::synth::make_native_patch(*instrument->engine, instrument->values,
                                                      instrument->document);
    if (!patch)
      return R::failure(patch.error());
    if (patch.value() != std::get<project::NativeInstrumentPatch>(track.instrument_source))
      operations.emplace_back(
          project::SetNativeInstrumentPatch{track.id, std::move(patch).value()});
  }
  for (auto& [slot, resolved] : effects) {
    const auto& saved = std::get<project::NativeEffectPatch>(slot->processor);
    auto patch =
        native_inserts::make_native_effect_patch(*resolved.engine, resolved.values, saved.document);
    if (!patch)
      return R::failure(patch.error());
    if (patch.value().parameters == saved.parameters)
      continue;
    patch.value().link = saved.link;
    operations.emplace_back(project::SetNativeEffectSettings{project::SignalPathId{track.id},
                                                             slot->id, std::move(patch).value()});
  }
  for (auto& note : notes) {
    // Saved in the engine's sparse form: defaults dropped, table order.
    auto resolved = native_dsp::note_effects::resolve_note_effect(note);
    if (!resolved)
      return R::failure(resolved.error());
    auto parameters = native_dsp::note_effects::sparse_note_effect_parameters(
        *resolved.value().engine, resolved.value().values);
    if (parameters != find_note_effect(track, note.id)->parameters)
      operations.emplace_back(
          project::SetNoteEffectSettings{track.id, note.id, std::move(parameters)});
  }
  return R::success(std::move(operations));
}

} // namespace

Result<void> TrackDeviceService::apply_comp_pattern(project::TrackId track,
                                                    project::InsertSlotId slot, std::size_t index) {
  const auto* insert = find_comp(snapshot(), track, slot);
  if (insert == nullptr)
    return Result<void>::failure({ErrorCode::not_found, "That Comp is not on the track"});
  auto parameters = comp::pattern_parameters(index, insert->parameters);
  if (!parameters)
    return Result<void>::failure(parameters.error());
  return set_note_effect_parameters(track, slot, std::move(parameters).value());
}

Result<TrackDeviceService::CapturedRack>
TrackDeviceService::capture_track_rack(project::TrackId track) const {
  using R = Result<CapturedRack>;
  const auto& state = snapshot();
  const auto owner = std::ranges::find(state.tracks, track, &project::Track::id);
  if (owner == state.tracks.end())
    return R::failure({ErrorCode::not_found, "Track not found"});
  CapturedRack captured;
  auto& rack = captured.rack;
  if (const auto* patch = std::get_if<project::NativeInstrumentPatch>(&owner->instrument_source))
    rack.instrument = *patch;
  else if (!std::holds_alternative<std::monostate>(owner->instrument_source))
    ++captured.skipped;
  std::vector<Renamed> renamed;
  std::size_t ordinal = 0;
  for (const auto& effect : owner->note_effects) {
    auto copy = effect;
    copy.id = persistence::rack_slot_id(++ordinal);
    renamed.push_back({effect.id, copy.id});
    rack.note_effects.push_back(std::move(copy));
  }
  for (const auto& effect : owner->effects) {
    const auto* patch = std::get_if<project::NativeEffectPatch>(&effect.processor);
    if (patch == nullptr) {
      ++captured.skipped;
      continue;
    }
    auto copy = *patch;
    copy.link.clear();
    copy.generation = 1;
    const auto id = persistence::rack_slot_id(++ordinal);
    renamed.push_back({effect.id, id});
    rack.effects.push_back({id, effect.enabled, std::move(copy)});
  }
  rack.macros = retarget(owner->macros, persistence::rack_track_id(), renamed);
  if (auto valid = persistence::validate_rack(rack); !valid)
    return R::failure(valid.error());
  return R::success(std::move(captured));
}

Result<void> TrackDeviceService::load_track_rack(project::TrackId track,
                                                 const persistence::TrackRack& rack) {
  if (auto valid = persistence::validate_rack(rack); !valid)
    return valid;
  const auto& state = snapshot();
  const auto owner = std::ranges::find(state.tracks, track, &project::Track::id);
  if (owner == state.tracks.end())
    return Result<void>::failure({ErrorCode::not_found, "Track not found"});
  // The engines check the rack's values and documents before anything moves.
  if (rack.instrument) {
    if (auto resolved = native_dsp::synth::resolve_native_instrument(*rack.instrument); !resolved)
      return Result<void>::failure(resolved.error());
  }
  for (const auto& effect : rack.effects) {
    const auto& patch = std::get<project::NativeEffectPatch>(effect.processor);
    if (auto resolved = native_inserts::resolve_native_effect(patch); !resolved)
      return Result<void>::failure(resolved.error());
  }
  for (const auto& effect : rack.note_effects) {
    if (auto resolved = native_dsp::note_effects::resolve_note_effect(effect); !resolved)
      return Result<void>::failure(resolved.error());
  }
  if (!rack.note_effects.empty() && !rack.instrument &&
      !std::holds_alternative<project::NativeInstrumentPatch>(owner->instrument_source))
    return Result<void>::failure(
        {ErrorCode::unsupported, "MIDI effects play into the built-in synths only"});
  const project::SignalPathId path{track};
  std::vector<project::Operation> operations;
  for (const auto& effect : owner->note_effects)
    operations.emplace_back(project::RemoveNoteEffect{track, effect.id});
  for (const auto& effect : owner->effects) {
    if (std::holds_alternative<project::NativeEffectPatch>(effect.processor))
      operations.emplace_back(project::RemoveEffectInsert{path, effect.id});
  }
  const auto* current = std::get_if<project::NativeInstrumentPatch>(&owner->instrument_source);
  // Changing engine drops the old engine's lanes in the model already.
  const bool same_engine =
      current != nullptr && (!rack.instrument || rack.instrument->engine == current->engine);
  if (rack.instrument)
    operations.emplace_back(project::SetNativeInstrumentPatch{track, rack.instrument});
  // Slots are allocated in command order from the project's high-water mark.
  auto serial = state.next_insert_slot_serial;
  std::vector<Renamed> renamed;
  for (std::size_t index = 0; index < rack.effects.size(); ++index) {
    const auto& effect = rack.effects[index];
    auto id = project::command_insert_slot_id(serial++);
    if (!id)
      return Result<void>::failure(id.error());
    renamed.push_back({effect.id, id.value()});
    auto patch = std::get<project::NativeEffectPatch>(effect.processor);
    patch.link.clear();
    patch.generation = 1;
    operations.emplace_back(project::AddNativeEffect{path, index, std::move(patch)});
    if (!effect.enabled)
      operations.emplace_back(project::SetEffectInsertEnabled{path, id.value(), false});
  }
  for (std::size_t index = 0; index < rack.note_effects.size(); ++index) {
    const auto& effect = rack.note_effects[index];
    auto id = project::command_insert_slot_id(serial++);
    if (!id)
      return Result<void>::failure(id.error());
    renamed.push_back({effect.id, id.value()});
    operations.emplace_back(project::AddNoteEffect{track, index, effect.engine,
                                                   effect.engine_version, effect.parameters});
    if (!effect.enabled)
      operations.emplace_back(project::SetNoteEffectEnabled{track, id.value(), false});
  }
  auto macros = retarget(rack.macros, track, renamed);
  // A parameter a macro now drives cannot keep a lane of its own.
  if (same_engine) {
    for (const auto& lane : owner->automation) {
      const auto* target = std::get_if<project::NativeParameterTarget>(&lane.target);
      if (target == nullptr)
        continue;
      const bool mapped = std::ranges::any_of(macros, [&](const project::Macro& macro) {
        return std::ranges::any_of(macro.mappings, [&](const project::MacroMapping& mapping) {
          const auto* native = std::get_if<project::NativeParameterTarget>(&mapping.destination);
          return native != nullptr && *native == *target;
        });
      });
      if (mapped)
        operations.emplace_back(project::RemoveAutomationLane{lane.id});
    }
  }
  operations.emplace_back(project::SetTrackMacros{track, std::move(macros)});
  auto applied = model().apply(operations);
  if (!applied)
    return Result<void>::failure(applied.error());
  return Result<void>::success();
}

Result<project::InsertSlotId> TrackDeviceService::add_note_effect(
    project::TrackId track, std::size_t index, std::string_view engine,
    std::vector<project::NativeInstrumentParameter> parameters, std::uint32_t engine_version) {
  using R = Result<project::InsertSlotId>;
  // Note effects play into the built-in synths' player (ADR-0059); VST3
  // instruments take the project's notes as they are.
  const auto* owner = find_track(snapshot(), track);
  if (owner == nullptr)
    return R::failure({ErrorCode::not_found, "Track not found"});
  if (!std::holds_alternative<project::NativeInstrumentPatch>(owner->instrument_source))
    return R::failure({ErrorCode::unsupported, "MIDI effects play into the built-in synths only"});
  auto resolved = native_dsp::note_effects::resolve_note_effect(engine, engine_version, parameters);
  if (!resolved)
    return R::failure(resolved.error());
  parameters = native_dsp::note_effects::sparse_note_effect_parameters(*resolved.value().engine,
                                                                       resolved.value().values);
  auto slot = project::command_insert_slot_id(snapshot().next_insert_slot_serial);
  if (!slot)
    return slot;
  auto applied = model().apply({project::AddNoteEffect{track, index, std::string(engine),
                                                       engine_version, std::move(parameters)}});
  if (!applied)
    return R::failure(applied.error());
  return slot;
}

Result<void> TrackDeviceService::remove_note_effect(project::TrackId track,
                                                    project::InsertSlotId slot) {
  return done(model().apply({project::RemoveNoteEffect{track, slot}}));
}

Result<void> TrackDeviceService::move_note_effect(project::TrackId track,
                                                  project::InsertSlotId slot, std::size_t index) {
  return done(model().apply({project::MoveNoteEffect{track, slot, index}}));
}

Result<void> TrackDeviceService::set_note_effect_enabled(project::TrackId track,
                                                         project::InsertSlotId slot, bool enabled) {
  return done(model().apply({project::SetNoteEffectEnabled{track, slot, enabled}}));
}

Result<void> TrackDeviceService::set_note_effect_parameters(
    project::TrackId track, project::InsertSlotId slot,
    std::vector<project::NativeInstrumentParameter> parameters,
    std::optional<std::uint64_t> gesture) {
  return apply_note_effect_settings(track, slot, std::move(parameters), false, gesture);
}

Result<void> TrackDeviceService::enable_note_effect_with_parameters(
    project::TrackId track, project::InsertSlotId slot,
    std::vector<project::NativeInstrumentParameter> parameters,
    std::optional<std::uint64_t> gesture) {
  return apply_note_effect_settings(track, slot, std::move(parameters), true, gesture);
}

Result<void> TrackDeviceService::apply_note_effect_settings(
    project::TrackId track, project::InsertSlotId slot,
    std::vector<project::NativeInstrumentParameter> parameters, bool enable,
    std::optional<std::uint64_t> gesture) {
  if (gesture && *gesture == 0)
    return Result<void>::failure(
        {ErrorCode::invalid_argument, "Note effect gesture ID must be nonzero"});
  const auto* owner = find_track(snapshot(), track);
  const auto* effect = owner ? find_note_effect(*owner, slot) : nullptr;
  if (effect == nullptr)
    return Result<void>::failure({ErrorCode::not_found, "Note effect not on track"});
  auto resolved = native_dsp::note_effects::resolve_note_effect(effect->engine,
                                                                effect->engine_version, parameters);
  if (!resolved)
    return Result<void>::failure(resolved.error());
  parameters = native_dsp::note_effects::sparse_note_effect_parameters(*resolved.value().engine,
                                                                       resolved.value().values);
  std::vector<project::Operation> operations;
  if (effect->parameters != parameters)
    operations.emplace_back(project::SetNoteEffectSettings{track, slot, std::move(parameters)});
  if (enable && !effect->enabled)
    operations.emplace_back(project::SetNoteEffectEnabled{track, slot, true});
  if (operations.empty())
    return Result<void>::success();
  return done(model().apply(operations, gesture));
}

Result<void> TrackDeviceService::set_macro_value(project::TrackId track, std::uint32_t index,
                                                 double value,
                                                 std::optional<std::uint64_t> gesture) {
  if (gesture && *gesture == 0)
    return Result<void>::failure({ErrorCode::invalid_argument, "Macro gesture ID must be nonzero"});
  if (!std::isfinite(value) || value < 0.0 || value > 1.0)
    return Result<void>::failure({ErrorCode::invalid_argument, "A macro turns from 0 to 1"});
  const auto* owner = find_track(snapshot(), track);
  if (owner == nullptr)
    return Result<void>::failure({ErrorCode::not_found, "Macro track not found"});
  auto macros = macros_through(*owner, index);
  if (!macros)
    return Result<void>::failure(macros.error());
  auto& macro = macros.value()[index];
  if (macro.value == value && index < owner->macros.size())
    return Result<void>::success();
  macro.value = value;
  auto operations = mapped_parameter_edits(*owner, macro, value);
  if (!operations)
    return Result<void>::failure(operations.error());
  operations.value().insert(operations.value().begin(),
                            project::SetTrackMacros{track, std::move(macros).value()});
  return done(model().apply(operations.value(), gesture));
}

Result<void> TrackDeviceService::set_macro_name(project::TrackId track, std::uint32_t index,
                                                std::string name) {
  const auto* owner = find_track(snapshot(), track);
  if (owner == nullptr)
    return Result<void>::failure({ErrorCode::not_found, "Macro track not found"});
  auto macros = macros_through(*owner, index);
  if (!macros)
    return Result<void>::failure(macros.error());
  if (macros.value()[index].name == name && index < owner->macros.size())
    return Result<void>::success();
  macros.value()[index].name = std::move(name);
  return done(model().apply({project::SetTrackMacros{track, std::move(macros).value()}}));
}

Result<void> TrackDeviceService::add_macro_mapping(project::TrackId track, std::uint32_t index,
                                                   project::MacroDestination destination,
                                                   double minimum, double maximum) {
  const auto* owner = find_track(snapshot(), track);
  if (owner == nullptr)
    return Result<void>::failure({ErrorCode::not_found, "Macro track not found"});
  auto domain = destination_domain(*owner, destination);
  if (!domain)
    return Result<void>::failure(domain.error());
  if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum < domain.value().minimum ||
      minimum > domain.value().maximum || maximum < domain.value().minimum ||
      maximum > domain.value().maximum)
    return Result<void>::failure(
        {ErrorCode::invalid_argument, "The macro's range is outside the parameter's"});
  auto macros = macros_through(*owner, index);
  if (!macros)
    return Result<void>::failure(macros.error());
  auto& macro = macros.value()[index];
  macro.mappings.push_back({std::move(destination), std::move(domain).value(), minimum, maximum});
  // The parameter follows the macro from now on.
  auto operations = mapped_parameter_edits(
      *owner, project::Macro{{}, macro.value, {macro.mappings.back()}}, macro.value);
  if (!operations)
    return Result<void>::failure(operations.error());
  operations.value().insert(operations.value().begin(),
                            project::SetTrackMacros{track, std::move(macros).value()});
  return done(model().apply(operations.value()));
}

Result<void>
TrackDeviceService::remove_macro_mapping(project::TrackId track, std::uint32_t index,
                                         const project::MacroDestination& destination) {
  const auto* owner = find_track(snapshot(), track);
  if (owner == nullptr || index >= owner->macros.size())
    return Result<void>::failure({ErrorCode::not_found, "Macro not found"});
  auto macros = owner->macros;
  const auto erased = std::erase_if(macros[index].mappings, [&](const project::MacroMapping& m) {
    return m.destination == destination;
  });
  if (erased == 0)
    return Result<void>::failure({ErrorCode::not_found, "The macro does not map that parameter"});
  return done(model().apply({project::SetTrackMacros{track, std::move(macros)}}));
}

Result<void> TrackDeviceService::set_macro_mapping_range(
    project::TrackId track, std::uint32_t index, const project::MacroDestination& destination,
    double minimum, double maximum, std::optional<std::uint64_t> gesture) {
  if (gesture && *gesture == 0)
    return Result<void>::failure({ErrorCode::invalid_argument, "Macro gesture ID must be nonzero"});
  const auto* owner = find_track(snapshot(), track);
  if (owner == nullptr || index >= owner->macros.size())
    return Result<void>::failure({ErrorCode::not_found, "Macro not found"});
  auto macros = owner->macros;
  auto& macro = macros[index];
  const auto mapping =
      std::ranges::find(macro.mappings, destination, &project::MacroMapping::destination);
  if (mapping == macro.mappings.end())
    return Result<void>::failure({ErrorCode::not_found, "The macro does not map that parameter"});
  const auto& domain = mapping->domain;
  if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum < domain.minimum ||
      minimum > domain.maximum || maximum < domain.minimum || maximum > domain.maximum)
    return Result<void>::failure(
        {ErrorCode::invalid_argument, "The macro's range is outside the parameter's"});
  if (mapping->minimum == minimum && mapping->maximum == maximum)
    return Result<void>::success();
  mapping->minimum = minimum;
  mapping->maximum = maximum;
  // The parameter follows the new range at once.
  auto operations =
      mapped_parameter_edits(*owner, project::Macro{{}, macro.value, {*mapping}}, macro.value);
  if (!operations)
    return Result<void>::failure(operations.error());
  operations.value().insert(operations.value().begin(),
                            project::SetTrackMacros{track, std::move(macros)});
  return done(model().apply(operations.value(), gesture));
}

} // namespace oscillate::application
