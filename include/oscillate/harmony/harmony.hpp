#pragma once

#include "oscillate/project/model.hpp"
#include "oscillate/timeline/timing.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Harmony (ADR-0083, ADR-0084): the chord table, names, tension, voicing, the
// Progression Lab (mood, shape, colour, devices), next-chord candidates,
// substitutions and per-chord device hints. Pure functions of their
// arguments. Everything marked callback safe allocates nothing and may run on
// the audio callback.
namespace oscillate::harmony {

using project::MusicalKey;

// ---- chord values ----
// The qualities the chord table (intervals) knows.
enum class ChordQuality : std::uint8_t {
  major = 1,
  minor,
  diminished,
  sus2,
  add9,
  sixth,
  minor_sixth,
  major_seventh,
  minor_seventh,
  dominant_seventh,
  half_diminished,
  diminished_seventh,
  seventh_sus4,
  major_ninth,
  minor_ninth,
  dominant_ninth,
  dominant_seventh_flat_ninth,
  minor_eleventh,
  dominant_thirteenth,
  // Melodic house and trance colour: a minor triad with its ninth, and the
  // triad with its fourth for its third.
  minor_add9,
  sus4,
  // Root and fifth alone, no third: the open fifth these styles lean on.
  power
};
inline constexpr std::uint8_t last_chord_quality = static_cast<std::uint8_t>(ChordQuality::power);
struct ChordSymbol {
  std::uint8_t root = 0; // Pitch class, C = 0.
  ChordQuality quality = ChordQuality::major;
  // A bass pitch class other than the root (an inversion or a pedal).
  std::optional<std::uint8_t> bass = std::nullopt;
  bool operator==(const ChordSymbol&) const = default;
};
// Which Progression Lab device placed a region. Presentation only: the
// chord, bass and top already say what sounds.
enum class ChordDevice : std::uint8_t {
  none,
  pedal,
  drone,
  walk,
  suspension,
  passing,
  borrowed,
  pull_home,
  slide
};
inline constexpr std::uint8_t last_chord_device = static_cast<std::uint8_t>(ChordDevice::slide);
// A chord over a span of song (or clip) ticks. Chords live as notes in MIDI
// clips (ADR-0084); regions are the harmony library's working form of them.
struct ChordRegion {
  timeline::TickPosition position;
  std::int64_t length_ticks = 0;
  ChordSymbol chord;
  // A pitch class held on top of the voicing (a drone), if any.
  std::optional<std::uint8_t> top = std::nullopt;
  ChordDevice device = ChordDevice::none;
  bool operator==(const ChordRegion&) const = default;
};

inline constexpr std::size_t max_chord_tones = 6;
// Chord tones plus a drone note.
inline constexpr std::size_t max_voices = max_chord_tones + 1;
inline constexpr std::uint8_t pitch_classes = 12;

// ---- the chord table (callback safe) ----
struct Intervals {
  std::array<std::uint8_t, max_chord_tones> semitones{}; // From the root, ascending; may pass 12.
  std::uint8_t count = 0;
};
[[nodiscard]] Intervals intervals(ChordQuality quality) noexcept;
// Bit n set when pitch class n sounds (the bass and root included).
[[nodiscard]] std::uint16_t pitch_class_set(const ChordSymbol& chord) noexcept;
[[nodiscard]] bool is_dominant(ChordQuality quality) noexcept;
[[nodiscard]] bool is_minor(ChordQuality quality) noexcept; // Minor third, diminished included.

// ---- names and function (control) ----
[[nodiscard]] std::string_view suffix(ChordQuality quality) noexcept; // "m9", "maj7", "°7", "7♭9"
[[nodiscard]] std::string_view pitch_name(std::uint8_t pitch_class, const MusicalKey& key) noexcept;
[[nodiscard]] std::string name(const ChordSymbol& chord, const MusicalKey& key); // "Am9/G"
[[nodiscard]] std::string roman(const ChordSymbol& chord,
                                const MusicalKey& key); // "iv", "V⁷", "♭II"
enum class Role : std::uint8_t { home, lift, pull, colour };
[[nodiscard]] Role role(const ChordSymbol& chord, const MusicalKey& key) noexcept;
// 0 (at rest) to 100 (most pull), from the chord's function in the key, the
// notes outside it, dominant colour and a pedal under it.
[[nodiscard]] int tension(const ChordSymbol& chord, const MusicalKey& key) noexcept;

// ---- voicing (callback safe) ----
// Open is melodic house's voicing: over the bass, the fifth, the root an
// octave up and the third (or what stands for it) above, the colour on top,
// low and wide (ADR-0084).
enum class VoicingStyle : std::uint8_t { close, drop2, spread, rootless, open };
enum class TopLine : std::uint8_t { free, hold, fall, rise };
// Where the left hand plays the bass: its bass_note from E2.
inline constexpr std::uint8_t comping_bass_lowest = 40;
struct VoicingOptions {
  VoicingStyle style = VoicingStyle::drop2;
  TopLine top_line = TopLine::free;
  int register_offset = 0; // Semitones from the default centre.
  // The upper voices' range; the bass (bass_note) sounds below it. C3 to A5:
  // a keyboard player's right hand plus the left hand's upper notes.
  std::uint8_t lowest = 48;
  std::uint8_t highest = 81;
  // Where the bass sounds: its bass_note from here.
  std::uint8_t bass_lowest = comping_bass_lowest;
  // The left hand's bass under the chord. Off, nothing sounds in the bass's
  // register; the chord's own bass (its root, or an inversion's note) is its
  // lowest note instead, so the chord still reads as itself.
  bool bass = true;
  // Open with the bass off: the voicing it has over the bass, the bass's
  // octave its lowest voice (the records' usual 12-19-24-27 without the 0).
  bool bass_octave_lowest = false;
};
// The Open voicing in melodic house's register: the bass from C2 (the
// records' median bass is F2), the chord from B2 to C5 (MIDI 47 to 72).
inline constexpr std::uint8_t open_bass_lowest = 36;
inline constexpr std::uint8_t open_lowest = 47;
inline constexpr std::uint8_t open_highest = 72;
[[nodiscard]] constexpr VoicingOptions open_voicing() noexcept {
  return {.style = VoicingStyle::open,
          .lowest = open_lowest,
          .highest = open_highest,
          .bass_lowest = open_bass_lowest};
}
struct Voicing {
  std::array<std::uint8_t, max_voices> notes{}; // MIDI notes, ascending.
  std::uint8_t count = 0;
};
// The upper voices of `chord` (Open: see VoicingStyle; its add9 may rub
// its ninth a semitone under a minor third, Ben Böhmer's colour). Otherwise
// 3 or 4 notes, always its third and seventh
// (or what stands for them), the root left to the bass unless the chord is a
// triad or a sixth. No adjacent semitones, no minor ninths (except in a 7♭9),
// wide intervals low down, all within [lowest, highest]. `top` is a drone
// pitch class kept as the top voice. Of the voicings that qualify, the one
// that moves least from `previous` (none: nearest the centre), honouring the
// style and top line.
[[nodiscard]] Voicing voice(const ChordSymbol& chord, std::optional<std::uint8_t> top,
                            const VoicingOptions& options, const Voicing* previous) noexcept;
// Total semitones the voices move between two voicings.
[[nodiscard]] int movement(const Voicing& from, const Voicing& to) noexcept;
// The bass note for a chord: its bass (or root) pitch class in [lowest, lowest + 12).
[[nodiscard]] std::uint8_t bass_note(const ChordSymbol& chord, std::uint8_t lowest) noexcept;
// Control: each region voiced from the one before.
[[nodiscard]] std::vector<Voicing> voice_lane(std::span<const ChordRegion> lane,
                                              const VoicingOptions& options);

// ---- the Progression Lab (control) ----
enum class Mode : std::uint8_t { phrygian, aeolian, dorian, mixolydian, ionian, lydian };
inline constexpr std::size_t mode_count = 6;
enum class Shape : std::uint8_t {
  still,
  vamp,
  loop,
  circle,
  fall,
  climb,
  anthem,
  bittersweet,
  pendulum,
  shuffle,
  sway,
  hope,
  odd_loop,
  long_phrase,
  rise_and_fall
};
inline constexpr std::size_t shape_count = 15;
// The most chords a shape has (Long phrase, Rise and fall).
inline constexpr std::size_t max_shape_chords = 8;
// How each chord is coloured. Natural colours as the records do: mostly
// plain triads, one or two chords a loop coloured by their degree (VI's
// maj7, v's sus4, VII's sixth, iv's m7, i's sus2; ADR-0084). Then plain
// triads; triads with their ninth added (Anjunadeep); sus2 in place of the
// third (open, trance pads); sevenths; ninths and elevenths (neo-soul).
enum class Colour : std::uint8_t { natural, triads, add9, sus2, sevenths, lush };
enum class Device : std::uint8_t {
  pedal,
  drone,
  walk,
  suspension,
  passing,
  borrowed,
  pull_home,
  slide
};
inline constexpr std::size_t device_count = 8;
using DeviceSet = std::uint16_t; // Bit per Device.
[[nodiscard]] constexpr DeviceSet device_bit(Device device) noexcept {
  return static_cast<DeviceSet>(1U << static_cast<unsigned>(device));
}
struct MoodPoint {
  double brightness = 0.3;   // 0 dark (Phrygian) .. 1 bright (Lydian).
  double restlessness = 0.4; // 0 still .. 1 restless.
};
// The modes the pad crosses, darkest first: all six, or with a song key the
// three of its family (minor: Phrygian, Aeolian, Dorian; major: Mixolydian,
// Ionian, Lydian), so a minor song gets minor chords on its tonic.
[[nodiscard]] std::span<const Mode> pad_modes(std::optional<project::KeyMode> key) noexcept;
[[nodiscard]] Mode mode_at(double brightness,
                           std::optional<project::KeyMode> key = std::nullopt) noexcept;
// The brightness at the middle of the key's own mode (Aeolian or Ionian).
[[nodiscard]] double home_brightness(project::KeyMode key) noexcept;
[[nodiscard]] std::string_view mode_name(Mode mode) noexcept;
[[nodiscard]] std::string_view feel_at(MoodPoint mood) noexcept; // "Hypnotic", "Dreamy", ...
[[nodiscard]] std::string_view shape_name(Shape shape) noexcept;
[[nodiscard]] std::string_view shape_summary(Shape shape) noexcept;
// The shape's scale degrees (0-based), for the panel's bass-motion glyph.
[[nodiscard]] std::span<const std::uint8_t> shape_degrees(Shape shape) noexcept;
[[nodiscard]] std::string_view device_name(Device device) noexcept;
[[nodiscard]] std::string_view device_hint(Device device) noexcept;

struct LabOptions {
  std::uint8_t tonic = 9; // Pitch class; the mood picks the mode on it.
  // The song key's mode, when it has one: the mood stays in its family.
  std::optional<project::KeyMode> key_mode;
  MoodPoint mood;
  Shape shape = Shape::loop;
  Colour colour = Colour::natural;
  std::int64_t chord_ticks = 4 * timeline::ticks_per_quarter; // Pace; at least a 16th.
  DeviceSet devices = 0;
  std::uint64_t seed = 0; // "More" is the next seed.
};
enum class Variant : std::uint8_t { as_shape, swap, rotate, borrow };
inline constexpr std::size_t variant_count = 4;
struct Suggestion {
  Variant variant = Variant::as_shape;
  std::vector<ChordRegion> chords; // From tick 0, contiguous.
  std::string label;               // "Aeolian · swaps chord 3"
};
[[nodiscard]] std::array<Suggestion, variant_count> suggest(const LabOptions& options);

// Repeats `progression` (from tick 0) from `start` up to `end`, cutting the
// last region at `end`. Empty when that would take more than
// max_tiled_regions regions (each becomes at least one note in a clip).
inline constexpr std::size_t max_tiled_regions = project::max_midi_notes_per_clip;
[[nodiscard]] std::vector<ChordRegion> tile(std::span<const ChordRegion> progression,
                                            timeline::TickPosition start,
                                            timeline::TickPosition end);
// `lane` with every region in [start, end) replaced by `regions` (cut at the edges).
[[nodiscard]] std::vector<ChordRegion> replace_range(std::span<const ChordRegion> lane,
                                                     timeline::TickPosition start,
                                                     timeline::TickPosition end,
                                                     std::span<const ChordRegion> regions);

// ---- what comes next, substitutions, hints (control) ----
struct NextCandidate {
  ChordSymbol chord;
  double likelihood = 0.0; // 0..1, the best is 1.
  int tension = 0;
  int shared_tones = 0;
};
inline constexpr std::size_t next_candidate_limit = 8;
[[nodiscard]] std::vector<NextCandidate> next_candidates(const ChordSymbol& current,
                                                         const MusicalKey& key);

enum class EditKind : std::uint8_t {
  richer,
  simpler,
  borrowed,
  sus,
  tritone,
  lead_in_dominant,
  passing_chord,
  pedal_bass,
  release_pedal,
  top_drone,
  suspension,
  pull_home,
  inversion
};
struct LaneEdit {
  EditKind kind = EditKind::richer;
  std::string title;             // "Borrowed", "Pedal tone"
  std::string preview;           // "D9", "E7 → Am9"
  std::string why;               // Hints only: one sentence on why it suits this chord.
  std::vector<ChordRegion> lane; // The whole lane after the edit.
  std::size_t selected = 0;      // The region to select afterwards.
  bool already = false;          // Hints only: the device is already there.
};
[[nodiscard]] std::vector<LaneEdit> substitutions(std::span<const ChordRegion> lane,
                                                  std::size_t index, const MusicalKey& key);
inline constexpr std::size_t hint_limit = 4;
[[nodiscard]] std::vector<LaneEdit> hints(std::span<const ChordRegion> lane, std::size_t index,
                                          const MusicalKey& key);

} // namespace oscillate::harmony
