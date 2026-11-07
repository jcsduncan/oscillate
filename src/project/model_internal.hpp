#pragma once

// Private to the project model: helpers shared by its validate, apply and
// history translation units.

#include "oscillate/project/model.hpp"

#include "oscillate/foundation/error.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace oscillate::project::detail {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] Error invalid(std::string message);

[[nodiscard]] Error conflict(std::string message);

[[nodiscard]] Error exhausted(std::string message);

// High word of every command-allocated effect slot ID; the low word is the serial.
inline constexpr std::uint64_t command_slot_namespace = 0x6c646177736c6f74ULL;

[[nodiscard]] Result<InsertSlotId> allocate_insert_slot(Snapshot& snapshot);

template <typename Id> void add_unique(std::vector<Id>& ids, Id id) {
  if (std::ranges::find(ids, id) == ids.end()) {
    ids.push_back(id);
  }
}

[[nodiscard]] const NativeInstrumentPatch* native_source(const Track& track);

[[nodiscard]] bool valid_native_identifier(std::string_view id);

// Macro mappings die with their parameter's owner (ADR-0059).
void remove_macro_mappings(Track& track, const auto& refers) {
  for (auto& macro : track.macros)
    std::erase_if(macro.mappings,
                  [&](const MacroMapping& mapping) { return refers(mapping.destination); });
}

// Sends that `doomed` matches leave with what they lead to (a removed track,
// a removed sidechain target), together with the automation of their gains.
template <typename Doomed> void remove_sends(Snapshot& state, Doomed&& doomed, ChangeSet& changes) {
  for (auto& track : state.tracks) {
    std::vector<SendId> dropped;
    std::erase_if(track.sends, [&](const SendRoute& send) {
      if (!doomed(send)) {
        return false;
      }
      dropped.push_back(send.id);
      return true;
    });
    if (dropped.empty()) {
      continue;
    }
    add_unique(changes.tracks, track.id);
    changes.routing = true;
    const auto on_dropped = [&](const AutomationLane& lane) {
      const auto* gain = std::get_if<SendGainTarget>(&lane.target);
      if (gain == nullptr || std::ranges::find(dropped, gain->send) == dropped.end()) {
        return false;
      }
      add_unique(changes.automation_lanes, lane.id);
      changes.automation = true;
      return true;
    };
    std::erase_if(track.automation, on_dropped);
    for (auto& clip : track.clips) {
      std::erase_if(clip.automation, on_dropped);
    }
    for (auto& clip : track.midi_clips) {
      std::erase_if(clip.automation, on_dropped);
    }
  }
}

void remove_effect_automation(Track& track, InsertSlotId slot, ChangeSet& changes);

[[nodiscard]] std::vector<const Vst3PluginInstance*> track_plugins(const Track& track);

[[nodiscard]] bool lowercase_hex(std::string_view value, std::size_t length);

[[nodiscard]] bool valid_beat_frames(const std::vector<std::uint64_t>& markers,
                                     std::uint64_t source_frames);

[[nodiscard]] bool valid_key(MusicalKey key);

[[nodiscard]] bool valid_text(std::string_view value, std::size_t limit, bool nonempty);

[[nodiscard]] Result<void>
validate_parameters(const std::vector<Vst3ParameterReference>& parameters);

[[nodiscard]] Result<void> validate_plugin(const Vst3PluginInstance& plugin);

[[nodiscard]] Result<void> validate_source(const SourceRecord& source);

[[nodiscard]] const SourceRecord* managed_audio_source(const ManagedAudioReference& reference,
                                                       const Snapshot& snapshot);

[[nodiscard]] bool valid_unit_tension(double value);

[[nodiscard]] Result<void> validate_native_document(const NativeSynthDocument& document,
                                                    const Snapshot& snapshot);

[[nodiscard]] Result<void> validate_native_effect_patch(const NativeEffectPatch& effect,
                                                        const Snapshot& snapshot);

[[nodiscard]] Result<void> validate_native_convolution(const NativeConvolution& effect,
                                                       const Snapshot& snapshot);

[[nodiscard]] bool has_clip_id(const Snapshot& state, ClipId id);

struct MutableMidiClip {
  Track* track;
  MidiClip* clip;
};

[[nodiscard]] std::optional<MutableMidiClip> find_mutable_midi_clip(Snapshot& state, ClipId id);

struct MutableClip {
  Track* track;
  Clip* clip;
};

[[nodiscard]] std::optional<MutableClip> find_mutable_clip(Snapshot& state, ClipId id);

[[nodiscard]] Result<void> validate(const Snapshot& state);

// The candidate snapshot and change records one command applies to.
struct ApplyContext {
  Snapshot& candidate;
  ChangeSet& changes;
  std::set<SourceId>& source_ids;
  std::set<SourceId>& changed_source_ids;
};

// One overload per Operation alternative, defined by domain in model_apply_*.cpp.
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const AddTrack& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const RemoveTrack& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const MoveTrack& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetTrackMuted& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetTrackSoloed& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetTrackLevelPan& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetMasterLevel& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetTrackMainOutput& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const AddSendRoute& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const RemoveSendRoute& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetSendGain& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const AddClip& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const RemoveClip& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const AddSource& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const RemoveSource& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const RelinkSource& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context,
                                         const ApplySourceKeyAnalysis& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context,
                                         const SetSourceKeyCorrection& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context,
                                         const ApplySourceTempoAnalysis& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context,
                                         const SetSourceTempoCorrection& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetSourceDownbeat& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context,
                                         const SetSourceUserBeatFrames& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context,
                                         const ApplySourceOnsetAnalysis& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context,
                                         const SetSourceDismissedOnsets& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetClipPlacement& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetClipSourceRange& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetClipFades& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetClipProcessing& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetClipClay& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const ReplaceTempoMap& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const ReplaceMeterMap& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetSampleRate& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context,
                                         const SetNativeInstrumentPatch& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const InsertVst3Plugin& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const AddNativeEffect& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context,
                                         const SetNativeEffectSettings& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const MoveEffectInsert& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context,
                                         const SetEffectInsertEnabled& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const RemoveEffectInsert& command);
// After an edit's operations: an effect the edit left alone in its linked
// group (its partners removed or linked elsewhere) is unlinked.
[[nodiscard]] Result<void> release_lone_links(const Snapshot& before, ApplyContext& context);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const RemoveVst3Plugin& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const MoveVst3Plugin& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context,
                                         const SetVst3ComponentState& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context,
                                         const SetVst3ControllerState& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context,
                                         const SetVst3ParameterReferences& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetVst3Missing& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const RelinkVst3Plugin& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const AddMidiClip& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context,
                                         const SetMidiClipPlacement& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetMidiPattern& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const AddMidiNote& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const RemoveMidiNote& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetMidiNote& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetMidiPedalLane& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const AddAutomationLane& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context,
                                         const ReplaceAutomationLane& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context,
                                         const RemoveAutomationLane& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetTrackName& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetTrackColor& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetClipName& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetClipColor& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetLeadBars& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetProjectKey& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const AddMarker& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const MoveMarker& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const RenameMarker& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const RemoveMarker& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const AddNoteEffect& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context,
                                         const SetNoteEffectSettings& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const MoveNoteEffect& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context,
                                         const SetNoteEffectEnabled& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const RemoveNoteEffect& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetTrackMacros& command);
[[nodiscard]] Result<void> apply_command(ApplyContext& context, const SetTrackFrozen& command);

[[nodiscard]] Result<void> apply_one(ApplyContext& context, const Operation& operation);

} // namespace oscillate::project::detail
