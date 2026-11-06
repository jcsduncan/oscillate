#pragma once

#include "oscillate/foundation/error.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// A strict, bounded Standard MIDI File reader (ADR-0045). Files are untrusted
// input: every length, count and tick is bounded, malformed input fails with
// a message and never reads out of range. Formats 0 and 1 are read; format 2
// (independent sequences) is refused. Control thread only.
namespace oscillate::midi_file {

inline constexpr std::size_t max_file_bytes = 32U * 1024U * 1024U;
inline constexpr std::size_t max_tracks = 512;
inline constexpr std::size_t max_events = 2'000'000;
inline constexpr std::size_t max_track_name_bytes = 256;
// About 2^40 file ticks: decades at any common division, and far from the
// project tick range after rescaling.
inline constexpr std::uint64_t max_tick = std::uint64_t{1} << 40U;

// Ticks per quarter note, or SMPTE frames per second and ticks per frame.
// SMPTE rates are 24, 25, 29 (29.97 drop-frame) and 30.
struct Division {
  bool smpte = false;
  std::uint16_t ticks_per_quarter = 0;
  std::uint8_t frames_per_second = 0;
  std::uint8_t ticks_per_frame = 0;
};

// A paired note in absolute file ticks; end > start always holds. Notes on
// the same channel and key pair first-in, first-out; a note-on with
// velocity 0 is a note-off.
struct Note {
  std::uint64_t start = 0;
  std::uint64_t end = 0;
  std::uint8_t channel = 0;
  std::uint8_t key = 0;
  std::uint8_t velocity = 0; // 1..127
};

struct Controller {
  std::uint64_t tick = 0;
  std::uint8_t channel = 0;
  std::uint8_t number = 0;
  std::uint8_t value = 0;
};

struct ProgramChange {
  std::uint64_t tick = 0;
  std::uint8_t channel = 0;
  std::uint8_t program = 0;
};

struct TempoChange {
  std::uint64_t tick = 0;
  std::uint32_t microseconds_per_quarter = 500'000;
};

struct TimeSignature {
  std::uint64_t tick = 0;
  std::uint8_t numerator = 4;
  std::uint16_t denominator = 4; // A power of two.
};

struct Track {
  std::string name;
  std::vector<Note> notes;             // Sorted by start, then key.
  std::vector<Controller> controllers; // In file order (ticks ascending).
  std::vector<ProgramChange> programs;
  std::uint64_t end_tick = 0; // The End of Track event's tick.
};

// What the reader repaired rather than refused, for the importer to report.
struct Repairs {
  std::size_t unmatched_note_offs = 0;
  std::size_t notes_closed_at_track_end = 0;
  std::size_t zero_length_notes = 0;
  std::size_t missing_end_of_track = 0;
};

struct File {
  std::uint16_t format = 0;
  Division division;
  std::vector<Track> tracks;
  // File-wide maps gathered from every track, sorted by tick; at one tick
  // the last event in file order wins.
  std::vector<TempoChange> tempo;
  std::vector<TimeSignature> meter;
  Repairs repairs;
};

[[nodiscard]] foundation::Result<File> parse(std::span<const std::uint8_t> bytes);

} // namespace oscillate::midi_file
