// The piano roll's musical tools (ADR-0059): hand-worked oracles for scale
// snapping, chords, strum, humanise, quantise, grooves, the groove file
// format and the session's one-step note edit.

#include "oscillate/application/midi_note_tools.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/persistence/groove_file.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

namespace {

namespace application = oscillate::application;
namespace persistence = oscillate::persistence;
namespace project = oscillate::project;
namespace fs = std::filesystem;
using oscillate::timeline::ticks_per_quarter;

constexpr std::int64_t sixteenth = ticks_per_quarter / 4;
constexpr std::int64_t eighth = ticks_per_quarter / 2;
constexpr std::int64_t bar = ticks_per_quarter * 4;

using oscillate::test::require;

project::NoteId note_id(std::uint64_t number) {
  return project::NoteId::from_bits({0, number}).value();
}

project::MidiNote note(std::uint64_t id, std::int64_t start, std::int64_t length, std::uint8_t key,
                       std::uint8_t velocity = 100) {
  return {note_id(id), {start}, {start + length}, key, velocity};
}

std::vector<project::NoteId> ids(const std::vector<project::MidiNote>& notes) {
  std::vector<project::NoteId> result;
  for (const auto& entry : notes)
    result.push_back(entry.id);
  return result;
}

const project::MidiNote& find(const std::vector<project::MidiNote>& notes, std::uint64_t id) {
  const auto found = std::ranges::find(notes, note_id(id), &project::MidiNote::id);
  REQUIRE_NE(found, notes.end(), "note missing from result");
  return *found;
}

const project::MusicalKey c_major{0, project::KeyMode::major};
const project::MusicalKey a_minor{9, project::KeyMode::minor};

void scales_snap_and_transpose() {
  const auto major = application::scale_pitch_classes(c_major);
  const std::array<bool, 12> white{true,  false, true,  false, true,  true,
                                   false, true,  false, true,  false, true};
  REQUIRE_EQ(major, white, "C major is the white keys");
  REQUIRE_EQ(application::scale_pitch_classes(a_minor), white, "A minor shares C major's notes");
  const auto e_minor = application::scale_pitch_classes({4, project::KeyMode::minor});
  REQUIRE_TRUE(e_minor[6], "E minor has F sharp, not F");
  REQUIRE_TRUE(!e_minor[5], "E minor has F sharp, not F");
  // C sharp is a semitone from C and D: a tie goes up.
  REQUIRE_EQ(application::snap_to_scale(61, c_major), 62, "C# snaps up to D");
  REQUIRE_EQ(application::snap_to_scale(64, c_major), 64, "an in-key note stays");
  REQUIRE_EQ(application::snap_to_scale(127, c_major), 127, "the top G stays");
  // E up two degrees is G; down three is B below.
  REQUIRE_EQ(application::transpose_in_scale(64, c_major, 2), 67, "E + 2 degrees = G");
  REQUIRE_EQ(application::transpose_in_scale(64, c_major, -3), 59, "E - 3 degrees = B");
  REQUIRE_EQ(application::scale_degrees_between(60, 72, c_major), 7, "an octave is 7 degrees");
  REQUIRE_EQ(application::scale_degrees_between(67, 64, c_major), -2, "G down to E is 2 degrees");
}

void chords_are_diatonic_in_key() {
  using Shape = application::ChordShape;
  const auto keys = [](std::vector<std::uint8_t> values) { return values; };
  REQUIRE_EQ(application::chord_keys(60, Shape::triad, 0, std::nullopt), keys({60, 64, 67}),
             "C major triad without a key");
  REQUIRE_EQ(application::chord_keys(62, Shape::triad, 0, c_major), keys({62, 65, 69}),
             "D in C major is D minor");
  REQUIRE_EQ(application::chord_keys(67, Shape::seventh, 0, c_major), keys({67, 71, 74, 77}),
             "G7 in C major is dominant");
  REQUIRE_EQ(application::chord_keys(60, Shape::seventh, 0, std::nullopt), keys({60, 64, 67, 71}),
             "C major 7th without a key");
  REQUIRE_EQ(application::chord_keys(60, Shape::sus4, 0, std::nullopt), keys({60, 65, 67}),
             "Csus4");
  REQUIRE_EQ(application::chord_keys(60, Shape::sus2, 0, c_major), keys({60, 62, 67}), "Csus2");
  REQUIRE_EQ(application::chord_keys(60, Shape::add9, 0, std::nullopt), keys({60, 64, 67, 74}),
             "Cadd9");
  REQUIRE_EQ(application::chord_keys(60, Shape::triad, 1, std::nullopt), keys({64, 67, 72}),
             "first inversion lifts the root");
  REQUIRE_EQ(application::chord_keys(60, Shape::triad, 2, std::nullopt), keys({67, 72, 76}),
             "second inversion lifts root and third");
  // C sharp snaps to D first in C major.
  REQUIRE_EQ(application::chord_keys(61, Shape::triad, 0, c_major), keys({62, 65, 69}),
             "the root snaps into the key");
  // A minor's B triad is diminished.
  REQUIRE_EQ(application::chord_keys(71, Shape::triad, 0, a_minor), keys({71, 74, 77}),
             "B in A minor is diminished");
  REQUIRE_EQ(application::chord_keys(123, Shape::triad, 0, std::nullopt), keys({123, 127}),
             "tones above 127 are left out");
}

void strums_spread_chords() {
  const std::vector<project::MidiNote> notes{
      note(1, 0, ticks_per_quarter, 60), note(2, 0, ticks_per_quarter, 64),
      note(3, 0, ticks_per_quarter, 67), note(4, eighth, ticks_per_quarter, 72)};
  const auto step = ticks_per_quarter / 32;
  const auto up = application::strum_notes(notes, ids(notes), step, true, bar);
  REQUIRE_EQ(up.size(), 2, "the lowest tone and the lone note stay");
  REQUIRE_EQ(find(up, 2).start.value, step, "upward: each higher tone a step later");
  REQUIRE_EQ(find(up, 3).start.value, 2 * step, "upward: each higher tone a step later");
  REQUIRE_EQ(find(up, 3).end.value, ticks_per_quarter, "ends stay");
  const auto down = application::strum_notes(notes, ids(notes), step, false, bar);
  REQUIRE_EQ(find(down, 2).start.value, step, "downward: the top first");
  REQUIRE_EQ(find(down, 1).start.value, 2 * step, "downward: the top first");
  // A short tone never starts at or past its end.
  const std::vector<project::MidiNote> short_chord{note(1, 0, 10, 60), note(2, 0, 10, 64)};
  const auto clipped = application::strum_notes(short_chord, ids(short_chord), 100, true, bar);
  REQUIRE_EQ(clipped.size(), 1, "a strum stops before the end");
  REQUIRE_EQ(clipped.front().start.value, 9, "a strum stops before the end");
}

void humanise_is_reproducible_and_bounded() {
  std::vector<project::MidiNote> notes;
  for (std::uint64_t index = 0; index < 32; ++index)
    notes.push_back(
        note(index + 1, static_cast<std::int64_t>(index) * eighth + eighth, sixteenth, 60));
  const auto most = ticks_per_quarter / 8;
  const auto first = application::humanize_timing(notes, ids(notes), most, 7, 20 * bar);
  const auto again = application::humanize_timing(notes, ids(notes), most, 7, 20 * bar);
  const auto other = application::humanize_timing(notes, ids(notes), most, 8, 20 * bar);
  REQUIRE_EQ(first, again, "one seed, one take");
  REQUIRE_NE(first, other, "another seed, another take");
  REQUIRE_GT(first.size(), 20, "most notes move");
  for (const auto& moved : first) {
    const auto& original = find(notes, moved.id.bits().low);
    REQUIRE_LE(std::llabs(moved.start.value - original.start.value), most, "within the range");
    REQUIRE_EQ(moved.end.value - moved.start.value, sixteenth, "lengths stay");
  }
  require(application::humanize_timing(notes, ids(notes), 0, 7, 20 * bar).empty(),
          "no range, no moves");
}

void grids_run_from_quarters_to_128ths() {
  for (const auto steps : {4, 8, 16, 32, 64, 128})
    require(application::supported_midi_grid(steps), "a power-of-two grid to 1/128 is offered");
  for (const auto steps : {0, -16, 3, 12, 24, 256})
    require(!application::supported_midi_grid(steps), "an odd, triplet or finer grid is refused");
  REQUIRE_EQ(application::midi_grid_ticks(16), sixteenth, "a 16th step is a 16th note");
  REQUIRE_EQ(application::midi_grid_ticks(128) * 128, bar, "128 of the finest steps make a bar");
}

void quantise_strength_swing_and_song_grid() {
  const std::vector<project::MidiNote> notes{note(1, 130'000'000, sixteenth, 60),
                                             note(2, 700'000'000, sixteenth, 62)};
  const auto all = ids(notes);
  auto full = application::quantize_notes(notes, all, {sixteenth, 1.0, 0.5, false},
                                          {.clip_start = 0, .pattern_length = bar});
  REQUIRE_TRUE(full, "130M goes to the 16th");
  REQUIRE_EQ(find(full.value(), 1).start.value, sixteenth, "130M goes to the 16th");
  REQUIRE_EQ(find(full.value(), 2).start.value, 720'000'000, "700M goes to 720M");
  auto half = application::quantize_notes(notes, all, {sixteenth, 0.5, 0.5, false},
                                          {.clip_start = 0, .pattern_length = bar});
  REQUIRE_TRUE(half, "half strength goes half way");
  REQUIRE_EQ(find(half.value(), 1).start.value, 185'000'000, "half strength goes half way");
  // 75% swing on 8ths puts every second 8th three quarters through the beat.
  auto swung = application::quantize_notes(notes, all, {eighth, 1.0, 0.75, false},
                                           {.clip_start = 0, .pattern_length = bar});
  REQUIRE_TRUE(swung, "the swung 8th");
  REQUIRE_EQ(find(swung.value(), 2).start.value, 720'000'000, "the swung 8th");
  REQUIRE_EQ(find(swung.value(), 1).start.value, 0, "the downbeat stays straight");
  // The grid is the song's: a clip starting on the off-16th lands its
  // first note on the song's 8th, half way between lines going later.
  const std::vector<project::MidiNote> offset{note(1, 0, sixteenth, 60)};
  auto song = application::quantize_notes(offset, ids(offset), {eighth, 1.0, 0.5, false},
                                          {.clip_start = sixteenth, .pattern_length = bar});
  REQUIRE_TRUE(song, "a tie goes to the later line");
  REQUIRE_EQ(song.value().front().start.value, sixteenth, "a tie goes to the later line");
  // Ends quantise too when asked.
  const std::vector<project::MidiNote> long_note{note(1, 0, 300'000'000, 60)};
  auto ends = application::quantize_notes(long_note, ids(long_note), {sixteenth, 1.0, 0.5, true},
                                          {.clip_start = 0, .pattern_length = bar});
  REQUIRE_TRUE(ends, "the end snaps to the grid");
  REQUIRE_EQ(ends.value().front().end.value, sixteenth, "the end snaps to the grid");
  const std::vector<project::MidiNote> crowded{note(1, 10, sixteenth, 60),
                                               note(2, 20, sixteenth, 60)};
  require(!application::quantize_notes(crowded, ids(crowded), {sixteenth, 1.0, 0.5, false},
                                       {.clip_start = 0, .pattern_length = bar}),
          "two notes landing together are refused");
  // Notes kept after the pattern's end stay.
  const std::vector<project::MidiNote> silent{note(1, bar + 7, sixteenth, 60)};
  auto kept = application::quantize_notes(silent, ids(silent), {sixteenth, 1.0, 0.5, false},
                                          {.clip_start = 0, .pattern_length = bar});
  REQUIRE_TRUE(kept, "silent notes stay");
  REQUIRE_TRUE(kept.value().empty(), "silent notes stay");
}

void grooves_move_and_accent() {
  const auto pool = application::builtin_grooves();
  const auto named = [&](std::string_view name) {
    const auto found = std::ranges::find(pool, name, &persistence::GrooveTemplate::name);
    REQUIRE_NE(found, pool.end(), "a built-in groove is missing");
    return *found;
  };
  for (const auto& groove : pool)
    require(static_cast<bool>(persistence::validate_groove(groove)), "built-ins are valid");
  const std::vector<project::MidiNote> notes{note(1, 0, sixteenth, 42),
                                             note(2, sixteenth, sixteenth, 42),
                                             note(3, 2 * sixteenth, sixteenth, 42)};
  // 75% swing on 16ths: the off-16th moves half a 16th late.
  auto swung = application::apply_groove(notes, ids(notes), named("Swing 16ths 75%"), {},
                                         {.clip_start = 0, .pattern_length = bar});
  REQUIRE_TRUE(swung, "only the off-16th moves");
  REQUIRE_EQ(swung.value().size(), 1, "only the off-16th moves");
  REQUIRE_EQ(find(swung.value(), 2).start.value, sixteenth + sixteenth / 2, "half a 16th late");
  // Half the timing amount moves half as far.
  auto gentle =
      application::apply_groove(notes, ids(notes), named("Swing 16ths 75%"), {1.0, 0.5, 1.0},
                                {.clip_start = 0, .pattern_length = bar});
  REQUIRE_TRUE(gentle, "half timing");
  REQUIRE_EQ(find(gentle.value(), 2).start.value, sixteenth + sixteenth / 4, "half timing");
  // MPC accents: the "e" plays at 78% of its velocity.
  auto accented =
      application::apply_groove(notes, ids(notes), named("MPC 16ths 54%"), {1.0, 0.0, 1.0},
                                {.clip_start = 0, .pattern_length = bar});
  REQUIRE_TRUE(accented, "the e is softer");
  REQUIRE_EQ(find(accented.value(), 2).velocity, 78, "the e is softer");
  REQUIRE_EQ(find(accented.value(), 3).velocity, 90, "the and is a little softer");
  // Quantise pulls an early hit onto the groove's grid first.
  const std::vector<project::MidiNote> early{note(1, sixteenth - 1'000'000, sixteenth, 42)};
  auto pulled =
      application::apply_groove(early, ids(early), named("Swing 16ths 75%"), {1.0, 1.0, 0.0},
                                {.clip_start = 0, .pattern_length = bar});
  REQUIRE_TRUE(pulled, "quantise then swing");
  REQUIRE_EQ(pulled.value().front().start.value, sixteenth + sixteenth / 2, "quantise then swing");
  // The groove runs on the song's grid: a clip at bar 2 lines up the same.
  const std::vector<project::MidiNote> later{note(1, sixteenth, sixteenth, 42)};
  auto shifted = application::apply_groove(later, ids(later), named("Swing 16ths 75%"), {},
                                           {.clip_start = bar, .pattern_length = bar});
  REQUIRE_TRUE(shifted, "a clip on a bar line grooves alike");
  REQUIRE_EQ(shifted.value().front().start.value, sixteenth + sixteenth / 2,
             "a clip on a bar line grooves alike");
}

void groove_files_round_trip_strictly() {
  const persistence::GrooveTemplate groove{"Late Hats", 4, {{0.0, 1.0}, {0.125, 0.8}}};
  const auto text = persistence::encode_groove(groove);
  auto decoded = persistence::decode_groove(text);
  REQUIRE_TRUE(decoded, "a groove file round-trips");
  REQUIRE_EQ(decoded.value(), groove, "a groove file round-trips");
  require(!persistence::decode_groove(R"({"kind":"oscillate.groove","version":1,"name":"x",
      "steps_per_quarter":4,"steps":[{"timing":0.9,"velocity":1}]})"),
          "a timing beyond half a step is refused");
  require(!persistence::decode_groove(R"({"kind":"oscillate.groove","version":1,"name":"x",
      "name":"y","steps_per_quarter":4,"steps":[{"timing":0,"velocity":1}]})"),
          "duplicate fields are refused");
  require(!persistence::decode_groove(R"({"kind":"oscillate.groove","version":1,"name":"x",
      "steps_per_quarter":4,"steps":[{"timing":0,"velocity":1,"extra":1}]})"),
          "unknown fields are refused");
  require(!persistence::decode_groove(R"({"kind":"oscillate.native-effect-preset","version":1,
      "name":"x","steps_per_quarter":4,"steps":[]})"),
          "another kind is refused");
  require(!persistence::decode_groove(R"({"kind":"oscillate.groove","version":1,"name":"x",
      "steps_per_quarter":5,"steps":[{"timing":0,"velocity":1}]})"),
          "an odd grid is refused");
  require(!persistence::decode_groove(std::string(persistence::max_groove_file_bytes + 1, ' ')),
          "an oversized file is refused");
}

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-note-tools") {}
};

void session_applies_one_step_edits() {
  Directory directory;
  auto opened = application::ProjectSession::create(directory.path() / "song");
  REQUIRE_OK(opened, "project creation failed");
  auto session = std::move(opened.value());
  const auto track = session->instruments().add_synth_track();
  REQUIRE_OK(track, "synth track failed");
  REQUIRE_OK(session->midi_clips().add_midi_clip(track.value(), {0}), "MIDI clip failed");
  const auto clip_of = [&]() -> const project::MidiClip& {
    return session->snapshot().tracks.front().midi_clips.front();
  };
  const auto clip = clip_of().id;
  REQUIRE_OK(session->notes().add_note(clip, {0}, 60), "fixture notes failed");
  REQUIRE_OK(session->notes().add_note(clip, {sixteenth}, 62), "fixture notes failed");
  const auto before = session->snapshot().revision;
  auto moved = clip_of().notes.front();
  moved.start = {eighth};
  moved.end = {eighth + (moved.end.value - 0)};
  moved.velocity = 64;
  const std::vector<project::MidiNote> changed{moved};
  const std::vector<project::MidiNote> added{note(99, 0, sixteenth, 64),
                                             note(99, 0, sixteenth, 67)};
  auto created = session->notes().apply_note_edit(clip, {.changed = changed, .added = added});
  REQUIRE_TRUE(created, "the edit applies and names the new notes");
  REQUIRE_EQ(created.value().size(), 2, "the edit applies and names the new notes");
  REQUIRE_EQ(session->snapshot().revision, before + 1, "one revision");
  REQUIRE_EQ(clip_of().notes.size(), 4, "two notes added");
  REQUIRE_NE(created.value()[0], created.value()[1], "new notes get fresh ids");
  REQUIRE_NE(created.value()[0], note_id(99), "new notes get fresh ids");
  // A clash changes nothing.
  const std::vector<project::MidiNote> clash{note(98, eighth, sixteenth, 60)};
  require(!session->notes().apply_note_edit(clip, {.changed = {}, .added = clash}),
          "a new note on an old one is refused");
  REQUIRE_EQ(session->snapshot().revision, before + 1, "a refused edit changes nothing");
  const std::vector unknown{note(12345, 0, sixteenth, 70)};
  require(!session->notes().apply_note_edit(clip, {.changed = unknown, .added = {}}),
          "an unknown note is refused");
  REQUIRE_OK(session->history().undo(), "one undo removes the whole edit");
  REQUIRE_EQ(clip_of().notes.size(), 2, "one undo removes the whole edit");
  REQUIRE_EQ(clip_of().notes.front().start.value, 0, "one undo removes the whole edit");
}

} // namespace

int main() {
  try {
    scales_snap_and_transpose();
    chords_are_diatonic_in_key();
    strums_spread_chords();
    humanise_is_reproducible_and_bounded();
    grids_run_from_quarters_to_128ths();
    quantise_strength_swing_and_song_grid();
    grooves_move_and_accent();
    groove_files_round_trip_strictly();
    session_applies_one_step_edits();
  } catch (const std::exception& error) {
    std::cerr << "midi note tools: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
