#include "oscillate/application/project_session.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

// Trimming a MIDI clip from its left edge (ADR-0096): the start moves, the
// end stays, and every note and pedal point keeps its timeline place, so the
// notes before the new start are kept but silent and dragging the start back
// brings them back. Each trim is one undo step and survives save and reopen.
namespace {
using namespace oscillate;
using oscillate::test::require;
using oscillate::test::take;

constexpr std::int64_t beat = timeline::ticks_per_quarter;
constexpr std::int64_t bar = 4 * beat;
// A two-bar clip from bar 1 with a note on its first beat and one in its
// second bar; the trim takes off the first bar.
constexpr std::int64_t clip_end = 2 * bar;
constexpr std::int64_t trimmed_start = bar;
constexpr std::int64_t late_note = bar + beat;
constexpr std::uint8_t early_key = 60;
constexpr std::uint8_t late_key = 64;
// The sustain pedal is down from the clip's start to here, across the trim.
constexpr std::int64_t sustain_up = bar + 2 * beat;
constexpr double pedal_down = 1.0;

void accepted(foundation::Result<void> result) {
  if (!result)
    throw std::runtime_error(result.error().message);
}

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-midi-trim") {}
};

struct Song {
  std::unique_ptr<application::ProjectSession> session;
  project::ClipId clip;
};

Song two_bar_clip(const Directory& directory) {
  auto session = take(application::ProjectSession::create(directory.path() / "song"));
  const auto track = take(session->instruments().add_synth_track());
  accepted(session->midi_clips().add_midi_clip(track, {0}));
  const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
  accepted(session->midi_clips().resize_midi_clip(clip, {clip_end}));
  accepted(session->notes().add_note(clip, {0}, early_key));
  accepted(session->notes().add_note(clip, {late_note}, late_key));
  return {std::move(session), clip};
}

const project::MidiClip& midi_clip(const application::ProjectSession& session, project::ClipId id) {
  for (const auto& track : session.snapshot().tracks) {
    const auto found = std::ranges::find(track.midi_clips, id, &project::MidiClip::id);
    if (found != track.midi_clips.end())
      return *found;
  }
  throw std::runtime_error("MIDI clip missing");
}

const project::MidiNote& note_with_key(const project::MidiClip& clip, std::uint8_t key) {
  const auto found = std::ranges::find(clip.notes, key, &project::MidiNote::key);
  REQUIRE_NE(found, clip.notes.end(), "a note is missing");
  return *found;
}

project::MusicalInterval placement(const project::MidiClip& clip) {
  const auto* musical = std::get_if<project::MusicalInterval>(&clip.placement);
  REQUIRE_TRUE(musical != nullptr, "the clip left musical time");
  return *musical;
}

std::int64_t sample_at(const application::ProjectSession& session, std::int64_t tick) {
  const auto& snapshot = session.snapshot();
  return take(snapshot.tempo.tick_to_sample({tick}, snapshot.sample_rate)).value;
}

struct NoteOn {
  std::int64_t sample;
  std::uint8_t key;
  bool operator==(const NoteOn&) const = default;
};

// What playback plays: every note-on, as the shared compiler schedules it.
std::vector<NoteOn> note_ons(const application::ProjectSession& session) {
  const auto plan = take(engine::compile_midi_events(session.snapshot()));
  std::vector<NoteOn> played;
  for (const auto& instrument : plan.instruments()) {
    for (const auto& event : instrument.events()) {
      if (event.kind == engine::MidiEventKind::note_on)
        played.push_back({event.sample.value, event.key});
    }
  }
  return played;
}

void left_trim_hides_and_reveals_notes() {
  Directory directory;
  auto [session, clip] = two_bar_clip(directory);
  const auto original = midi_clip(*session, clip);
  const std::vector<NoteOn> both{{sample_at(*session, 0), early_key},
                                 {sample_at(*session, late_note), late_key}};
  REQUIRE_EQ(note_ons(*session), both, "the untrimmed clip plays both notes");

  const auto revision = session->snapshot().revision;
  accepted(session->midi_clips().trim_midi_clip_start(clip, {trimmed_start}));
  REQUIRE_EQ(session->snapshot().revision, revision + 1, "a trim is one edit");
  const auto& trimmed = midi_clip(*session, clip);
  REQUIRE_EQ(placement(trimmed).start.value, trimmed_start, "the start moved to the trim");
  REQUIRE_EQ(placement(trimmed).end.value, clip_end, "the end stayed");
  REQUIRE_EQ(trimmed.pattern_length.value, clip_end - trimmed_start, "the pattern ends at the end");
  REQUIRE_EQ(note_with_key(trimmed, early_key).start.value, -trimmed_start,
             "the note before the new start is kept before tick 0");
  REQUIRE_EQ(note_with_key(trimmed, late_key).start.value, late_note - trimmed_start,
             "the later note counts from the new start");
  const std::vector<NoteOn> late_only{{sample_at(*session, late_note), late_key}};
  REQUIRE_EQ(note_ons(*session), late_only,
             "the trimmed note is silent and the other plays where it did");

  // The trim survives save and reopen with its hidden note.
  accepted(session->saving().save());
  const auto saved = midi_clip(*session, clip);
  session.reset();
  auto reopened = take(application::ProjectSession::open(directory.path() / "song"));
  REQUIRE_TRUE(midi_clip(*reopened, clip) == saved, "the trimmed clip reopens as saved");
  REQUIRE_EQ(note_ons(*reopened), late_only, "the reopened clip plays as before");

  // Dragging the start back reveals the note where it was.
  accepted(reopened->midi_clips().trim_midi_clip_start(clip, {0}));
  const auto& revealed = midi_clip(*reopened, clip);
  REQUIRE_TRUE(revealed.notes == original.notes, "the notes are back at their places");
  REQUIRE_EQ(revealed.pattern_length, original.pattern_length, "the pattern is whole again");
  REQUIRE_TRUE(revealed.placement == original.placement, "the clip spans its first bars again");
  REQUIRE_EQ(note_ons(*reopened), both, "both notes play again");
}

void undo_restores_the_trim() {
  Directory directory;
  auto [session, clip] = two_bar_clip(directory);
  const auto original = midi_clip(*session, clip);
  accepted(session->midi_clips().trim_midi_clip_start(clip, {trimmed_start}));
  accepted(session->history().undo());
  REQUIRE_TRUE(midi_clip(*session, clip) == original, "one undo restores the untrimmed clip");
  accepted(session->history().redo());
  REQUIRE_EQ(placement(midi_clip(*session, clip)).start.value, trimmed_start, "redo trims again");
}

void trimmed_sustain_holds_from_the_new_start() {
  Directory directory;
  auto [session, clip] = two_bar_clip(directory);
  accepted(session->pedals().paint_pedal_span(clip, project::MidiPedal::sustain, {0}, {sustain_up},
                                              pedal_down));
  accepted(session->midi_clips().trim_midi_clip_start(clip, {trimmed_start}));
  const auto& lane = midi_clip(*session, clip).pedals.front();
  REQUIRE_LT(lane.points.front().position.value, 0, "the pedal's press is kept before tick 0");
  const auto plan = take(engine::compile_midi_events(session->snapshot()));
  const auto& pedals = plan.instruments().front().pedals();
  REQUIRE_FALSE(pedals.empty(), "the trimmed clip still pedals");
  REQUIRE_EQ(pedals.front().sample.value, sample_at(*session, trimmed_start),
             "the held pedal is down from the new start");
  REQUIRE_EQ(pedals.front().value, pedal_down, "the held pedal is down from the new start");
  REQUIRE_TRUE(std::ranges::any_of(pedals,
                                   [&](const engine::ScheduledPedalEvent& event) {
                                     return event.sample.value == sample_at(*session, sustain_up) &&
                                            event.value == 0.0;
                                   }),
               "the pedal still lifts where it did");
}

void refused_trims_change_nothing() {
  Directory directory;
  auto [session, clip] = two_bar_clip(directory);
  const auto revision = session->snapshot().revision;
  REQUIRE_FALSE(session->midi_clips().trim_midi_clip_start(clip, {clip_end}),
                "a start at the end is refused");
  REQUIRE_FALSE(session->midi_clips().trim_midi_clip_start(clip, {-bar}),
                "a start before bar 1 is refused");
  // A looping one-bar pattern over two bars: its start stays inside the
  // first pattern.
  accepted(session->midi_clips().resize_midi_clip(clip, {bar}));
  accepted(session->midi_clips().set_midi_clip_loop(clip, true));
  accepted(session->midi_clips().resize_midi_clip(clip, {clip_end}));
  const auto looping = session->snapshot().revision;
  REQUIRE_FALSE(session->midi_clips().trim_midi_clip_start(clip, {late_note}),
                "a looping clip cannot start past its first pattern");
  REQUIRE_EQ(session->snapshot().revision, looping, "refused trims make no edit");
  REQUIRE_GT(looping, revision, "the loop fixture edited the clip");
}

} // namespace

int main() {
  try {
    left_trim_hides_and_reveals_notes();
    undo_restores_the_trim();
    trimmed_sustain_holds_from_the_new_start();
    refused_trims_change_nothing();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "MIDI clip trim contract passed\n";
  return 0;
}
