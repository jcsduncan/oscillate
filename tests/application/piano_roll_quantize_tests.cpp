#include "oscillate/application/midi_note_tools.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace {
namespace fs = std::filesystem;
using oscillate::application::ProjectSession;

using oscillate::test::require;

struct Scratch : public oscillate::test::TempDir {
  Scratch() : TempDir("oscillate-piano") {}
  const std::filesystem::path root{path()};
};

void quantise_preserves_notes_and_is_one_undo_step() {
  Scratch scratch;
  auto created = ProjectSession::create(scratch.root / "project");
  REQUIRE_OK(created, "project creation failed");
  auto session = std::move(created).value();
  auto track = session->instruments().add_synth_track();
  REQUIRE_OK(track, "MIDI clip creation failed");
  REQUIRE_OK(session->midi_clips().add_midi_clip(track.value(), {0}), "MIDI clip creation failed");
  const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
  REQUIRE_OK(session->notes().add_note_with_duration(clip, {120'000'000}, {120'000'000}, 60),
             "grid-length notes were not added");
  REQUIRE_OK(session->notes().add_note_with_duration(clip, {900'000'000}, {480'000'000}, 62),
             "grid-length notes were not added");
  const auto before = session->snapshot().revision;
  REQUIRE_OK(session->midi_clips().quantize_midi_clip(clip, 8),
             "quantise did not commit exactly one project revision");
  REQUIRE_EQ(session->snapshot().revision, before + 1,
             "quantise did not commit exactly one project revision");
  const auto& quantised = session->snapshot().tracks.front().midi_clips.front().notes;
  REQUIRE_EQ(quantised[0].start.value, 0,
             "quantise changed duration or velocity, or chose wrong grid positions");
  REQUIRE_EQ(quantised[0].end.value, 120'000'000,
             "quantise changed duration or velocity, or chose wrong grid positions");
  REQUIRE_EQ(quantised[1].start.value, 960'000'000,
             "quantise changed duration or velocity, or chose wrong grid positions");
  REQUIRE_EQ(quantised[1].end.value, 1'440'000'000,
             "quantise changed duration or velocity, or chose wrong grid positions");
  REQUIRE_EQ(quantised[0].velocity, 100,
             "quantise changed duration or velocity, or chose wrong grid positions");
  REQUIRE_OK(session->history().undo(), "quantise undo failed");
  const auto& restored = session->snapshot().tracks.front().midi_clips.front().notes;
  REQUIRE_EQ(restored[0].start.value, 120'000'000, "one undo did not restore every note start");
  REQUIRE_EQ(restored[1].start.value, 900'000'000, "one undo did not restore every note start");
  REQUIRE_OK(session->history().redo(), "quantised notes could not be redone and saved");
  REQUIRE_OK(session->saving().save(), "quantised notes could not be redone and saved");
  session.reset();
  auto reopened = ProjectSession::open(scratch.root / "project");
  REQUIRE_OK(reopened, "quantised project could not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().midi_clips.front().notes[1].start.value,
             960'000'000, "quantised note did not survive save and reopen");
  session = std::move(reopened).value();
  const auto note_id = session->snapshot().tracks.front().midi_clips.front().notes[1].id;
  const auto before_resize = session->snapshot().revision;
  REQUIRE_OK(session->notes().resize_note(clip, note_id, {1'680'000'000}),
             "note resize did not preserve its ID and commit one revision");
  REQUIRE_EQ(session->snapshot().revision, before_resize + 1,
             "note resize did not preserve its ID and commit one revision");
  REQUIRE_EQ(session->snapshot().tracks.front().midi_clips.front().notes[1].end.value,
             1'680'000'000, "note resize did not preserve its ID and commit one revision");
  REQUIRE_OK(session->history().undo(), "one Undo did not restore note duration");
  REQUIRE_EQ(session->snapshot().tracks.front().midi_clips.front().notes[1].end.value,
             1'440'000'000, "one Undo did not restore note duration");
  require(!session->notes().resize_note(clip, note_id, {900'000'000}).has_value(),
          "note resize accepted an end before the start");
  REQUIRE_OK(session->notes().add_note_with_duration(clip, {240'000'000}, {120'000'000}, 60),
             "collision fixture note was not added");
  const auto before_collision = session->snapshot().revision;
  REQUIRE_TRUE(!session->midi_clips().quantize_midi_clip(clip, 4).has_value(),
               "colliding quantise partially changed the project");
  REQUIRE_EQ(session->snapshot().revision, before_collision,
             "colliding quantise partially changed the project");
  REQUIRE_EQ(session->snapshot().tracks.front().midi_clips.front().notes[2].start.value,
             240'000'000, "colliding quantise partially changed the project");
  require(!session->midi_clips().quantize_midi_clip(clip, 7).has_value(),
          "unsupported MIDI grid accepted");
  for (const auto steps : {12, 256})
    require(!session->midi_clips().quantize_midi_clip(clip, steps).has_value() &&
                session->snapshot().revision == before_collision,
            "unsupported MIDI grid accepted");
  // The finest grid: a note a third of a 128th past line 67 snaps back to it,
  // in one revision; the notes already on 128th lines stay.
  const auto finest = oscillate::application::midi_grid_steps_per_bar.back();
  const auto fine = oscillate::application::midi_grid_ticks(finest);
  REQUIRE_OK(session->notes().add_note_with_duration(clip, {67 * fine + fine / 3}, {fine}, 64),
             "fine-grid fixture note was not added");
  const auto before_fine = session->snapshot().revision;
  REQUIRE_OK(session->midi_clips().quantize_midi_clip(clip, finest), "1/128 quantise was refused");
  REQUIRE_EQ(session->snapshot().revision, before_fine + 1, "1/128 quantise was refused");
  for (const auto& note : session->snapshot().tracks.front().midi_clips.front().notes)
    require(note.start.value % fine == 0 && (note.key != 64 || note.start.value == 67 * fine),
            "a note missed the 1/128 grid");
}
} // namespace

int main() {
  try {
    quantise_preserves_notes_and_is_one_undo_step();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
