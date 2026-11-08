#include "oscillate/midi_file/smf.hpp"
#include "oscillate/test/check.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// ADR-0045: the Standard MIDI File reader, from hand-built bytes — every
// supported feature, and malformed input failing cleanly.
namespace {
namespace smf = oscillate::midi_file;
using Bytes = std::vector<std::uint8_t>;
using oscillate::foundation::ErrorCode;

using oscillate::test::require;

void append(Bytes& out, std::initializer_list<std::uint8_t> bytes) {
  out.insert(out.end(), bytes.begin(), bytes.end());
}

void vlq(Bytes& out, std::uint32_t value) {
  std::uint8_t buffer[4];
  int count = 0;
  buffer[count++] = static_cast<std::uint8_t>(value & 0x7FU);
  while ((value >>= 7U) != 0)
    buffer[count++] = static_cast<std::uint8_t>((value & 0x7FU) | 0x80U);
  while (count > 0)
    out.push_back(buffer[--count]);
}

void u32(Bytes& out, std::uint32_t value) {
  append(out, {static_cast<std::uint8_t>(value >> 24U), static_cast<std::uint8_t>(value >> 16U),
               static_cast<std::uint8_t>(value >> 8U), static_cast<std::uint8_t>(value)});
}

Bytes header(std::uint16_t format, std::uint16_t tracks, std::uint16_t division) {
  Bytes out{'M', 'T', 'h', 'd'};
  u32(out, 6);
  append(out, {static_cast<std::uint8_t>(format >> 8U), static_cast<std::uint8_t>(format),
               static_cast<std::uint8_t>(tracks >> 8U), static_cast<std::uint8_t>(tracks),
               static_cast<std::uint8_t>(division >> 8U), static_cast<std::uint8_t>(division)});
  return out;
}

void chunk(Bytes& file, const char* tag, const Bytes& body) {
  file.insert(file.end(), tag, tag + 4);
  u32(file, static_cast<std::uint32_t>(body.size()));
  file.insert(file.end(), body.begin(), body.end());
}

// Builds one track's events with explicit deltas.
struct TrackBytes {
  Bytes body;
  TrackBytes& at(std::uint32_t delta, std::initializer_list<std::uint8_t> event) {
    vlq(body, delta);
    append(body, event);
    return *this;
  }
  TrackBytes& meta(std::uint32_t delta, std::uint8_t type, const Bytes& data) {
    vlq(body, delta);
    append(body, {0xFF, type});
    vlq(body, static_cast<std::uint32_t>(data.size()));
    body.insert(body.end(), data.begin(), data.end());
    return *this;
  }
  TrackBytes& end(std::uint32_t delta = 0) { return meta(delta, 0x2F, {}); }
};

smf::File parsed(const Bytes& bytes) {
  auto result = smf::parse(bytes);
  if (!result)
    throw std::runtime_error("parse failed: " + result.error().message);
  return std::move(result).value();
}

void refused(const Bytes& bytes, ErrorCode code, std::string_view why) {
  const auto result = smf::parse(bytes);
  require(!result, std::string(why) + " was accepted");
  REQUIRE_EQ(result.error().code, code,
             std::string(why) + " failed with the wrong code: " + result.error().message);
}

void format_zero_with_running_status() {
  TrackBytes track;
  track.meta(0, 0x03, {'L', 'e', 'a', 'd', 0x07})
      .meta(0, 0x51, {0x07, 0xA1, 0x20}) // 500000 us = 120 BPM
      .meta(0, 0x58, {6, 3, 24, 8})      // 6/8
      .at(0, {0x90, 60, 100})            // C4 on
      .at(0, {64, 90})                   // running status: E4 on
      .at(240, {60, 0})                  // running status: C4 off (velocity 0)
      .at(0, {0x80, 64, 0})              // E4 off
      .at(10, {0xB0, 64, 127})           // sustain down
      .at(10, {64, 63})                  // running status: half pedal
      .at(0, {0xC0, 5})                  // program change: one data byte
      .at(0, {0xE0, 0, 64})              // pitch bend, ignored
      .end(100);
  auto bytes = header(0, 1, 480);
  chunk(bytes, "MTrk", track.body);
  const auto file = parsed(bytes);
  REQUIRE_EQ(file.format, 0, "header");
  REQUIRE_TRUE(!file.division.smpte, "header");
  REQUIRE_EQ(file.division.ticks_per_quarter, 480, "header");
  REQUIRE_EQ(file.tracks.size(), 1, "the track name is read without control bytes");
  REQUIRE_EQ(file.tracks[0].name, "Lead", "the track name is read without control bytes");
  const auto& notes = file.tracks[0].notes;
  REQUIRE_EQ(notes.size(), 2, "running status and velocity-0 note-offs pair notes");
  REQUIRE_EQ(notes[0].key, 60, "running status and velocity-0 note-offs pair notes");
  REQUIRE_EQ(notes[0].start, 0, "running status and velocity-0 note-offs pair notes");
  REQUIRE_EQ(notes[0].end, 240, "running status and velocity-0 note-offs pair notes");
  REQUIRE_EQ(notes[0].velocity, 100, "running status and velocity-0 note-offs pair notes");
  REQUIRE_EQ(notes[1].key, 64, "running status and velocity-0 note-offs pair notes");
  REQUIRE_EQ(notes[1].end, 240, "running status and velocity-0 note-offs pair notes");
  const auto& cc = file.tracks[0].controllers;
  REQUIRE_EQ(cc.size(), 2, "controllers keep their ticks");
  REQUIRE_EQ(cc[0].tick, 250, "controllers keep their ticks");
  REQUIRE_EQ(cc[0].value, 127, "controllers keep their ticks");
  REQUIRE_EQ(cc[1].tick, 260, "controllers keep their ticks");
  REQUIRE_EQ(cc[1].value, 63, "controllers keep their ticks");
  REQUIRE_EQ(file.tracks[0].programs.size(), 1, "program changes read one data byte");
  REQUIRE_EQ(file.tracks[0].programs[0].program, 5, "program changes read one data byte");
  REQUIRE_EQ(file.tracks[0].end_tick, 360, "End of Track tick");
  REQUIRE_EQ(file.tempo.size(), 1, "tempo");
  REQUIRE_EQ(file.tempo[0].microseconds_per_quarter, 500'000, "tempo");
  REQUIRE_EQ(file.meter.size(), 1, "time signature");
  REQUIRE_EQ(file.meter[0].numerator, 6, "time signature");
  REQUIRE_EQ(file.meter[0].denominator, 8, "time signature");
  const auto& r = file.repairs;
  REQUIRE_EQ(r.unmatched_note_offs, 0, "a clean file needs no repairs");
  REQUIRE_EQ(r.notes_closed_at_track_end, 0, "a clean file needs no repairs");
  REQUIRE_EQ(r.zero_length_notes, 0, "a clean file needs no repairs");
  REQUIRE_EQ(r.missing_end_of_track, 0, "a clean file needs no repairs");
}

void format_one_merges_maps_and_skips_unknown_chunks() {
  TrackBytes conductor;
  conductor.meta(0, 0x51, {0x07, 0xA1, 0x20})
      .meta(0, 0x51, {0x06, 0x1A, 0x80}) // Same tick: the later one wins (150 BPM).
      .meta(960, 0x51, {0x09, 0x27, 0xC0})
      .end();
  TrackBytes notes;
  notes.at(0, {0x91, 36, 80}).at(120, {0x81, 36, 0}).end();
  auto bytes = header(1, 2, 96);
  chunk(bytes, "MTrk", conductor.body);
  chunk(bytes, "XFIH", {1, 2, 3}); // Unknown chunk between tracks.
  chunk(bytes, "MTrk", notes.body);
  chunk(bytes, "MTrk", {0x00, 0xFF, 0x2F, 0x00}); // Beyond the declared count: ignored.
  const auto file = parsed(bytes);
  REQUIRE_EQ(file.tracks.size(), 2, "declared tracks are read, extra chunks ignored");
  REQUIRE_EQ(file.tempo.size(), 2, "tempo merges file-wide, the last event at a tick wins");
  REQUIRE_EQ(file.tempo[0].microseconds_per_quarter, 400'000,
             "tempo merges file-wide, the last event at a tick wins");
  REQUIRE_EQ(file.tempo[1].tick, 960, "tempo merges file-wide, the last event at a tick wins");
  REQUIRE_EQ(file.tracks[1].notes.size(), 1, "channels are kept");
  REQUIRE_EQ(file.tracks[1].notes[0].channel, 1, "channels are kept");
}

void overlapping_notes_pair_first_in_first_out() {
  TrackBytes track;
  track.at(0, {0x90, 60, 10})
      .at(10, {0x90, 60, 20}) // Second C4 before the first ends.
      .at(10, {0x80, 60, 0})  // Ends the first.
      .at(10, {0x80, 60, 0})  // Ends the second.
      .at(0, {0x80, 61, 0})   // Unmatched.
      .at(0, {0x90, 62, 30})
      .at(0, {0x80, 62, 0})  // Zero length.
      .at(0, {0x90, 63, 40}) // Never closed.
      .end(50);
  auto bytes = header(0, 1, 480);
  chunk(bytes, "MTrk", track.body);
  const auto file = parsed(bytes);
  const auto& notes = file.tracks[0].notes;
  REQUIRE_EQ(notes.size(), 4, "four notes");
  REQUIRE_EQ(notes[0].key, 60, "same-pitch notes pair first in, first out");
  REQUIRE_EQ(notes[0].start, 0, "same-pitch notes pair first in, first out");
  REQUIRE_EQ(notes[0].end, 20, "same-pitch notes pair first in, first out");
  REQUIRE_EQ(notes[0].velocity, 10, "same-pitch notes pair first in, first out");
  REQUIRE_EQ(notes[1].key, 60, "same-pitch notes pair first in, first out");
  REQUIRE_EQ(notes[1].start, 10, "same-pitch notes pair first in, first out");
  REQUIRE_EQ(notes[1].end, 30, "same-pitch notes pair first in, first out");
  REQUIRE_EQ(notes[1].velocity, 20, "same-pitch notes pair first in, first out");
  REQUIRE_EQ(notes[2].key, 62, "a zero-length note lasts one tick");
  REQUIRE_EQ(notes[2].end, notes[2].start + 1, "a zero-length note lasts one tick");
  REQUIRE_EQ(notes[3].key, 63, "a held note ends at End of Track");
  REQUIRE_EQ(notes[3].start, 30, "a held note ends at End of Track");
  REQUIRE_EQ(notes[3].end, 80, "a held note ends at End of Track");
  REQUIRE_EQ(file.repairs.unmatched_note_offs, 1, "repairs are counted");
  REQUIRE_EQ(file.repairs.zero_length_notes, 1, "repairs are counted");
  REQUIRE_EQ(file.repairs.notes_closed_at_track_end, 1, "repairs are counted");
}

void smpte_and_missing_end_of_track() {
  TrackBytes track;
  track.at(0, {0x90, 60, 100}).at(40, {0x80, 60, 0}); // No End of Track.
  auto bytes = header(0, 1, 0xE728);                  // -25 fps, 40 ticks per frame.
  chunk(bytes, "MTrk", track.body);
  const auto file = parsed(bytes);
  REQUIRE_TRUE(file.division.smpte, "SMPTE division");
  REQUIRE_EQ(file.division.frames_per_second, 25, "SMPTE division");
  REQUIRE_EQ(file.division.ticks_per_frame, 40, "SMPTE division");
  REQUIRE_EQ(file.repairs.missing_end_of_track, 1,
             "a missing End of Track is repaired at the last event");
  REQUIRE_EQ(file.tracks[0].end_tick, 40, "a missing End of Track is repaired at the last event");
  auto drop = header(0, 1, 0xE304); // -29: 29.97 drop-frame.
  chunk(drop, "MTrk", TrackBytes{}.end().body);
  REQUIRE_EQ(parsed(drop).division.frames_per_second, 29, "29.97 drop-frame");
}

void sysex_and_meta_cancel_running_status() {
  TrackBytes track;
  track.at(0, {0x90, 60, 100}).at(0, {0xF0, 0x02, 0x7E, 0xF7}).at(10, {60, 0}).end();
  auto bytes = header(0, 1, 480);
  chunk(bytes, "MTrk", track.body);
  refused(bytes, ErrorCode::corrupt_data, "running status after SysEx");
  // A meta event other than End of Track cancels it too.
  TrackBytes after_meta;
  after_meta.at(0, {0x90, 60, 100}).meta(0, 0x01, {'x'}).at(10, {60, 0}).end();
  auto meta_bytes = header(0, 1, 480);
  chunk(meta_bytes, "MTrk", after_meta.body);
  refused(meta_bytes, ErrorCode::corrupt_data, "running status after a meta event");
}

// Track names come out as valid UTF-8: UTF-8 is kept, other bytes are read as
// Latin-1, controls are dropped and the length limit never splits a character.
void track_names_are_utf8() {
  const auto name_of = [](const Bytes& text) {
    TrackBytes track;
    track.meta(0, 0x03, text).end();
    auto bytes = header(0, 1, 480);
    chunk(bytes, "MTrk", track.body);
    return parsed(bytes).tracks[0].name;
  };
  REQUIRE_EQ(name_of({'C', 0xC3, 0xA9, 'l'}), "C\xC3\xA9l", "UTF-8 names are kept");
  REQUIRE_EQ(name_of({'C', 0xE9, 'l', 0x85}), "C\xC3\xA9l", "Latin-1 names are transcoded");
  Bytes long_name(smf::max_track_name_bytes - 1, 'a');
  long_name.push_back(0xC3);
  long_name.push_back(0xA9);
  const auto cut = name_of(long_name);
  REQUIRE_EQ(cut, std::string(smf::max_track_name_bytes - 1, 'a'),
             "the length limit does not split a character");
}

void malformed_input_fails_cleanly() {
  refused({}, ErrorCode::corrupt_data, "an empty file");
  refused({'R', 'I', 'F', 'F', 0, 0, 0, 0}, ErrorCode::corrupt_data, "a non-MIDI file");
  {
    auto bytes = header(2, 1, 480);
    chunk(bytes, "MTrk", TrackBytes{}.end().body);
    refused(bytes, ErrorCode::unsupported, "format 2");
  }
  {
    auto bytes = header(0, 2, 480);
    chunk(bytes, "MTrk", TrackBytes{}.end().body);
    chunk(bytes, "MTrk", TrackBytes{}.end().body);
    refused(bytes, ErrorCode::corrupt_data, "format 0 with two tracks");
  }
  {
    auto bytes = header(1, 2, 480);
    chunk(bytes, "MTrk", TrackBytes{}.end().body);
    refused(bytes, ErrorCode::corrupt_data, "a missing declared track");
  }
  {
    auto bytes = header(0, 1, 0);
    chunk(bytes, "MTrk", TrackBytes{}.end().body);
    refused(bytes, ErrorCode::corrupt_data, "a zero division");
  }
  {
    auto bytes = header(0, 1, 0xE628); // -26 fps
    chunk(bytes, "MTrk", TrackBytes{}.end().body);
    refused(bytes, ErrorCode::corrupt_data, "an unknown SMPTE rate");
  }
  {
    auto bytes = header(1, 1000, 480);
    refused(bytes, ErrorCode::resource_exhausted, "too many tracks");
  }
  {
    auto bytes = header(0, 1, 480);
    Bytes body;
    append(body, {0x00, 0x90, 60});
    chunk(bytes, "MTrk", body);
    refused(bytes, ErrorCode::corrupt_data, "a message cut short");
  }
  {
    auto bytes = header(0, 1, 480);
    bytes.insert(bytes.end(), {'M', 'T', 'r', 'k', 0, 0, 0, 50, 0x00});
    refused(bytes, ErrorCode::corrupt_data, "a chunk longer than the file");
  }
  {
    auto bytes = header(0, 1, 480);
    chunk(bytes, "MTrk", {0x81, 0x81, 0x81, 0x81, 0x00, 0xFF, 0x2F, 0x00});
    refused(bytes, ErrorCode::corrupt_data, "a five-byte variable length");
  }
  {
    auto bytes = header(0, 1, 480);
    chunk(bytes, "MTrk", TrackBytes{}.at(0, {60, 100}).end().body);
    refused(bytes, ErrorCode::corrupt_data, "data without a status");
  }
  {
    auto bytes = header(0, 1, 480);
    chunk(bytes, "MTrk", TrackBytes{}.at(0, {0x90, 0x90, 100}).end().body);
    refused(bytes, ErrorCode::corrupt_data, "a status byte in a data position");
  }
  {
    auto bytes = header(0, 1, 480);
    chunk(bytes, "MTrk", TrackBytes{}.meta(0, 0x51, {0, 0, 0}).end().body);
    refused(bytes, ErrorCode::corrupt_data, "a zero tempo");
  }
  {
    auto bytes = header(0, 1, 480);
    chunk(bytes, "MTrk", TrackBytes{}.meta(0, 0x51, {7, 0xA1}).end().body);
    refused(bytes, ErrorCode::corrupt_data, "a short tempo");
  }
  {
    auto bytes = header(0, 1, 480);
    chunk(bytes, "MTrk", TrackBytes{}.meta(0, 0x58, {0, 2, 24, 8}).end().body);
    refused(bytes, ErrorCode::corrupt_data, "a zero time-signature numerator");
  }
  {
    auto bytes = header(0, 1, 480);
    chunk(bytes, "MTrk", TrackBytes{}.at(0, {0xF2, 0, 0}).end().body);
    refused(bytes, ErrorCode::corrupt_data, "a system common message");
  }
  {
    auto bytes = header(0, 1, 480);
    Bytes body;
    append(body, {0x00, 0xFF, 0x7F, 0x20, 1, 2}); // Meta length past the chunk.
    chunk(bytes, "MTrk", body);
    refused(bytes, ErrorCode::corrupt_data, "a meta event longer than its chunk");
  }
  {
    TrackBytes track;
    track.at(0x0FFFFFFF, {0x90, 60, 1});
    for (int i = 0; i < 5'000; ++i)
      track.at(0x0FFFFFFF, {0x80, 60, 0});
    auto bytes = header(0, 1, 480);
    chunk(bytes, "MTrk", track.end().body);
    refused(bytes, ErrorCode::resource_exhausted, "a file beyond the tick bound");
  }
  {
    Bytes big(smf::max_file_bytes + 1, 0);
    refused(big, ErrorCode::resource_exhausted, "an oversized file");
  }
}

// Deterministic mutation of a valid file: every outcome is a clean result.
void mutations_never_misbehave() {
  TrackBytes track;
  track.meta(0, 0x03, {'x'})
      .meta(0, 0x51, {0x07, 0xA1, 0x20})
      .meta(0, 0x58, {4, 2, 24, 8})
      .at(0, {0x90, 60, 100})
      .at(10, {62, 90})
      .at(10, {0xB0, 64, 127})
      .at(10, {0x80, 60, 0})
      .at(0, {0xF0, 1, 0xF7})
      .at(5, {0x80, 62, 0})
      .end();
  auto base = header(1, 2, 480);
  chunk(base, "MTrk", track.body);
  chunk(base, "MTrk", track.body);
  std::uint32_t state = 0x1234567U;
  const auto next = [&] {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return state;
  };
  std::size_t accepted = 0;
  for (int round = 0; round < 50'000; ++round) {
    auto bytes = base;
    const auto edits = 1 + next() % 4;
    for (std::uint32_t e = 0; e < edits; ++e) {
      const auto at = next() % bytes.size();
      switch (next() % 3) {
      case 0:
        bytes[at] = static_cast<std::uint8_t>(next());
        break;
      case 1:
        bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(at));
        break;
      default:
        bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                     static_cast<std::uint8_t>(next()));
        break;
      }
      if (bytes.empty())
        bytes.push_back(0);
    }
    const auto result = smf::parse(bytes);
    if (result) {
      ++accepted;
      for (const auto& parsed_track : result.value().tracks) {
        for (const auto& note : parsed_track.notes)
          require(note.end > note.start && note.velocity > 0 && note.key < 128 && note.channel < 16,
                  "every accepted note is well formed");
      }
    }
  }
  REQUIRE_GT(accepted, 0, "some mutations stay valid");
}

} // namespace

int main() {
  try {
    format_zero_with_running_status();
    format_one_merges_maps_and_skips_unknown_chunks();
    overlapping_notes_pair_first_in_first_out();
    smpte_and_missing_end_of_track();
    sysex_and_meta_cancel_running_status();
    track_names_are_utf8();
    malformed_input_fails_cleanly();
    mutations_never_misbehave();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "MIDI file contract passed\n";
  return 0;
}
