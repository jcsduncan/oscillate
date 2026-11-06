#pragma once

#include "oscillate/harmony/harmony.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/timeline/timing.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Chords as notes (ADR-0084): a MIDI clip's chords are its notes. The piano
// roll's chord strip reads them with recognise(), and a chord edit rewrites
// the notes in their rhythm with rewrite(). write() lays a lane down as held
// chords. Control-thread functions: they allocate, and stay near-linear in
// the clip's notes up to project::max_midi_notes_per_clip.
namespace oscillate::harmony {

// The grid recognition reads chords on: a 16th. Note edges round to it.
inline constexpr std::int64_t recognition_step_ticks = timeline::ticks_per_quarter / 4;
// A recognised region lasts at least this many steps; shorter readings (a
// passing note) are absorbed into a neighbour.
inline constexpr std::int64_t min_region_steps = 2;
// Notes below the bass's octave (VoicingOptions::bass_lowest + 12) are the
// left hand's bass; notes from it up are the chord. Voicings written or
// re-voiced here keep the chord at or above it (VoicingOptions::lowest is
// raised to it). With the default options that is E3:
inline constexpr std::uint8_t bass_role_below = comping_bass_lowest + 12;
// The same split for a voicing's own bass register.
[[nodiscard]] constexpr std::uint8_t bass_split(const VoicingOptions& options) noexcept {
  return static_cast<std::uint8_t>(options.bass_lowest + pitch_classes);
}
// Held chords' velocity.
inline constexpr std::uint8_t written_velocity = 96;

// The id rewrite() and write() give a note they create; the session assigns
// fresh ids. (The all-zero id is reserved, so this is the all-ones id.)
[[nodiscard]] project::NoteId unassigned_note_id();

// The chord a set of sounding notes makes: the best template match over all
// roots and qualities (all chord tones present scores highest; a missing
// fifth costs less than another missing tone, a missing third more; tones
// outside the chord cost most), bass = the lowest note's pitch class (a slash
// chord when it is not the root; the bass never counts as a tone outside the
// chord). Ties go to the earlier root and quality. nullopt for fewer than 2
// distinct pitch classes (a bass-only step).
[[nodiscard]] std::optional<ChordSymbol> identify(std::span<const std::uint8_t> keys);

// The chords a clip's notes make, as contiguous regions over [0, length) on
// the recognition grid: per-step recognition (a step with fewer than three
// pitch classes that fit the chord before inherits it, as do silence and a
// bass-only step; a lone bass struck ahead of the chord it is the bass of,
// on the beat before an off-beat stab, joins that chord), equal neighbours
// merged, regions shorter than min_region_steps absorbed into a neighbour,
// then each region re-identified from the notes sounding in it (a note sounding less than a quarter
// as long as the region's longest is a passing note and left out). `top` is the highest note's
// pitch class when it is held through the region, is not a chord tone, and the rest make a complete
// chord without it (a drone). Empty when nothing is recognised. Regions carry ChordDevice::none.
[[nodiscard]] std::vector<ChordRegion> recognise(std::span<const project::MidiNote> notes,
                                                 timeline::TickPosition length);

// The notes with the chords of `lane` (sorted, non-overlapping regions in clip
// ticks). A region whose notes already read as its chord (as recognise reads
// them, silence inheriting the chord before) is kept, unless it is the lane
// region `revoice` names: that one is re-voiced afresh in the options' style
// (not led from the voicing before), for the Voicing control. Otherwise its
// notes are re-voiced in their rhythm: notes held across the region's edges
// are cut there (the piece inside continues as the new chord);
// bass-role notes become the new bass in their octave; upper notes, grouped
// by onset, map onto the new voicing (voice() led from the previous region's
// voicing; a slash chord also sounds its root) by their rank in the region's
// fullest group, top to top, and voicing notes between two neighbouring mapped
// voices join at the group's onset and length. A region with no notes gets
// the chord written as held notes. Notes outside every region keep their
// pitch, timing and id, except that, like every note, they are cropped to
// [0, length) (dropped when nothing is left) and of notes sharing a start and
// key only the first is kept. Kept and re-pitched notes keep their ids;
// created notes and the later pieces of cut notes carry unassigned_note_id().
// The result is sorted by start then key, has no two notes on one start and
// key, and lies inside [0, length).
[[nodiscard]] std::vector<project::MidiNote>
rewrite(std::span<const project::MidiNote> notes, std::span<const ChordRegion> lane,
        timeline::TickPosition length, const VoicingOptions& options,
        std::optional<std::size_t> revoice = std::nullopt);

// Held chords: each region's bass (bass_note from comping_bass_lowest) and
// voicing (top = drone; a slash chord also sounds its root) for its whole
// length, each voiced from the one before. Ids are unassigned_note_id().
[[nodiscard]] std::vector<project::MidiNote> write(std::span<const ChordRegion> lane,
                                                   const VoicingOptions& options,
                                                   std::uint8_t velocity = written_velocity);

// ---- Held rhythm (ADR-0084): how melodic house plays chords ----
// In PML's chord pack 49 of 52 parts never re-strike a chord: they hold it,
// and about one change in five lands early. Anticipation moves each change
// (after the first) earlier by a 16th, an 8th or a dotted 8th, or varies it
// change by change.
enum class Anticipation : std::uint8_t { none, sixteenth, eighth, dotted_eighth, varied };
// Varied cycles these, in 16ths: a dotted 8th, an 8th, a 16th, an 8th.
inline constexpr std::array<std::int64_t, 4> varied_anticipation{3, 2, 1, 2};
[[nodiscard]] std::int64_t anticipation_sixteenths(Anticipation anticipation,
                                                   std::size_t change) noexcept;
// The lane with each change moved earlier, the chord before it shortened.
[[nodiscard]] std::vector<ChordRegion> anticipate(std::span<const ChordRegion> lane,
                                                  Anticipation anticipation);
// Push and land: held chords, and before each change the next chord touched
// once, short and softer, then held from the change. The touch comes a
// dotted 8th early unless the anticipation sets it (at least an 8th).
inline constexpr std::int64_t touch_sixteenths = 2;
inline constexpr std::uint8_t touch_velocity = 78;
[[nodiscard]] std::vector<project::MidiNote>
push_and_land(std::span<const ChordRegion> lane, const VoicingOptions& options,
              Anticipation anticipation, std::uint8_t velocity = written_velocity);

// ---- Layers (ADR-0084): parts on their own tracks that follow the chords ----

// A pad: the chords held in the Open voicing an octave up, without the bass.
inline constexpr int pad_octave_up = 12;
[[nodiscard]] std::vector<project::MidiNote> pad_layer(std::span<const ChordRegion> lane,
                                                       std::uint8_t velocity = written_velocity);
// The chords held in a close voicing high up, for a pluck that a Chord Comp
// on its track rhythmizes (the rhythmic supporter).
inline constexpr std::uint8_t pluck_lowest = 60;
inline constexpr std::uint8_t pluck_highest = 84;
[[nodiscard]] std::vector<project::MidiNote> pluck_layer(std::span<const ChordRegion> lane,
                                                         std::uint8_t velocity = written_velocity);

// A running tonic: the key's tonic pulsing through every chord, a pedal the
// changes move under; each beat's first pulse accented.
enum class Pulse : std::uint8_t { eighths, sixteenths, dotted_eighths };
inline constexpr std::array<std::int64_t, 3> pulse_sixteenths{2, 1, 3};
inline constexpr std::uint8_t tonic_lowest = 60;
inline constexpr std::uint8_t accent_velocity = 100;
inline constexpr std::uint8_t pulse_velocity = 76;
[[nodiscard]] std::vector<project::MidiNote> running_tonic(timeline::TickPosition length,
                                                           std::uint8_t tonic, Pulse pulse,
                                                           std::uint8_t lowest = tonic_lowest);

// A bass line under the chords' basses: held, on the off-beat 8ths (house),
// or rolling 16ths that leave each beat to the kick (trance).
enum class BassStyle : std::uint8_t { held, offbeat, rolling };
[[nodiscard]] std::vector<project::MidiNote> bass_line(std::span<const ChordRegion> lane,
                                                       BassStyle style,
                                                       std::uint8_t lowest = open_bass_lowest,
                                                       std::uint8_t velocity = written_velocity);

} // namespace oscillate::harmony
