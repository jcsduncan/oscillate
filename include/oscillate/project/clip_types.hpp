#pragma once

// Audio and MIDI clips: placement, fades, warp, Clay, notes and pedals.

#include "oscillate/project/automation_types.hpp"
#include "oscillate/project/ids.hpp"
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

struct MusicalInterval {
  timeline::TickPosition start;
  timeline::TickPosition end;
  bool operator==(const MusicalInterval&) const = default;
};
struct ElapsedInterval {
  timeline::SamplePosition start;
  timeline::SamplePosition end;
  bool operator==(const ElapsedInterval&) const = default;
};
using Placement = std::variant<MusicalInterval, ElapsedInterval>;

struct SourceFramePosition {
  std::int64_t value;
  auto operator<=>(const SourceFramePosition&) const = default;
};
struct SourceRange {
  SourceFramePosition start;
  SourceFramePosition end;
  bool operator==(const SourceRange&) const = default;
};

enum class FadeCurve : std::uint8_t { linear = 1, smoothstep = 2 };
struct ClipFades {
  double in_fraction = 0.0;
  double out_fraction = 0.0;
  FadeCurve in_curve = FadeCurve::linear;
  FadeCurve out_curve = FadeCurve::linear;
  bool operator==(const ClipFades&) const = default;
};

// Reviewed per-instance processing intent. The two clip endpoints are implicit.
// Source frames are absolute in the immutable source; target offsets are local
// to the clip and use its placement timebase.
inline constexpr std::size_t max_clip_warp_markers = 4'094;
struct ClipWarpMarker {
  SourceFramePosition source_frame;
  std::variant<timeline::TickPosition, timeline::SamplePosition> target_offset;
  bool operator==(const ClipWarpMarker&) const = default;
};
inline constexpr double max_clip_pitch_semitones = 48.0;

// Stretch mode and remembered per-mode parameters (ADR-0026
// point 4). The mode is always explicit; Re-Pitch is never a default.
enum class WarpMode : std::uint8_t { repitch, beats, tones, texture, complex, complex_pro };
enum class WarpBeatsLoop : std::uint8_t { off, forward, back_and_forth };
struct ClipWarpBeats {
  WarpBeatsLoop loop = WarpBeatsLoop::forward;
  double envelope = 100.0; // 0..100
  bool operator==(const ClipWarpBeats&) const = default;
};
struct ClipWarpTones {
  double grain_ms = 40.0; // 5..200
  bool operator==(const ClipWarpTones&) const = default;
};
struct ClipWarpTexture {
  double grain_ms = 100.0; // 10..500
  double flux = 0.0;       // 0..100
  bool operator==(const ClipWarpTexture&) const = default;
};
struct ClipWarpComplexPro {
  double formants = 100.0; // 0..100, applied only with preserve_formants
  double envelope = 128.0; // 8..256
  bool operator==(const ClipWarpComplexPro&) const = default;
};
struct ClipWarpSettings {
  WarpMode mode = WarpMode::complex;
  bool mode_overridden = false;
  ClipWarpBeats beats;
  ClipWarpTones tones;
  ClipWarpTexture texture;
  ClipWarpComplexPro complex_pro;
  bool operator==(const ClipWarpSettings&) const = default;
};

struct ClipProcessing {
  double pitch_semitones = 0.0; // -48..48; inactive in Re-Pitch
  bool preserve_formants = true;
  std::vector<ClipWarpMarker> reviewed_warp_markers;
  ClipWarpSettings warp;
  // True when pitch or markers need the warp renderer. A placement whose
  // duration differs from its rate-converted source also needs it; that is
  // decided by the clip frame plan, not here.
  [[nodiscard]] bool requires_rendering() const noexcept {
    return (pitch_semitones != 0.0 && warp.mode != WarpMode::repitch) ||
           !reviewed_warp_markers.empty();
  }
  bool operator==(const ClipProcessing&) const = default;
};

// Clay note edits on an audio clip (ADR-0081). The analysis
// is not saved: it is recomputed deterministically from the clip's source
// (its content hash), the mode and analysis_version, which the saved note
// ids refer to. Split and join replay in order on the analysed take before
// the edits apply, so the ids they mint are stable. Frames are source
// frames at the source rate: Clay edits come before warp.
enum class ClayMode : std::uint8_t { melodic = 1, polyphonic = 2, percussive = 3 };
enum class ClayOpKind : std::uint8_t { split = 1, join = 2 };
struct ClayStructuralOp {
  ClayOpKind kind = ClayOpKind::split;
  std::uint32_t note = 0;
  // join: the note joined into `note`.
  std::uint32_t other = 0;
  // split: the source frame strictly inside `note` where it splits.
  std::int64_t frame = 0;
  bool operator==(const ClayStructuralOp&) const = default;
};
// One note's edit; defaults change nothing. A duplicate is an added voice
// (a harmony) made from `note`'s model; `voice` tells duplicates of one note
// apart (0 for the note's own edit).
struct ClayNoteEdit {
  std::uint32_t note = 0;
  std::uint32_t voice = 0;
  double pitch_cents = 0.0;
  double modulation = 1.0;
  double drift = 1.0;
  double formant_cents = 0.0;
  double gain_db = 0.0;
  bool muted = false;
  std::optional<std::int64_t> start{};
  std::optional<std::int64_t> end{};
  bool operator==(const ClayNoteEdit&) const = default;
};
inline constexpr std::size_t max_clay_edits = 4'096;
inline constexpr std::size_t max_clay_ops = 4'096;
inline constexpr std::uint32_t max_clay_voices = 8;
struct ClayTake {
  ClayMode mode = ClayMode::melodic;
  std::uint32_t analysis_version = 0;
  std::vector<ClayStructuralOp> ops;
  std::vector<ClayNoteEdit> edits;
  // Snapping and the key macros work in; absent is chromatic.
  std::optional<MusicalKey> key;
  bool operator==(const ClayTake&) const = default;
};

struct Clip {
  ClipId id;
  SourceId source;
  SourceRange source_range;
  Placement placement;
  std::uint64_t settings_generation = 1;
  std::vector<AutomationLane> automation;
  ClipFades fades;
  ClipProcessing processing;
  // Presentation only: an sRGB 0xRRGGBB override of the
  // track's colour; absent follows the track.
  std::optional<std::uint32_t> color;
  // Presentation only: what the clip shows, usually the song's
  // name from its file; empty shows the track's name.
  std::string name;
  // Clay note edits (ADR-0081); absent plays the source as is.
  std::optional<ClayTake> clay = std::nullopt;
  Clip(ClipId clip_id, SourceId source_id, SourceRange range, Placement placed,
       std::uint64_t generation = 1, std::vector<AutomationLane> lanes = {}, ClipFades fade = {},
       ClipProcessing processed = {})
      : id(clip_id), source(source_id), source_range(range), placement(placed),
        settings_generation(generation), automation(std::move(lanes)), fades(fade),
        processing(std::move(processed)) {}
  bool operator==(const Clip&) const = default;
};

inline constexpr std::size_t max_midi_notes_per_clip = 4'096;
inline constexpr std::size_t max_midi_events_per_project = 100'000;

struct MidiNote {
  NoteId id;
  timeline::TickPosition start;
  timeline::TickPosition end;
  std::uint8_t key;
  std::uint8_t velocity;
  bool operator==(const MidiNote&) const = default;
};

// Pedals travel with a clip's notes because they change what a note-off
// means (ADR-0035): sustain (CC64, half-pedal values allowed), sostenuto
// (CC66) and soft/una corda (CC67). A lane holds each point's value, 0..1,
// until the next point; points repeat with the pattern when it loops.
enum class MidiPedal : std::uint8_t { sustain, sostenuto, soft };
inline constexpr std::size_t midi_pedal_count = 3;
inline constexpr std::size_t max_midi_pedal_points_per_lane = 4'096;

struct MidiPedalPoint {
  timeline::TickPosition position;
  double value = 0.0;
  bool operator==(const MidiPedalPoint&) const = default;
};

struct MidiPedalLane {
  MidiPedal pedal = MidiPedal::sustain;
  std::vector<MidiPedalPoint> points;
  bool operator==(const MidiPedalLane&) const = default;
};

// A MIDI clip plays the first `pattern_length` ticks of its notes and pedal
// points, repeated when `loop` is on, until the placement ends. Notes and
// points at or after the pattern's end are kept but silent (a note crossing
// it is cut there), so shortening a clip and lengthening it again restores
// them, as in Live. Likewise, notes and points before tick 0 (a clip
// trimmed from its left edge, ADR-0096) are kept but silent; the last pedal
// point before tick 0 holds its value from the pattern's start.
struct MidiClip {
  ClipId id;
  Placement placement;
  timeline::TickPosition pattern_length;
  bool loop = false;
  std::vector<MidiNote> notes;
  std::uint64_t settings_generation = 1;
  std::vector<AutomationLane> automation;
  std::vector<MidiPedalLane> pedals; // At most one lane per pedal.
  // Presentation only, as for audio clips.
  std::optional<std::uint32_t> color;
  MidiClip(ClipId clip_id, Placement placed, timeline::TickPosition length, bool repeated,
           std::vector<MidiNote> clip_notes, std::uint64_t generation = 1,
           std::vector<AutomationLane> lanes = {}, std::vector<MidiPedalLane> pedal_lanes = {})
      : id(clip_id), placement(placed), pattern_length(length), loop(repeated),
        notes(std::move(clip_notes)), settings_generation(generation), automation(std::move(lanes)),
        pedals(std::move(pedal_lanes)) {}
  bool operator==(const MidiClip&) const = default;
};

// Whether a note plays: it starts inside the pattern, not before tick 0
// (trimmed off the left edge) or at or after the pattern's end.
[[nodiscard]] constexpr bool note_plays(const MidiClip& clip, const MidiNote& note) noexcept {
  return note.start.value >= 0 && note.start < clip.pattern_length;
}

} // namespace oscillate::project
