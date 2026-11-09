#pragma once

// Built-in instrument, effect and note-effect state and the structured synth document.

#include "oscillate/project/automation_types.hpp"
#include "oscillate/project/ids.hpp"
#include "oscillate/project/plugin_types.hpp"
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

inline constexpr std::size_t max_native_instrument_parameters = 2'048;
inline constexpr std::size_t max_native_identifier_length = 96;

struct NativeInstrumentParameter {
  std::string id;
  double value = 0.0;
  bool operator==(const NativeInstrumentParameter&) const = default;
};

// The encoded audio remains a managed source. Repeating its immutable identity
// here prevents a relink or source-record edit from silently changing what a
// synth's or an effect's media slot plays.
struct ManagedAudioReference {
  SourceId source;
  std::string sha256;
  std::uint64_t encoded_bytes;
  std::uint32_t sample_rate;
  std::uint32_t channels;
  std::uint64_t frames;
  bool operator==(const ManagedAudioReference&) const = default;
};

// Structured synth state (ADR-0057): what a flat parameter list
// cannot hold. The project validates structure, limits and media identity;
// the engine validates meaning (which sources, destinations, modules and
// media slots exist) when a command is built or a source is prepared.
// Continuous amounts stay flat parameters so they remain automatable.
inline constexpr std::size_t max_native_synth_routes = 256;
inline constexpr std::size_t max_native_synth_curves = 64;
inline constexpr std::size_t max_native_curve_points = 64;
inline constexpr std::size_t max_native_synth_chains = 4;
inline constexpr std::size_t max_native_chain_slots = 8;
inline constexpr std::size_t max_native_synth_sequences = 8;
inline constexpr std::size_t max_native_sequence_notes = 512;
inline constexpr std::size_t max_native_synth_media = 32;
inline constexpr std::size_t max_native_synth_labels = 32;
inline constexpr std::size_t max_native_label_length = 64;
// A media slot plays at most this much decoded audio (32 MiB of stereo
// float frames).
inline constexpr std::uint64_t max_native_synth_media_frames = 4'194'304;
inline constexpr std::size_t max_native_regions = 128;    // per document
inline constexpr std::size_t max_native_region_name = 64; // UTF-8 bytes
inline constexpr std::size_t max_native_region_tag = 16;
// Per-voice hand edits (ADR-0073): lanes per document, values per lane.
inline constexpr std::size_t max_native_voice_lanes = 64;
inline constexpr std::size_t max_native_voice_lane_values = 64;

// One modulation route. Its amount is the engine's flat parameter for `slot`,
// so automation and the panel reach it like any knob. `aux` (empty: none)
// scales the source; `tension` bends the source's response, -1..1; `scale`
// (0..1) is the route's fixed output level.
struct NativeModRoute {
  std::uint32_t slot = 0;
  std::string source;
  std::string aux;
  std::string destination;
  bool bipolar = false;
  double tension = 0.0;
  double scale = 1.0;
  bool operator==(const NativeModRoute&) const = default;
};

// A curve segment from this point to the next: bent by tension (-1..1), or
// held at this point's value.
enum class NativeCurveSegment : std::uint8_t { curve, step };
struct NativeCurvePoint {
  double x = 0.0; // 0..1, strictly increasing; the first is 0, the last 1.
  double y = 0.0; // 0..1.
  double tension = 0.0;
  NativeCurveSegment segment = NativeCurveSegment::curve;
  bool operator==(const NativeCurvePoint&) const = default;
};
// A drawn shape: an LFO cycle, an envelope segment, a matrix response curve.
struct NativeCurve {
  std::string id;
  std::vector<NativeCurvePoint> points;
  bool operator==(const NativeCurve&) const = default;
};

// An ordered list of modules, such as an effect bus. `uid` names the slot's
// flat parameters, so reordering moves a module with its settings and its
// automation; uids are unique across every chain of one document.
struct NativeChainSlot {
  std::uint32_t uid = 0;
  std::string module;
  bool operator==(const NativeChainSlot&) const = default;
};
struct NativeChain {
  std::string id;
  std::vector<NativeChainSlot> modules; // Not "slots": Qt defines that as a macro.
  bool operator==(const NativeChain&) const = default;
};

// Notes the synth plays itself (a clip sequencer clip), in ticks from the
// sequence start; the sequence repeats every `length` ticks.
struct NativeSequenceNote {
  timeline::TickPosition start;
  timeline::TickPosition length;
  std::uint8_t key = 60;
  std::uint8_t velocity = 100;
  bool operator==(const NativeSequenceNote&) const = default;
};
struct NativeSequence {
  std::string id;
  timeline::TickPosition length;
  std::vector<NativeSequenceNote> notes;
  bool operator==(const NativeSequence&) const = default;
};

// Content an engine generates or ships itself, by its stable name.
struct NativeBuiltinMedia {
  std::string name;
  bool operator==(const NativeBuiltinMedia&) const = default;
};
// Audio a media slot plays: a wavetable (frames of `frame_length` samples),
// a sample or a noise. frame_length 0 means the audio is not cut into frames.
struct NativeMediaSlot {
  std::string id;
  std::variant<NativeBuiltinMedia, ManagedAudioReference> media;
  std::uint32_t frame_length = 0;
  bool operator==(const NativeMediaSlot&) const = default;
};

// A user-visible name the engine shows, such as a macro's.
struct NativeLabel {
  std::string id;
  std::string text;
  bool operator==(const NativeLabel&) const = default;
};

// A region of a media slot (ADR-0063): it runs from `start_frame` to the
// next region's start in the same media, or to the media's end. `slot` is a
// stable id, unique in the document, that keys the engine's per-region
// parameters. The model checks structure; the engine checks meaning.
struct NativeRegion {
  std::uint32_t slot = 0;        // stable id, unique in the document
  std::string media;             // a NativeMediaSlot id in this document
  std::uint64_t start_frame = 0; // in the media's source frames
  std::string tag;               // engine vocabulary; Crate: kick|snare|hat|other
  std::string name;              // shown on the pad; may be empty
  bool operator==(const NativeRegion&) const = default;
};

// One voice's hand-set value in a voice lane (ADR-0073): which voice (an
// engine's voice number) and its value, -1..1.
struct NativeVoiceValue {
  std::uint32_t voice = 0;
  double value = 0.0;
  bool operator==(const NativeVoiceValue&) const = default;
};
// Values a user painted for some of an engine's voices, keyed by an id in the
// engine's vocabulary (Nova: "notes.pitch", "unison.a.pan"). Voices left out
// keep what the engine gives them. Voices strictly increase.
struct NativeVoiceLane {
  std::string id;
  std::vector<NativeVoiceValue> values;
  bool operator==(const NativeVoiceLane&) const = default;
};

struct NativeSynthDocument {
  std::vector<NativeModRoute> routes;
  std::vector<NativeCurve> curves;
  std::vector<NativeChain> chains;
  std::vector<NativeSequence> sequences;
  std::vector<NativeMediaSlot> media;
  std::vector<NativeLabel> labels;
  std::vector<NativeRegion> regions;
  std::vector<NativeVoiceLane> voice_lanes;
  [[nodiscard]] bool empty() const noexcept {
    return routes.empty() && curves.empty() && chains.empty() && sequences.empty() &&
           media.empty() && labels.empty() && regions.empty() && voice_lanes.empty();
  }
  bool operator==(const NativeSynthDocument&) const = default;
};

// Saved state of a built-in synth engine (for example "prism"), independent of
// any one engine's layout: the engine's stable parameter ids with plain values,
// storing only values that differ from that engine version's defaults, and the
// engine's structured document (empty for engines without one). The project
// validates structure only; the engine validates ids and ranges when a command
// is built or a source is prepared, so new engines need no schema.
struct NativeInstrumentPatch {
  std::string engine;
  std::uint32_t engine_version = 1;
  std::vector<NativeInstrumentParameter> parameters;
  NativeSynthDocument document = {};
  bool operator==(const NativeInstrumentPatch&) const = default;
};

// Durable processing intent. An instrument generates audio before its owning
// path's ordered audio-effect slots.
using InstrumentSource = std::variant<std::monostate, Vst3PluginInstance, NativeInstrumentPatch>;

// A built-in effect (ADR-0040): the engine's stable parameter ids with plain
// values, storing only values that differ from that engine version's
// defaults, and its structured document (ADR-0058; drawn curves and, for an
// engine that plays media such as Cast's impulse response, media slots;
// empty for most effects). The project validates structure only; the
// engine resolves ids, ranges and the document when a command is built or a
// stage is prepared, so new effects need no schema. Effects of one engine
// sharing a non-empty link (a native identifier) share their document: the
// session edits the group's documents together; values stay per instance.
struct NativeEffectPatch {
  std::string engine;
  std::uint32_t engine_version = 1;
  std::vector<NativeInstrumentParameter> parameters = {};
  std::uint64_t generation = 1;
  NativeSynthDocument document = {};
  std::string link = {};
  bool operator==(const NativeEffectPatch&) const = default;
};
using EffectProcessor = std::variant<Vst3PluginInstance, NativeEffectPatch>;
struct EffectInsert {
  InsertSlotId id;
  bool enabled = true;
  EffectProcessor processor;
  bool operator==(const EffectInsert&) const = default;
};

// A MIDI effect before a track's instrument (ADR-0059): an
// arpeggiator, chord, note echo, randomiser or scale. Like a built-in effect
// it saves the engine's stable parameter ids with plain values, storing only
// non-defaults; the project validates structure only and the engine resolves
// the rest. Slot ids share the effect slots' allocator and namespace, so an
// id names one note effect or one effect insert across the project.
inline constexpr std::size_t max_note_effects_per_track = 8;
struct NoteEffectInsert {
  InsertSlotId id;
  bool enabled = true;
  std::string engine;
  std::uint32_t engine_version = 1;
  std::vector<NativeInstrumentParameter> parameters = {};
  bool operator==(const NoteEffectInsert&) const = default;
};

} // namespace oscillate::project
