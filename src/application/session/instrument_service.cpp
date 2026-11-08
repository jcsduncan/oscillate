#include "oscillate/application/session/instrument_service.hpp"

#include "edit_support.hpp"
#include "midi_clips.hpp"
#include "oscillate/application/key_cache.hpp"
#include "oscillate/application/midi_note_tools.hpp"
#include "oscillate/application/tempo_cache.hpp"
#include "oscillate/midi_file/smf.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/lock.hpp"
#include "oscillate/native_dsp/synth/project_binding.hpp"
#include "session_state.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::application {
namespace {

using ChannelKey = project::TrackControlTarget::Key;
using detail::find_midi_clip;
using detail::midi_anchor_tick;

// A saved patch's Lock switch: stored values omit defaults, and Lock
// defaults to off. Any other choice locks (Anvil's Bass note and Keep tone).
bool lock_switch_on(const project::NativeInstrumentPatch& patch, std::string_view parameter) {
  const auto found =
      std::ranges::find(patch.parameters, parameter, &project::NativeInstrumentParameter::id);
  return found != patch.parameters.end() && found->value != native_dsp::synth::lock_choice_off;
}

} // namespace

foundation::Result<project::TrackId>
InstrumentService::add_synth_track(std::optional<std::size_t> insert_index) {
  return add_native_instrument_track(native_dsp::synth::default_engine().descriptor->id,
                                     std::nullopt, insert_index);
}

foundation::Result<project::TrackId>
InstrumentService::add_native_instrument_track(std::string_view engine,
                                               std::optional<std::size_t> preset,
                                               std::optional<std::size_t> insert_index) {
  using Result = foundation::Result<project::TrackId>;
  const auto* found = native_dsp::synth::find_engine(engine);
  if (found == nullptr) {
    return Result::failure({foundation::ErrorCode::not_found, "Built-in synth not found"});
  }
  auto id = numbered_id<project::TrackId>(next_track_number());
  if (!id) {
    return Result::failure(id.error());
  }
  auto patch = native_dsp::synth::native_patch_for_preset(*found, preset);
  if (!patch) {
    return Result::failure(patch.error());
  }
  auto applied =
      model().apply({project::AddTrack{id.value(), project::TrackKind::instrument, insert_index},
                     project::SetNativeInstrumentPatch{id.value(), std::move(patch).value()}});
  if (!applied) {
    return Result::failure(applied.error());
  }
  return Result::success(id.value());
}

foundation::Result<void>
InstrumentService::select_native_instrument(project::TrackId track, std::string_view engine,
                                            std::optional<std::size_t> preset) {
  using Result = foundation::Result<void>;
  const auto* found = native_dsp::synth::find_engine(engine);
  if (found == nullptr) {
    return Result::failure({foundation::ErrorCode::not_found, "Built-in synth not found"});
  }
  auto patch = native_dsp::synth::native_patch_for_preset(*found, preset);
  if (!patch) {
    return Result::failure(patch.error());
  }
  auto applied =
      model().apply({project::SetNativeInstrumentPatch{track, std::move(patch).value()}});
  return applied ? Result::success() : Result::failure(applied.error());
}

foundation::Result<std::vector<double>>
InstrumentService::native_instrument_values(project::TrackId track) const {
  using Result = foundation::Result<std::vector<double>>;
  auto found = native_track(snapshot(), track);
  if (!found) {
    return Result::failure(found.error());
  }
  return Result::success(std::move(found.value().resolved.values));
}

foundation::Result<void> InstrumentService::set_native_instrument_values(
    project::TrackId track, std::span<const double> values, std::optional<std::uint64_t> gesture) {
  using Result = foundation::Result<void>;
  if (gesture && *gesture == 0) {
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Synth gesture ID must be nonzero"});
  }
  auto found = native_track(snapshot(), track);
  if (!found) {
    return Result::failure(found.error());
  }
  auto patch = native_dsp::synth::make_native_patch(*found.value().resolved.engine, values,
                                                    found.value().resolved.document);
  if (!patch) {
    return Result::failure(patch.error());
  }
  if (patch.value() ==
      std::get<project::NativeInstrumentPatch>(found.value().track->instrument_source)) {
    return Result::success();
  }
  auto applied =
      model().apply({project::SetNativeInstrumentPatch{track, std::move(patch).value()}}, gesture);
  return applied ? Result::success() : Result::failure(applied.error());
}

foundation::Result<void> InstrumentService::set_native_instrument_state(
    project::TrackId track, std::vector<project::NativeInstrumentParameter> values,
    project::NativeSynthDocument document, std::optional<std::uint64_t> gesture) {
  using Result = foundation::Result<void>;
  if (gesture && *gesture == 0) {
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Synth gesture ID must be nonzero"});
  }
  auto found = native_track(snapshot(), track);
  if (!found) {
    return Result::failure(found.error());
  }
  const auto& current =
      std::get<project::NativeInstrumentPatch>(found.value().track->instrument_source);
  // Resolve the candidate as the engine sees it, then store its canonical
  // form (only non-default values).
  project::NativeInstrumentPatch candidate{current.engine, current.engine_version,
                                           std::move(values), std::move(document)};
  auto resolved = native_dsp::synth::resolve_native_instrument(candidate);
  if (!resolved) {
    return Result::failure(resolved.error());
  }
  auto patch = native_dsp::synth::make_native_patch(
      *resolved.value().engine, resolved.value().values, std::move(resolved.value().document));
  if (!patch) {
    return Result::failure(patch.error());
  }
  if (patch.value() == current) {
    return Result::success();
  }
  auto applied =
      model().apply({project::SetNativeInstrumentPatch{track, std::move(patch).value()}}, gesture);
  return applied ? Result::success() : Result::failure(applied.error());
}

KickBassLockState kick_bass_lock_state(const project::Snapshot& snapshot, project::TrackId track) {
  namespace synth = native_dsp::synth;
  const auto bass = std::ranges::find(snapshot.tracks, track, &project::Track::id);
  const auto* bass_patch =
      bass != snapshot.tracks.end()
          ? std::get_if<project::NativeInstrumentPatch>(&bass->instrument_source)
          : nullptr;
  if (bass_patch == nullptr || bass_patch->engine != synth::lock_bass_engine) {
    return {};
  }
  KickBassLockState state;
  bool kicks_locked = true;
  bool any_unmuted = false;
  for (const auto& other : snapshot.tracks) {
    const auto* patch = std::get_if<project::NativeInstrumentPatch>(&other.instrument_source);
    if (patch == nullptr || patch->engine != synth::lock_kick_engine) {
      continue;
    }
    state.offered = true;
    if (!other.muted) {
      any_unmuted = true;
      kicks_locked = kicks_locked && lock_switch_on(*patch, synth::lock_kick_parameter);
    }
  }
  state.locked = state.offered && any_unmuted && kicks_locked &&
                 lock_switch_on(*bass_patch, synth::lock_bass_parameter);
  return state;
}

foundation::Result<void> InstrumentService::lock_kick_and_bass(project::TrackId bass, bool on) {
  using Result = foundation::Result<void>;
  namespace synth = native_dsp::synth;
  const auto& current = snapshot();
  auto found = native_track(current, bass);
  if (!found) {
    return Result::failure(found.error());
  }
  if (found.value().resolved.engine->descriptor->id != synth::lock_bass_engine) {
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Kick & Bass Lock needs a Nova bass track"});
  }
  std::vector<project::Operation> operations;
  // One track's patch with its Lock switch at `value` (`on_value` for the
  // bass, Keep tone for a kick), unless it already reads that way: a kick
  // already on Bass note stays there.
  const auto switch_lock = [&](const NativeTrack& track, std::string_view parameter,
                               double on_value) -> Result {
    const auto& engine = *track.resolved.engine;
    const auto index = native_dsp::find_parameter(*engine.descriptor, parameter);
    if (!index) {
      return Result::failure(
          {foundation::ErrorCode::not_found, "The synth has no Kick & Bass Lock switch"});
    }
    const double current = track.resolved.values[*index];
    if ((current != synth::lock_choice_off) == on) {
      return Result::success();
    }
    const double value = on ? on_value : synth::lock_choice_off;
    auto values = track.resolved.values;
    values[*index] = value;
    auto patch = synth::make_native_patch(engine, values, track.resolved.document);
    if (!patch) {
      return Result::failure(patch.error());
    }
    operations.emplace_back(
        project::SetNativeInstrumentPatch{track.track->id, std::move(patch).value()});
    return Result::success();
  };
  if (auto switched = switch_lock(found.value(), synth::lock_bass_parameter, synth::lock_choice_on);
      !switched) {
    return switched;
  }
  for (const auto& track : current.tracks) {
    const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
    if (track.muted || patch == nullptr || patch->engine != synth::lock_kick_engine) {
      continue;
    }
    auto kick = native_track(current, track.id);
    if (!kick) {
      return Result::failure(kick.error());
    }
    // The user keeps the kick's drawn tone: the pair forms without retuning.
    if (auto switched =
            switch_lock(kick.value(), synth::lock_kick_parameter, synth::lock_kick_keep_tone);
        !switched) {
      return switched;
    }
  }
  if (operations.empty()) {
    return Result::success();
  }
  auto applied = model().apply(operations);
  return applied ? Result::success() : Result::failure(applied.error());
}

foundation::Result<project::NativeSynthDocument>
InstrumentService::native_synth_document(project::TrackId track) const {
  using Result = foundation::Result<project::NativeSynthDocument>;
  auto found = native_track(snapshot(), track);
  if (!found) {
    return Result::failure(found.error());
  }
  return Result::success(std::move(found.value().resolved.document));
}

foundation::Result<void>
InstrumentService::set_native_synth_document(project::TrackId track,
                                             project::NativeSynthDocument document,
                                             std::optional<std::uint64_t> gesture) {
  using Result = foundation::Result<void>;
  if (gesture && *gesture == 0) {
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Synth gesture ID must be nonzero"});
  }
  auto found = native_track(snapshot(), track);
  if (!found) {
    return Result::failure(found.error());
  }
  auto patch = std::get<project::NativeInstrumentPatch>(found.value().track->instrument_source);
  if (patch.document == document) {
    return Result::success();
  }
  patch.document = std::move(document);
  if (auto resolved = native_dsp::synth::resolve_native_instrument(patch); !resolved) {
    return Result::failure(resolved.error());
  }
  auto applied =
      model().apply({project::SetNativeInstrumentPatch{track, std::move(patch)}}, gesture);
  return applied ? Result::success() : Result::failure(applied.error());
}

foundation::Result<project::TrackId>
InstrumentService::create_vst3_instrument_track(Vst3InsertIdentity identity) {
  // Any number of instrument tracks of any kind play together (ADR-0050).
  auto track = numbered_id<project::TrackId>(next_track_number());
  auto plugin = numbered_id<project::PluginInstanceId>(next_plugin_number());
  if (!track)
    return foundation::Result<project::TrackId>::failure(track.error());
  if (!plugin)
    return foundation::Result<project::TrackId>::failure(plugin.error());
  auto instance = make_vst3_instance(plugin.value(), std::move(identity));
  auto applied = model().apply({project::AddTrack{track.value(), project::TrackKind::instrument},
                                project::InsertVst3Plugin{track.value(), 0, std::move(instance)}});
  if (!applied)
    return foundation::Result<project::TrackId>::failure(applied.error());
  auto saved = saving_.save();
  if (!saved) {
    return foundation::Result<project::TrackId>::failure(
        {saved.error().code,
         "VST3 track accepted in current project, but save failed: " + saved.error().message});
  }
  return foundation::Result<project::TrackId>::success(track.value());
}

foundation::Result<void> InstrumentService::set_vst3_instrument_state(
    std::uint64_t opened_revision, project::TrackId track,
    const project::Vst3PluginInstance& original, std::vector<std::uint8_t> state,
    std::vector<project::Vst3ParameterReference> parameters, EditorSavePolicy save_policy) {
  return set_vst3_instrument_state(opened_revision, track, original, std::move(state),
                                   original.controller_state, std::move(parameters), save_policy);
}

foundation::Result<void> InstrumentService::set_vst3_instrument_state(
    std::uint64_t opened_revision, project::TrackId track,
    const project::Vst3PluginInstance& original, std::vector<std::uint8_t> component_state,
    std::optional<std::vector<std::uint8_t>> controller_state,
    std::vector<project::Vst3ParameterReference> parameters, EditorSavePolicy save_policy) {
  const auto& current = snapshot();
  const auto owner = std::ranges::find(current.tracks, track, &project::Track::id);
  const auto* source = owner == current.tracks.end()
                           ? nullptr
                           : std::get_if<project::Vst3PluginInstance>(&owner->instrument_source);
  if (current.revision != opened_revision || owner == current.tracks.end() ||
      owner->kind != project::TrackKind::instrument || source == nullptr || *source != original)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::conflict, "VST3 instrument editor source or revision changed"});
  auto applied =
      model().apply({project::SetVst3ComponentState{original.id, std::move(component_state)},
                     project::SetVst3ControllerState{original.id, std::move(controller_state)},
                     project::SetVst3ParameterReferences{original.id, std::move(parameters)}});
  if (!applied)
    return foundation::Result<void>::failure(applied.error());
  if (save_policy == EditorSavePolicy::background)
    return foundation::Result<void>::success();
  auto saved = saving_.save();
  if (!saved)
    return foundation::Result<void>::failure(
        {saved.error().code,
         "VST3 instrument state accepted in current project, but save failed: " +
             saved.error().message});
  return foundation::Result<void>::success();
}

} // namespace oscillate::application
