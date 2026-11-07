#include "oscillate/application/midi_pedal_edit.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

// The pedal strip's editing rules (ADR-0035): spans from points, painting
// that keeps the rest of the lane, lifting, moving, legato pedalling per
// chord, sounding ends under sustain and sostenuto, and the session
// commands as single undo steps.
namespace {
namespace fs = std::filesystem;
using namespace oscillate;
using application::PedalSpan;
using project::MidiPedal;
using Points = std::vector<project::MidiPedalPoint>;

constexpr std::int64_t q = timeline::ticks_per_quarter;
constexpr std::int64_t bar = 4 * q;

using oscillate::test::require;

template <typename Id> Id id(std::uint64_t value) { return Id::from_bits({0, value}).value(); }
using project::ClipId;
using project::NoteId;

Points points(std::initializer_list<std::pair<std::int64_t, double>> list) {
  Points out;
  for (const auto& [tick, value] : list)
    out.push_back({{tick}, value});
  return out;
}

void spans_and_painting() {
  const auto lane = points({{0, 1.0}, {q, 0.0}, {2 * q, 0.5}, {3 * q, 1.0}});
  const auto spans = application::pedal_spans(lane, bar);
  REQUIRE_EQ(
      spans, (std::vector<PedalSpan>{{0, q, 1.0}, {2 * q, 3 * q, 0.5}, {3 * q, bar, 1.0}}),
      "spans follow the steps; a depth change without a lift is two adjacent spans; the last "
      "runs to the pattern's end");
  REQUIRE_EQ(application::pedal_value_at(lane, q / 2), 1.0,
             "the value holds from each point, 0 before the first");
  REQUIRE_EQ(application::pedal_value_at(lane, q), 0.0,
             "the value holds from each point, 0 before the first");
  REQUIRE_EQ(application::pedal_value_at(points({{q, 1.0}}), 0), 0.0,
             "the value holds from each point, 0 before the first");

  // Painting keeps what the lane did after the span.
  auto painted = application::paint_pedal(lane, q / 2, 5 * q / 2, 0.75, bar);
  REQUIRE_EQ(painted, points({{0, 1.0}, {q / 2, 0.75}, {5 * q / 2, 0.5}, {3 * q, 1.0}}),
             "paint replaces the inside and restores the value after it");
  painted = application::paint_pedal(points({}), q, 2 * q, 1.0, bar);
  REQUIRE_EQ(painted, points({{q, 1.0}, {2 * q, 0.0}}), "a span on an empty lane is down then up");
  REQUIRE_EQ(application::paint_pedal(painted, 3 * q, 9 * q, 1.0, bar),
             points({{q, 1.0}, {2 * q, 0.0}, {3 * q, 1.0}}),
             "a span past the pattern is clamped; the clip end releases it");
  require(application::paint_pedal(points({{q, 1.0}, {2 * q, 0.0}}), q, 2 * q, 0.0, bar).empty(),
          "painting a lift over the only span empties the lane");
  REQUIRE_EQ(application::paint_pedal(points({{0, 1.0}, {q, 0.0}}), q, 2 * q, 1.0, bar),
             points({{0, 1.0}, {2 * q, 0.0}}), "touching spans at one depth merge");

  // Points kept after the pattern's end (a shortened clip) are silent: they
  // make no spans and pass through edits untouched.
  const auto kept = points({{q, 1.0}, {bar, 0.0}, {bar + q, 1.0}});
  REQUIRE_EQ(application::pedal_spans(kept, bar), (std::vector<PedalSpan>{{q, bar, 1.0}}),
             "points after the pattern's end make no spans");
  REQUIRE_EQ(application::paint_pedal(kept, 0, bar, 0.0, bar), points({{bar, 0.0}, {bar + q, 1.0}}),
             "edits inside the pattern dropped points kept after its end");
  REQUIRE_EQ(application::remove_pedal_span(kept, {q + 10}, bar),
             points({{bar, 0.0}, {bar + q, 1.0}}),
             "edits inside the pattern dropped points kept after its end");
}

void lifting_and_chords() {
  const auto lane = points({{0, 1.0}, {q, 0.0}, {2 * q, 1.0}, {3 * q, 0.0}});
  REQUIRE_EQ(application::remove_pedal_span(lane, {2 * q + 10}, bar), points({{0, 1.0}, {q, 0.0}}),
             "lifting a span leaves the others");
  REQUIRE_EQ(application::remove_pedal_span(lane, {q + 10}, bar), lane,
             "lifting where the pedal is up changes nothing");

  // Chords at 0, 1 and 2.5 quarters (two notes share the first onset).
  const std::vector<project::MidiNote> notes{{id<NoteId>(1), {0}, {q}, 60, 100},
                                             {id<NoteId>(2), {0}, {q}, 64, 100},
                                             {id<NoteId>(3), {q}, {2 * q}, 62, 100},
                                             {id<NoteId>(4), {5 * q / 2}, {bar}, 65, 100}};
  const std::int64_t repress = q / 8;
  REQUIRE_EQ(
      application::pedal_per_chord(notes, bar, {.repress = repress}),
      points({{repress, 1.0},
              {q, 0.0},
              {q + repress, 1.0},
              {5 * q / 2, 0.0},
              {5 * q / 2 + repress, 1.0}}),
      "each chord is pedalled from just after it lands until the next lands; the last holds");
}

void sounding_ends() {
  project::MidiClip clip{id<ClipId>(1), project::MusicalInterval{{0}, {bar}}, {bar}, false, {}, 1};
  const project::MidiNote early{id<NoteId>(1), {0}, {q / 2}, 60, 100};
  const project::MidiNote late{id<NoteId>(2), {q}, {3 * q / 2}, 64, 100};
  REQUIRE_EQ(application::sounding_end(early, clip), q / 2,
             "without pedals a note ends at its end");
  clip.pedals = {{MidiPedal::sustain, points({{0, 1.0}, {3 * q, 0.0}})}};
  REQUIRE_EQ(application::sounding_end(early, clip), 3 * q,
             "released under the sustain pedal, it rings to the lift");
  clip.pedals = {{MidiPedal::sustain, points({{0, 0.3}})}};
  REQUIRE_EQ(application::sounding_end(early, clip), q / 2, "below half pedal it is not held");
  clip.pedals = {{MidiPedal::sustain, points({{q / 4, 1.0}})}};
  REQUIRE_EQ(application::sounding_end(early, clip), bar, "with no lift the clip end releases it");
  // Sostenuto goes down while `early` is held, after it starts.
  clip.pedals = {{MidiPedal::sostenuto, points({{q / 4, 1.0}, {2 * q, 0.0}})}};
  REQUIRE_EQ(application::sounding_end(early, clip), 2 * q,
             "sostenuto holds the note that was down when it went down");
  REQUIRE_EQ(application::sounding_end(late, clip), 3 * q / 2,
             "and not a note struck while it was already down");
}

struct Scratch : public oscillate::test::TempDir {
  Scratch() : TempDir("oscillate-pedal") {}
  const std::filesystem::path root{path()};
};

void session_commands_are_single_undo_steps() {
  Scratch scratch;
  auto session = std::move(application::ProjectSession::create(scratch.root / "p")).value();
  const auto track = session->instruments().add_synth_track().value();
  REQUIRE_OK(session->midi_clips().add_midi_clip(track, {0}), "MIDI clip");
  const auto clip = session->snapshot().tracks.front().midi_clips.front().id;
  REQUIRE_OK(session->notes().add_note_with_duration(clip, {0}, {q}, 60), "notes");
  REQUIRE_OK(session->notes().add_note_with_duration(clip, {2 * q}, {q}, 64), "notes");
  const auto lane = [&]() {
    const auto& pedals = session->snapshot().tracks.front().midi_clips.front().pedals;
    return pedals.empty() ? Points{} : pedals.front().points;
  };
  auto revision = session->snapshot().revision;
  REQUIRE_OK(session->pedals().paint_pedal_span(clip, MidiPedal::sustain, {0}, {q}, 1.0),
             "paint is one revision");
  REQUIRE_EQ(session->snapshot().revision, revision + 1, "paint is one revision");
  REQUIRE_EQ(lane(), points({{0, 1.0}, {q, 0.0}}), "paint is one revision");
  REQUIRE_TRUE(session->pedals()
                   .move_pedal_span(clip, MidiPedal::sustain, {q / 2}, {q / 4}, {3 * q / 2})
                   .has_value(),
               "move keeps the depth");
  REQUIRE_EQ(lane(), points({{q / 4, 1.0}, {3 * q / 2, 0.0}}), "move keeps the depth");
  require(
      !session->pedals().move_pedal_span(clip, MidiPedal::sustain, {2 * q}, {0}, {q}).has_value(),
      "moving where no span is fails");
  revision = session->snapshot().revision;
  REQUIRE_OK(session->pedals().pedal_per_chord(clip), "pedal per chord is one revision");
  REQUIRE_EQ(session->snapshot().revision, revision + 1, "pedal per chord is one revision");
  REQUIRE_EQ(lane(), points({{q / 8, 1.0}, {2 * q, 0.0}, {2 * q + q / 8, 1.0}}),
             "pedal per chord is one revision");
  REQUIRE_OK(session->pedals().remove_pedal_span(clip, MidiPedal::sustain, {q}),
             "remove lifts the span under the tick");
  REQUIRE_EQ(lane(), points({{2 * q + q / 8, 1.0}}), "remove lifts the span under the tick");
  REQUIRE_OK(session->history().undo(), "each command undoes on its own");
  REQUIRE_OK(session->history().undo(), "each command undoes on its own");
  REQUIRE_EQ(lane(), points({{q / 4, 1.0}, {3 * q / 2, 0.0}}), "each command undoes on its own");
}

} // namespace

int main() {
  try {
    spans_and_painting();
    lifting_and_chords();
    sounding_ends();
    session_commands_are_single_undo_steps();
  } catch (const std::exception& error) {
    std::cerr << "pedal edit contract: " << error.what() << '\n';
    return 1;
  }
  std::cout << "pedal edit contract passed\n";
  return 0;
}
