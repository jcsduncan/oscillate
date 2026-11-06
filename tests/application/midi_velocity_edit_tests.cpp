#include "oscillate/application/midi_velocity_edit.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

// The velocity lane's rules: a line across stems, humanise and randomise
// within limits and reproducibly from a seed, scope, and the session's one
// undo step that keeps timing and identity, survives save/reopen, and
// reaches the compiled MIDI plan playback and export share.
namespace {
namespace fs = std::filesystem;
using namespace oscillate;
using application::VelocityChange;
using project::MidiNote;
using project::NoteId;

constexpr std::int64_t q = timeline::ticks_per_quarter;

using oscillate::test::require;

template <typename Id> Id id(std::uint64_t value) { return Id::from_bits({0, value}).value(); }

std::vector<MidiNote> row(int count, std::uint8_t velocity) {
  std::vector<MidiNote> notes;
  for (int i = 0; i < count; ++i) {
    notes.push_back({id<NoteId>(static_cast<std::uint64_t>(i + 1)),
                     {i * q / 4},
                     {i * q / 4 + q / 8},
                     static_cast<std::uint8_t>(60 + i % 12),
                     velocity});
  }
  return notes;
}

std::vector<NoteId> ids(const std::vector<MidiNote>& notes) {
  std::vector<NoteId> out;
  for (const auto& note : notes)
    out.push_back(note.id);
  return out;
}

void lines_ramp_and_respect_scope() {
  const auto notes = row(5, 64);
  const auto all = ids(notes);
  const auto ramp = application::velocity_line(notes, all, 0, 20.0, q, 120.0);
  REQUIRE_EQ(ramp.size(), 5, "a line sets each stem to its value at the note's start");
  REQUIRE_EQ(ramp[0].second, 20, "a line sets each stem to its value at the note's start");
  REQUIRE_EQ(ramp[2].second, 70, "a line sets each stem to its value at the note's start");
  REQUIRE_EQ(ramp[4].second, 120, "a line sets each stem to its value at the note's start");
  const auto reversed = application::velocity_line(notes, all, q, 120.0, 0, 20.0);
  REQUIRE_EQ(reversed, ramp, "a line drawn right to left is the same line");
  const std::vector<NoteId> one{notes[2].id};
  const auto scoped = application::velocity_line(notes, one, 0, 20.0, q, 120.0);
  REQUIRE_EQ(scoped.size(), 1, "only notes in scope change");
  REQUIRE_EQ(scoped[0].first, notes[2].id, "only notes in scope change");
  REQUIRE_EQ(application::velocity_line(notes, all, 0, -50.0, q, 400.0).front().second, 1,
             "velocities stay within 1..127");
  REQUIRE_EQ(application::velocity_line(notes, all, 0, -50.0, q, 400.0).back().second, 127,
             "velocities stay within 1..127");
}

void humanise_is_gentle_centred_and_reproducible() {
  auto notes = row(400, 64);
  for (std::size_t i = 0; i < notes.size(); i += 4)
    notes[i].velocity = 120; // Accents.
  const auto all = ids(notes);
  const auto a = application::humanize_velocities(notes, all, 1.0, 42);
  const auto b = application::humanize_velocities(notes, all, 1.0, 42);
  const auto c = application::humanize_velocities(notes, all, 1.0, 43);
  REQUIRE_EQ(a, b, "a seed reproduces a variation; another seed rerolls");
  REQUIRE_NE(a, c, "a seed reproduces a variation; another seed rerolls");
  double offset_sum = 0.0;
  double accent_min = 127.0;
  double plain_max = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const double offset = static_cast<double>(a[i].second) - notes[i].velocity;
    REQUIRE_LE(std::fabs(offset), 16.0, "humanise moves a note by at most 16");
    offset_sum += offset;
    if (notes[i].velocity == 120)
      accent_min = std::min(accent_min, static_cast<double>(a[i].second));
    else
      plain_max = std::max(plain_max, static_cast<double>(a[i].second));
  }
  REQUIRE_LT(std::fabs(offset_sum / static_cast<double>(a.size())), 1.5,
             "the variation is centred: dynamics do not drift");
  REQUIRE_GT(accent_min, plain_max, "accents stay accents");
  for (const auto& change : application::humanize_velocities(notes, all, 0.0, 7))
    REQUIRE_EQ(change.second, std::ranges::find(notes, change.first, &MidiNote::id)->velocity,
               "strength 0 changes nothing");
}

void randomise_spans_its_range() {
  const auto notes = row(500, 64);
  const auto all = ids(notes);
  const auto random = application::randomize_velocities(notes, all, 30, 90, 9);
  int lowest = 127;
  int highest = 0;
  for (const auto& change : random) {
    lowest = std::min<int>(lowest, change.second);
    highest = std::max<int>(highest, change.second);
  }
  REQUIRE_EQ(lowest, 30, "randomise covers exactly [minimum, maximum]");
  REQUIRE_EQ(highest, 90, "randomise covers exactly [minimum, maximum]");
  REQUIRE_EQ(application::randomize_velocities(notes, all, 90, 30, 9), random,
             "a swapped range is the same range");
}

struct Scratch : public oscillate::test::TempDir {
  Scratch() : TempDir("oscillate-velocity") {}
  const std::filesystem::path root{path()};
};

void session_edits_are_one_step_and_reach_playback() {
  Scratch scratch;
  auto session = std::move(application::ProjectSession::create(scratch.root / "p")).value();
  const auto track = session->instruments().add_synth_track().value();
  REQUIRE_OK(session->midi_clips().add_midi_clip(track, {0}), "MIDI clip");
  const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
  session->notes().set_new_note_velocity(37);
  REQUIRE_OK(session->notes().add_note_with_duration(clip, {0}, {q}, 60), "notes");
  REQUIRE_OK(session->notes().add_note_with_duration(clip, {q}, {q}, 64), "notes");
  const auto notes = [&]() { return session->snapshot().tracks.front().midi_clips.front().notes; };
  REQUIRE_EQ(notes()[0].velocity, 37, "new notes take the last velocity set in the lane");
  REQUIRE_EQ(notes()[1].velocity, 37, "new notes take the last velocity set in the lane");
  const auto before = notes();
  const auto revision = session->snapshot().revision;
  const std::vector<VelocityChange> changes{{before[0].id, 20}, {before[1].id, 110}};
  REQUIRE_OK(session->notes().set_note_velocities(clip, changes),
             "several velocities change in one revision");
  REQUIRE_EQ(session->snapshot().revision, revision + 1,
             "several velocities change in one revision");
  REQUIRE_EQ(notes()[0].id, before[0].id, "velocity changes keep every note's identity and timing");
  REQUIRE_EQ(notes()[0].start, before[0].start,
             "velocity changes keep every note's identity and timing");
  REQUIRE_EQ(notes()[0].end, before[0].end,
             "velocity changes keep every note's identity and timing");
  REQUIRE_EQ(notes()[0].key, before[0].key,
             "velocity changes keep every note's identity and timing");
  REQUIRE_EQ(notes()[0].velocity, 20, "velocity changes keep every note's identity and timing");
  REQUIRE_EQ(notes()[1].velocity, 110, "velocity changes keep every note's identity and timing");
  REQUIRE_TRUE(!session->notes()
                    .set_note_velocities(clip, std::vector<VelocityChange>{{before[0].id, 0}})
                    .has_value(),
               "out-of-range velocities and unknown notes are rejected");
  REQUIRE_TRUE(!session->notes()
                    .set_note_velocities(clip, std::vector<VelocityChange>{{id<NoteId>(999), 5}})
                    .has_value(),
               "out-of-range velocities and unknown notes are rejected");
  const auto plan = engine::compile_midi_events(session->snapshot()).value();
  const auto& events = plan.instruments().front().events();
  require(std::ranges::any_of(events,
                              [](const auto& e) {
                                return e.kind == engine::MidiEventKind::note_on &&
                                       e.velocity == 110;
                              }),
          "the compiled plan, shared by playback and export, plays the new velocity");
  REQUIRE_OK(session->history().undo(), "one undo restores both");
  REQUIRE_EQ(notes(), before, "one undo restores both");
  REQUIRE_OK(session->history().redo(), "redo and save");
  REQUIRE_OK(session->saving().save(), "redo and save");
  session.reset();
  auto reopened = std::move(application::ProjectSession::open(scratch.root / "p")).value();
  const auto& kept = reopened->snapshot().tracks.front().midi_clips.front().notes;
  REQUIRE_EQ(kept[0].velocity, 20, "velocities survive save and reopen");
  REQUIRE_EQ(kept[1].velocity, 110, "velocities survive save and reopen");
}

} // namespace

int main() {
  try {
    lines_ramp_and_respect_scope();
    humanise_is_gentle_centred_and_reproducible();
    randomise_spans_its_range();
    session_edits_are_one_step_and_reach_playback();
  } catch (const std::exception& error) {
    std::cerr << "velocity edit contract: " << error.what() << '\n';
    return 1;
  }
  std::cout << "velocity edit contract passed\n";
  return 0;
}
