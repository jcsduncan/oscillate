#pragma once

// Project commands (one Operation each) and the change set an apply reports.

#include "oscillate/project/snapshot.hpp"
#include "oscillate/timeline/timing.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::project {

struct AddTrack {
  TrackId id;
  TrackKind kind = TrackKind::audio;
  // Omitted places the track with its kind: a return bus last, any other
  // track above the first return bus. An explicit index is used as given.
  std::optional<std::size_t> insert_index = std::nullopt;
};
// Also removes the routes into the track: other tracks' sends to it (with
// their gain automation), and their outputs into it fall back to the master.
struct RemoveTrack {
  TrackId id;
};
struct MoveTrack {
  TrackId id;
  // Final index in the saved track vector, after moving this track.
  std::size_t to_index;
};
struct SetTrackMuted {
  TrackId id;
  bool muted;
};
struct SetTrackSoloed {
  TrackId id;
  bool soloed;
};
// Presentation commands. They change no audio and are
// reported through ChangeSet::presentation only.
struct SetTrackName {
  TrackId id;
  std::string name;
};
struct SetTrackColor {
  TrackId id;
  std::optional<std::uint32_t> color;
};
// An audio clip; empty shows the track's name.
struct SetClipName {
  ClipId id;
  std::string name;
};
// An audio or MIDI clip.
struct SetClipColor {
  ClipId id;
  std::optional<std::uint32_t> color;
};
struct AddMarker {
  Marker marker;
};
struct MoveMarker {
  MarkerId id;
  timeline::TickPosition position;
};
struct RenameMarker {
  MarkerId id;
  std::string name;
};
struct RemoveMarker {
  MarkerId id;
};
struct SetTrackLevelPan {
  TrackId id;
  double level;
  double pan;
};
struct SetMasterLevel {
  double level;
};
// Master rebalance (ADR-0123): the whole setting, the role a track's
// musician chose (none: inferred) and the role heard in its audio.
struct SetMasterRebalance {
  MasterRebalance rebalance;
};
struct SetTrackMixRole {
  TrackId id;
  std::optional<MixRole> role;
};
struct SetTrackHeardRole {
  TrackId id;
  std::optional<MixRole> role;
};
// Sets or clears the project key.
struct SetProjectKey {
  std::optional<MusicalKey> key;
};
struct SetLeadBars {
  std::uint32_t bars;
};
struct SetTrackMainOutput {
  TrackId track;
  std::optional<TrackId> destination;
};
struct AddSendRoute {
  TrackId track;
  SendRoute send;
};
struct RemoveSendRoute {
  SendId id;
};
struct SetSendGain {
  SendId id;
  double gain;
};
struct AddClip {
  TrackId track;
  Clip clip;
};
struct RemoveClip {
  ClipId id;
};
struct AddSource {
  SourceRecord source;
};
struct RemoveSource {
  SourceId id;
};
// Changes only an explicit external link. The source's encoded identity and
// every clip reference remain stable; filesystem verification belongs to the store.
struct RelinkSource {
  std::uint64_t origin_revision;
  SourceId id;
  std::string expected_last_known_path;
  std::string replacement_path;
};
// A worker captures a project revision and immutable source metadata before
// analysis. Any intervening edit, including undo/redo, rejects the completion.
struct ApplySourceTempoAnalysis {
  std::uint64_t origin_revision;
  SourceId source;
  std::string source_sha256;
  std::uint32_t sample_rate;
  std::uint64_t frames;
  double bpm;
  double confidence;
  std::vector<std::uint64_t> generated_beat_frames;
  bool suggestions_truncated;
};
struct SetSourceTempoCorrection {
  SourceId source;
  std::optional<double> bpm;
};
struct SetSourceDownbeat {
  SourceId source;
  std::optional<std::uint64_t> frame;
};
struct ApplySourceKeyAnalysis {
  std::uint64_t origin_revision;
  SourceId source;
  std::string source_sha256;
  std::uint32_t sample_rate;
  std::uint64_t frames;
  std::optional<MusicalKey> detected;
  double confidence;
  std::vector<CompatibleKey> compatible;
};
struct SetSourceKeyCorrection {
  SourceId source;
  std::optional<MusicalKey> correction;
};
struct SetSourceUserBeatFrames {
  SourceId source;
  std::vector<std::uint64_t> frames;
};
// Captured like ApplySourceTempoAnalysis; replaces only generated onsets.
struct ApplySourceOnsetAnalysis {
  std::uint64_t origin_revision;
  SourceId source;
  std::string source_sha256;
  std::uint32_t sample_rate;
  std::uint64_t frames;
  std::vector<std::uint64_t> generated_onset_frames;
  bool truncated;
};
// Replaces the user's dismissed onsets; generated onsets near them are
// removed. Removing a dismissal does not restore an old suggestion.
struct SetSourceDismissedOnsets {
  SourceId source;
  std::vector<std::uint64_t> frames;
};
struct SetClipPlacement {
  ClipId id;
  Placement placement;
};
struct SetClipSourceRange {
  ClipId id;
  SourceRange range;
};
struct SetClipFades {
  ClipId id;
  ClipFades fades;
};
struct SetClipProcessing {
  ClipId id;
  ClipProcessing processing;
};
// An audio clip's Clay take; nullopt removes it.
struct SetClipClay {
  ClipId id;
  std::optional<ClayTake> take;
};
struct ReplaceTempoMap {
  timeline::TempoMap map;
};
struct ReplaceMeterMap {
  timeline::MeterMap map;
};
struct SetSampleRate {
  std::uint32_t rate;
};
// Selects or edits a built-in synth. It may replace an empty or native
// source; a VST3 instrument is removed through its own commands.
struct SetNativeInstrumentPatch {
  TrackId track;
  std::optional<NativeInstrumentPatch> patch;
};
struct AddMidiClip {
  TrackId track;
  MidiClip clip;
};
struct SetMidiClipPlacement {
  ClipId id;
  Placement placement;
};
struct SetMidiPattern {
  ClipId id;
  timeline::TickPosition length;
  bool loop;
};
struct AddMidiNote {
  ClipId clip;
  MidiNote note;
};
struct RemoveMidiNote {
  ClipId clip;
  NoteId note;
};
struct SetMidiNote {
  ClipId clip;
  MidiNote note;
};
// Replaces the clip's lane for `pedal`; an empty point list removes it.
struct SetMidiPedalLane {
  ClipId clip;
  MidiPedal pedal;
  std::vector<MidiPedalPoint> points;
};
struct InsertVst3Plugin {
  TrackId track;
  std::size_t index;
  Vst3PluginInstance plugin;
};
struct RemoveVst3Plugin {
  PluginInstanceId id;
};
struct MoveVst3Plugin {
  TrackId track;
  PluginInstanceId id;
  std::size_t index;
};
// Effect positions are zero-based. Move's index is in the chain after removing
// the selected slot. The instrument source is never part of this chain.
struct AddNativeEffect {
  SignalPathId path;
  std::size_t index;
  NativeEffectPatch processor;
};
// Replaces only the settings of an existing native insert. The saved generation
// advances monotonically; the caller's processor generation is not authoritative.
struct SetNativeEffectSettings {
  SignalPathId path;
  InsertSlotId id;
  NativeEffectPatch processor;
};
struct MoveEffectInsert {
  SignalPathId path;
  InsertSlotId id;
  std::size_t index;
};
struct SetEffectInsertEnabled {
  SignalPathId path;
  InsertSlotId id;
  bool enabled;
};
struct RemoveEffectInsert {
  SignalPathId path;
  InsertSlotId id;
};
struct SetVst3ComponentState {
  PluginInstanceId id;
  std::vector<std::uint8_t> state;
};
struct SetVst3ControllerState {
  PluginInstanceId id;
  std::optional<std::vector<std::uint8_t>> state;
};
struct SetVst3ParameterReferences {
  PluginInstanceId id;
  std::vector<Vst3ParameterReference> parameters;
};
struct SetVst3Missing {
  PluginInstanceId id;
  bool missing;
};
struct RelinkVst3Plugin {
  PluginInstanceId id;
  std::string expected_bundle_path;
  std::string expected_binary_sha256;
  std::string canonical_bundle_path;
};
// Note effects (ADR-0059) on an instrument track. Add allocates the slot id
// from next_insert_slot_serial; settings replace the sparse values; a slot
// keeps its engine (changing effect is remove + add). Move's index is in the
// list after removing the slot. Removing a slot drops macro mappings to it.
struct AddNoteEffect {
  TrackId track;
  std::size_t index;
  std::string engine;
  std::uint32_t engine_version = 1;
  std::vector<NativeInstrumentParameter> parameters = {};
};
struct SetNoteEffectSettings {
  TrackId track;
  InsertSlotId id;
  std::vector<NativeInstrumentParameter> parameters;
};
struct MoveNoteEffect {
  TrackId track;
  InsertSlotId id;
  std::size_t index;
};
struct SetNoteEffectEnabled {
  TrackId track;
  InsertSlotId id;
  bool enabled;
};
struct RemoveNoteEffect {
  TrackId track;
  InsertSlotId id;
};
// Replaces a track's macros (at most max_macros_per_track; the index is the
// position). The session writes mapped parameter values alongside.
struct SetTrackMacros {
  TrackId track;
  std::vector<Macro> macros;
};
// Freezes (a render) or unfreezes (none) a track. The render's managed
// source must already be in the project, or be added in the same command.
struct SetTrackFrozen {
  TrackId track;
  std::optional<FrozenTrack> frozen;
};
struct AddAutomationLane {
  std::variant<TrackId, ClipId> owner;
  AutomationLane lane;
};
struct ReplaceAutomationLane {
  AutomationLaneId id;
  AutomationLane lane;
};
struct RemoveAutomationLane {
  AutomationLaneId id;
};
using Operation = std::variant<
    AddTrack, RemoveTrack, MoveTrack, SetTrackMuted, SetTrackSoloed, SetTrackLevelPan,
    SetMasterLevel, SetTrackMainOutput, AddSendRoute, RemoveSendRoute, SetSendGain, AddClip,
    RemoveClip, AddSource, RemoveSource, RelinkSource, SetClipPlacement, ApplySourceTempoAnalysis,
    SetSourceTempoCorrection, SetSourceDownbeat, SetSourceUserBeatFrames, ApplySourceOnsetAnalysis,
    SetSourceDismissedOnsets, ApplySourceKeyAnalysis, SetSourceKeyCorrection, SetClipSourceRange,
    SetClipFades, SetClipProcessing, SetClipClay, ReplaceTempoMap, ReplaceMeterMap, SetSampleRate,
    SetNativeInstrumentPatch, AddMidiClip, SetMidiClipPlacement, SetMidiPattern, AddMidiNote,
    RemoveMidiNote, SetMidiNote, SetMidiPedalLane, InsertVst3Plugin, RemoveVst3Plugin,
    MoveVst3Plugin, AddNativeEffect, SetNativeEffectSettings, MoveEffectInsert,
    SetEffectInsertEnabled, RemoveEffectInsert, SetVst3ComponentState, SetVst3ControllerState,
    SetVst3ParameterReferences, SetVst3Missing, RelinkVst3Plugin, AddAutomationLane,
    ReplaceAutomationLane, RemoveAutomationLane, SetTrackName, SetTrackColor, SetClipName,
    SetClipColor, AddMarker, MoveMarker, RenameMarker, RemoveMarker, SetProjectKey, SetLeadBars,
    AddNoteEffect, SetNoteEffectSettings, MoveNoteEffect, SetNoteEffectEnabled, RemoveNoteEffect,
    SetTrackMacros, SetTrackFrozen, SetMasterRebalance, SetTrackMixRole, SetTrackHeardRole>;

struct ChangeSet {
  std::vector<TrackId> tracks;
  std::vector<ClipId> clips;
  std::vector<SourceId> sources;
  std::vector<SourceId> source_tempo_reviews;
  std::vector<SourceId> source_key_reviews;
  std::vector<PluginInstanceId> plugins;
  std::vector<AutomationLaneId> automation_lanes;
  bool automation = false;
  bool timing = false;
  bool sample_rate = false;
  bool master_effects = false;
  bool routing = false;
  // The project key changed: engines that tune to it (Knock's pads) hear it.
  bool key = false;
  // Names, colours or markers changed; nothing audible did.
  bool presentation = false;
};

} // namespace oscillate::project
