#pragma once

// Tracks, routing, macros, markers and the immutable project snapshot.

#include "oscillate/project/automation_types.hpp"
#include "oscillate/project/clip_types.hpp"
#include "oscillate/project/ids.hpp"
#include "oscillate/project/native_document.hpp"
#include "oscillate/project/plugin_types.hpp"
#include "oscillate/project/source_types.hpp"
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

enum class TrackKind : std::uint8_t { audio = 1, instrument = 2, group_bus = 3, return_bus = 4 };
// Whether a track of this kind can hold clips (audio clips, or MIDI clips
// and their notes); buses only process what reaches them.
[[nodiscard]] constexpr bool track_holds_clips(TrackKind kind) noexcept {
  switch (kind) {
  case TrackKind::audio:
  case TrackKind::instrument:
    return true;
  case TrackKind::group_bus:
  case TrackKind::return_bus:
    return false;
  }
  return false;
}
inline constexpr std::size_t max_sends_per_track = 32;
inline constexpr std::size_t max_sends_per_project = 2'048;
inline constexpr std::size_t max_routing_tracks = 4'096;

// Durable routing intent. The source is the owning track. Gain is linear;
// an absent main destination means master. Routing activation is separate.
enum class SendPurpose : std::uint8_t { audible = 1, sidechain = 2 };
struct SendRoute {
  SendId id;
  TrackId destination;
  double gain;
  SendPurpose purpose;
  // A sidechain names one built-in effect or VST3 effect slot on its
  // destination. A VST3 target still needs a verified auxiliary-bus receipt
  // before any live or offline graph can activate it.
  // Audible sends have no target slot.
  std::optional<InsertSlotId> target_insert;
  SendRoute(SendId route_id, TrackId target, double linear_gain = 1.0,
            SendPurpose route_purpose = SendPurpose::audible,
            std::optional<InsertSlotId> insert = std::nullopt)
      : id(route_id), destination(target), gain(linear_gain), purpose(route_purpose),
        target_insert(insert) {}
  bool operator==(const SendRoute&) const = default;
};

// Macro knobs (ADR-0059). A macro drives parameters on its own
// track: the built-in instrument's (NativeParameterTarget, naming the track),
// or a built-in effect's or note effect's (NativeEffectParameterTarget, whose
// slot is in the track's effects or note_effects). Turning a macro to v sets
// each mapped parameter to the blend from minimum to maximum at v in the
// domain's scale (minimum may exceed maximum: an inverted mapping), rounded
// to a step when the domain is discrete. The session writes those values in
// the same command as the macro's; the domain is the destination's automation
// domain, copied when the mapping is made. A parameter has at most one macro
// and, while mapped, no automation lane of its own: a lane on the macro
// drives it instead.
inline constexpr std::size_t max_macros_per_track = 8;
inline constexpr std::size_t max_macro_mappings = 16;
inline constexpr std::size_t max_macro_name_bytes = 64;
using MacroDestination = std::variant<NativeParameterTarget, NativeEffectParameterTarget>;
struct MacroMapping {
  MacroDestination destination;
  AutomationDomain domain;
  double minimum = 0.0;
  double maximum = 1.0;
  bool operator==(const MacroMapping&) const = default;
};
struct Macro {
  std::string name; // Empty: the app shows "Macro <n>".
  double value = 0.0;
  std::vector<MacroMapping> mappings = {};
  bool operator==(const Macro&) const = default;
};

// A frozen track (ADR-0059) plays a render of its instrument,
// note effects and inserts instead of running them: stereo, at the project
// rate, a managed WAV source whose frame 0 sounds at project frame
// first_frame. The fader, pan, mute, sends and routing still apply live.
// rendered_revision is the audible revision the render was made from. While
// frozen, commands that would change what was rendered (the instrument, note
// effects, inserts, clips, notes, macros, parameter automation, the tempo or
// sample rate) are refused: unfreeze first.
struct FrozenTrack {
  ManagedAudioReference audio;
  std::uint64_t first_frame = 0;
  std::uint64_t rendered_revision = 0;
  bool operator==(const FrozenTrack&) const = default;
};

struct Track {
  TrackId id;
  std::vector<Clip> clips;
  TrackKind kind = TrackKind::audio;
  InstrumentSource instrument_source;
  std::vector<MidiClip> midi_clips;
  std::vector<EffectInsert> effects;
  std::optional<TrackId> main_output;
  std::vector<SendRoute> sends;
  std::vector<AutomationLane> automation;
  bool muted = false;
  bool soloed = false;
  // Linear post-insert level and stereo balance. Zero pan is unity on both
  // channels; positive values attenuate left, negative values attenuate right.
  double level = 1.0;
  double pan = 0.0;
  // Presentation only: a user name (empty: the app numbers the
  // track) and an sRGB 0xRRGGBB colour (absent: the app's palette).
  std::string name;
  std::optional<std::uint32_t> color;
  // MIDI effects before the instrument (ADR-0059).
  std::vector<NoteEffectInsert> note_effects = {};
  std::vector<Macro> macros = {};
  std::optional<FrozenTrack> frozen = std::nullopt;
  Track(TrackId track_id, std::vector<Clip> audio_clips = {},
        TrackKind track_kind = TrackKind::audio, InstrumentSource source = {},
        std::vector<MidiClip> notes = {}, std::vector<EffectInsert> inserts = {},
        std::optional<TrackId> output = {}, std::vector<SendRoute> send_routes = {},
        std::vector<AutomationLane> lanes = {})
      : id(track_id), clips(std::move(audio_clips)), kind(track_kind),
        instrument_source(std::move(source)), midi_clips(std::move(notes)),
        effects(std::move(inserts)), main_output(output), sends(std::move(send_routes)),
        automation(std::move(lanes)) {}
  bool operator==(const Track&) const = default;
};

inline constexpr std::size_t max_track_name_bytes = 128;
inline constexpr std::uint32_t max_presentation_color = 0xFF'FF'FF;

// A named arrangement position. Markers are kept sorted by
// position, one per position; they never affect audio.
struct Marker {
  MarkerId id;
  timeline::TickPosition position;
  std::string name;
  bool operator==(const Marker&) const = default;
};
inline constexpr std::size_t max_markers_per_project = 1'024;
inline constexpr std::uint32_t max_lead_bars = 1'024;
inline constexpr std::size_t max_marker_name_bytes = 128;

// This is durable project intent only. Audible graph status belongs to application.
struct Snapshot {
  ProjectId id;
  std::uint64_t revision;
  std::uint32_t sample_rate;
  timeline::TempoMap tempo;
  timeline::MeterMap meter;
  std::vector<Track> tracks;
  std::vector<SourceRecord> sources = {};
  std::vector<EffectInsert> master_effects = {};
  std::vector<SourceTempoReview> source_tempo_reviews = {};
  // Monotonic allocation high-water mark for command-created slots. Undo and
  // restore never lower it, so a retired runtime identity cannot be reused.
  std::uint64_t next_insert_slot_serial = 1;
  std::vector<SourceKeyReview> source_key_reviews = {};
  // Linear gain after the master insert chain; unity by default.
  double master_level = 1.0;
  std::vector<Marker> markers = {};
  // The song's key, set by the musician; none until chosen.
  // Presentation and editing aid only: it never changes audio.
  std::optional<MusicalKey> key = std::nullopt;
  // Bars before bar 1 (an intro that starts before the song's
  // first downbeat). The timeline still starts at zero; the ruler counts
  // these bars as 1 - lead_bars .. 0. Presentation only.
  std::uint32_t lead_bars = 0;
  // The revision of the last change that could be heard. Names, colours
  // and markers carry it forward, so playback built at or after it
  // is current. Derived, not saved: a loaded project starts at its revision.
  std::uint64_t audible_revision = 0;
};

// True for the channel keys the routed mixer evaluates.
[[nodiscard]] constexpr bool is_channel_control(TrackControlTarget::Key key) noexcept {
  return key == TrackControlTarget::Key::channel_level ||
         key == TrackControlTarget::Key::channel_pan ||
         key == TrackControlTarget::Key::channel_mute;
}

// The automation domain each channel key must carry.
struct ChannelControlDomain {
  const char* semantic_key;
  const char* unit;
  double minimum;
  double maximum;
  bool discrete;
  std::uint32_t step_count;
  AutomationScale scale;
};
[[nodiscard]] constexpr ChannelControlDomain
channel_control_domain(TrackControlTarget::Key key) noexcept {
  switch (key) {
  case TrackControlTarget::Key::channel_pan:
    return {"mixer.channel_pan", "balance", -1.0, 1.0, false, 0, AutomationScale::linear};
  case TrackControlTarget::Key::channel_mute:
    return {"mixer.channel_mute", "switch", 0.0, 1.0, true, 1, AutomationScale::linear};
  case TrackControlTarget::Key::channel_level:
    break;
  }
  return {"mixer.channel_level", "linear_gain", 0.0, 4.0, false, 0, AutomationScale::decibel};
}

// The automation domain every macro lane carries (ADR-0059), with the
// macro's current value as its default.
[[nodiscard]] inline AutomationDomain macro_automation_domain(double value) {
  return {AutomationRepresentation::physical,
          "macro",
          "normalized",
          0.0,
          1.0,
          value,
          false,
          0,
          AutomationScale::linear};
}

// Direct-to-master consumers must reject this saved routing intent until they
// compile a routed graph. An absent main output means direct master output.
[[nodiscard]] bool requires_routed_rendering(const Snapshot& snapshot) noexcept;

// Consumers without a prepared automation evaluator must reject this durable
// intent before rendering, including lanes owned by clips.
[[nodiscard]] bool requires_automation_rendering(const Snapshot& snapshot) noexcept;

// Until a convolution runtime is bound in every render path, consumers must
// explicitly reject a saved convolution insert even when its slot is bypassed.
[[nodiscard]] bool has_convolution_insert(const Snapshot& snapshot) noexcept;

// The heap and inline bytes a snapshot holds, estimated from its element
// counts, strings and plugin state blobs. Undo history retention budgets with
// it; it is not an allocator measurement.
[[nodiscard]] std::size_t estimated_bytes(const Snapshot& snapshot) noexcept;

} // namespace oscillate::project
