// Comp in the project (ADR-0083, ADR-0084): a pattern sets the rhythm and
// keeps the musician's settings; a rhythm applied to a clip's chords writes
// exactly what a Comp on the track plays over them, as one undo step; note
// rewrites in one gesture are one undo step.
//   oscillate_application_comp_session_tests [case...]
#include "oscillate/application/comp_patterns.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/native_dsp/note_effects/note_effect.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_inserts/tempo_ticks.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
namespace app = oscillate::application;
namespace comp = oscillate::application::comp;
namespace engine = oscillate::engine;
namespace foundation = oscillate::foundation;
namespace fx = oscillate::native_dsp::note_effects;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace fs = std::filesystem;

constexpr std::int64_t bar = 4 * timeline::ticks_per_quarter;
constexpr std::uint32_t block = 256;

using oscillate::test::require;

using oscillate::test::take;

void accepted(const foundation::Result<void>& result) {
  if (!result)
    throw std::runtime_error(result.error().message);
}

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-comp") {}
};

template <typename Id> Id make_id(std::uint64_t value) { return take(Id::from_bits({0, value})); }

// Chords as a keyboard player holds them (the bass first), a bar each.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization): a throw fails the test at start-up.
const std::vector<std::vector<int>> progression{
    {45, 55, 59, 60, 64}, // Am9
    {50, 57, 60, 64, 65}, // Dm9
    {41, 55, 57, 60, 64}, // Fmaj9
    {40, 55, 59, 62, 67}, // Em7
};

std::vector<project::MidiNote> chord_notes() {
  std::vector<project::MidiNote> notes;
  std::uint64_t id = 1;
  for (std::size_t index = 0; index < progression.size(); ++index) {
    const auto at = static_cast<std::int64_t>(index) * bar;
    for (const int key : progression[index])
      notes.push_back(
          {make_id<project::NoteId>(id++), {at}, {at + bar}, static_cast<std::uint8_t>(key), 90});
  }
  return notes;
}

// A synth track with a Comp and a four-bar clip of chords from bar 2, so
// the clip's tick 0 is not the song's.
struct Song {
  Directory directory;
  std::unique_ptr<app::ProjectSession> session =
      take(app::ProjectSession::create(directory.path() / "song"));
  project::TrackId track = take(session->instruments().add_synth_track());
  project::InsertSlotId slot = take(session->track_devices().add_note_effect(track, 0, "comp"));
  project::ClipId clip =
      take(session->midi_clips().add_midi_clip_with_notes(track, {bar}, {4 * bar}, chord_notes()));
  [[nodiscard]] const project::Track& owner() const {
    return *std::ranges::find(session->snapshot().tracks, track, &project::Track::id);
  }
  [[nodiscard]] const project::NoteEffectInsert& insert() const {
    return *std::ranges::find(owner().note_effects, slot, &project::NoteEffectInsert::id);
  }
  [[nodiscard]] const project::MidiClip& midi() const {
    return *std::ranges::find(owner().midi_clips, clip, &project::MidiClip::id);
  }
  [[nodiscard]] double value(std::string_view id) const {
    const auto resolved = take(fx::resolve_note_effect(insert()));
    return resolved
        .values[take(oscillate::native_dsp::find_parameter(*resolved.engine->descriptor, id))];
  }
};

std::size_t pattern_index(std::string_view name) {
  return static_cast<std::size_t>(
      std::ranges::find(comp::patterns(), name, &comp::CompPattern::name) -
      comp::patterns().begin());
}

// Techno stabs over a Comp set to velocity 90 an octave up: the lanes, push
// and groove change, the velocity and octave stay. A pattern that isn't
// there is refused. With no Comp yet, a pattern is the defaults plus it.
void a_pattern_keeps_the_musicians_settings() {
  Song song;
  accepted(song.session->track_devices().set_note_effect_parameters(
      song.track, song.slot, {{"comp.velocity", 90.0}, {"comp.octave", 1.0}}));
  const auto stabs = pattern_index("Techno stabs");
  REQUIRE_EQ(comp::patterns().size(), 10, "ten patterns");
  const auto steps = song.session->history().undo_steps().size();
  accepted(song.session->track_devices().apply_comp_pattern(song.track, song.slot, stabs));
  REQUIRE_EQ(song.session->history().undo_steps().size(), steps + 1, "a pattern is one undo step");
  REQUIRE_EQ(song.value("comp.mid.7"), 2.0, "the stabs' lanes are in: the and of 2 and 4, no bass");
  REQUIRE_EQ(song.value("comp.mid.15"), 2.0,
             "the stabs' lanes are in: the and of 2 and 4, no bass");
  REQUIRE_EQ(song.value("comp.mid.1"), 0.0, "the stabs' lanes are in: the and of 2 and 4, no bass");
  REQUIRE_EQ(song.value("comp.low.1"), 0.0, "the stabs' lanes are in: the and of 2 and 4, no bass");
  REQUIRE_EQ(song.value("comp.groove"), 0.0, "with its groove and push");
  REQUIRE_EQ(song.value("comp.push_amount"), 0.0, "with its groove and push");
  REQUIRE_EQ(song.value("comp.velocity"), 90.0, "and the velocity and octave are kept");
  REQUIRE_EQ(song.value("comp.octave"), 1.0, "and the velocity and octave are kept");
  require(!song.session->track_devices().apply_comp_pattern(song.track, song.slot,
                                                            comp::patterns().size()),
          "a pattern that isn't there is refused");
  const auto fresh = take(comp::pattern_parameters(stabs, {}));
  REQUIRE_NE(
      std::ranges::find(fresh, std::string("comp.mid.7"), &project::NativeInstrumentParameter::id),
      fresh.end(), "with no Comp yet, the pattern over the defaults");
  REQUIRE_TRUE(std::ranges::none_of(fresh,
                                    [](const project::NativeInstrumentParameter& parameter) {
                                      return parameter.id == "comp.velocity";
                                    }),
               "with no Comp yet, the pattern over the defaults");
}

// What Comp applies to the clip is what a Comp on the track plays live over
// the same notes, played from the clip's start, on exactly the same frames,
// pushes and humanise included: the live chain here is fed as the player
// feeds it, from the compiled MIDI plan with its lookahead.
void applied_rhythm_plays_what_comp_played() {
  Song song;
  accepted(song.session->track_devices().set_note_effect_parameters(
      song.track, song.slot,
      {{"comp.push_chance", 0.5}, {"comp.humanise", 0.6}, {"comp.seed", 11.0}}));
  const auto& state = song.session->snapshot();
  const auto held = song.midi().notes;
  const auto notes = take(song.session->notes().comp_rhythm_notes(
      song.clip, song.insert().parameters, held, {0}, {4 * bar}));
  require(!notes.empty(), "Comp plays the chords");
  using Event = std::tuple<std::int64_t, bool, int>; // (frame, on, key)
  const auto frame_of = [&](std::int64_t tick) {
    return take(state.tempo.tick_to_sample({tick}, state.sample_rate)).value;
  };
  std::multiset<Event> applied;
  for (const auto& note : notes) {
    REQUIRE_GE(note.start.value, 0, "every note is inside the range");
    REQUIRE_LE(note.end.value, 4 * bar, "every note is inside the range");
    REQUIRE_LT(note.start, note.end, "every note is inside the range");
    applied.insert({frame_of(bar + note.start.value), true, note.key});
    applied.insert({frame_of(bar + note.end.value), false, note.key});
  }
  // Live: the same Comp in a chain, block by block, from the clip's start.
  const auto plan = take(engine::compile_midi_events(state));
  const auto& events = plan.instruments().front();
  auto chain = take(fx::NoteEffectChain::create(song.owner().note_effects));
  const auto resolved = take(fx::resolve_note_effect(song.insert()));
  const fx::StageControls control{true, resolved.values, {}};
  engine::HeldNotes input;
  std::vector<engine::BlockMidiEvent> buffer(fx::block_event_capacity);
  std::vector<engine::BlockMidiEvent> scratch(fx::block_event_capacity);
  std::vector<fx::AheadEvent> ahead(fx::ahead_event_capacity);
  const auto first = frame_of(bar);
  const auto end = frame_of(5 * bar);
  std::multiset<Event> live;
  std::multiset<std::tuple<std::uint64_t, int>> open;
  const oscillate::native_inserts::TempoTicks ticks(state.tempo, state.sample_rate);
  for (std::int64_t frame = first; frame < end + end / 4; frame += block) {
    const fx::NoteClock clock(ticks, static_cast<std::uint64_t>(frame), block);
    const auto copied = input.copy_block(
        events, {frame}, block, 1, std::span(buffer).first(engine::HeldNotes::block_capacity));
    REQUIRE_EQ(copied.status, engine::BlockMidiStatus::ready, "the plan's notes copy");
    const auto seen = fx::copy_ahead(events.events(), clock, ahead);
    for (const auto& event : chain->process(buffer, copied.count, scratch, clock,
                                            std::span(&control, 1), std::span(ahead).first(seen))) {
      const auto at = frame + event.offset;
      const bool on = event.kind == engine::MidiEventKind::note_on;
      if (on && at >= first && at < end) {
        live.insert({at, true, event.key});
        open.insert({event.note_id_bits.low, event.key});
      } else if (!on && open.erase({event.note_id_bits.low, event.key}) == 1) {
        live.insert({std::min(at, end), false, event.key});
      }
    }
  }
  REQUIRE_EQ(applied, live, "the clip plays exactly what Comp played");
  // A pushed change: a following chord's bass in the last 16ths of a bar.
  const auto pushed = std::ranges::any_of(notes, [](const project::MidiNote& note) {
    const auto within = note.start.value % bar;
    return within > 3 * bar / 4 && (note.key == 50 || note.key == 41 || note.key == 40);
  });
  require(pushed, "a change is pushed in early");
}

// Apply rhythm is one undo step: the notes starting in the range become
// Comp's, a note held into the range is cut where it starts, the rest stay.
// Undo puts the chords back. Applying past the pattern is refused; a
// preview past it is not.
void apply_comp_rhythm_is_one_undo_step() {
  Song song;
  // A drone under bars 1-2 of the clip, held across the range's start.
  auto chords = chord_notes();
  chords.push_back({make_id<project::NoteId>(100), {0}, {2 * bar}, 33, 80});
  accepted(song.session->midi_clips().replace_midi_clip_notes(song.clip, {4 * bar}, chords));
  const auto before = song.midi().notes;
  const auto held = chord_notes();
  const auto parameters = take(comp::pattern_parameters(pattern_index("Anjuna offbeats"), {}));
  const auto wanted =
      take(song.session->notes().comp_rhythm_notes(song.clip, parameters, held, {bar}, {3 * bar}));
  REQUIRE_EQ(song.midi().notes, before, "a preview changes nothing");
  const auto steps = song.session->history().undo_steps().size();
  accepted(song.session->notes().apply_comp_rhythm(song.clip, parameters, held, {bar}, {3 * bar}));
  REQUIRE_EQ(song.session->history().undo_steps().size(), steps + 1, "one undo step");
  using Shape = std::tuple<std::int64_t, std::int64_t, int, int>;
  std::multiset<Shape> inside;
  std::multiset<Shape> outside;
  std::set<project::NoteId> ids;
  for (const auto& note : song.midi().notes) {
    ids.insert(note.id);
    const Shape shape{note.start.value, note.end.value, note.key, note.velocity};
    (note.start.value >= bar && note.start.value < 3 * bar ? inside : outside).insert(shape);
  }
  REQUIRE_EQ(ids.size(), song.midi().notes.size(), "every note has its own id");
  std::multiset<Shape> expected_inside;
  for (const auto& note : wanted)
    expected_inside.insert({note.start.value, note.end.value, note.key, note.velocity});
  REQUIRE_EQ(inside, expected_inside, "the range holds Comp's notes");
  std::multiset<Shape> expected_outside;
  for (const auto& note : before) {
    if (note.start.value >= bar && note.start.value < 3 * bar)
      continue;
    expected_outside.insert(
        {note.start.value, std::min(note.end.value, note.start.value < bar ? bar : note.end.value),
         note.key, note.velocity});
  }
  REQUIRE_EQ(outside, expected_outside, "the chords outside stay, the drone cut where Comp starts");
  accepted(song.session->history().undo());
  REQUIRE_EQ(song.midi().notes, before, "undo puts the chords back");
  require(
      !song.session->notes().apply_comp_rhythm(song.clip, parameters, held, {3 * bar}, {5 * bar}),
      "a range past the pattern is refused");
  require(static_cast<bool>(song.session->notes().comp_rhythm_notes(song.clip, parameters, held,
                                                                    {3 * bar}, {5 * bar})),
          "but previews past it, for a caller lengthening the clip");
  require(!song.session->notes().comp_rhythm_notes(song.clip, parameters, held, {bar}, {bar}),
          "an empty range is refused");
}

// Rewrites of a clip's notes with one gesture (a mood-pad drag) coalesce
// into one undo step; without a gesture, each is its own.
void a_gesture_coalesces_note_rewrites() {
  Song song;
  auto notes = chord_notes();
  const auto steps = song.session->history().undo_steps().size();
  constexpr std::uint64_t drag = 41;
  for (int pass = 0; pass < 3; ++pass) {
    for (auto& note : notes)
      note.key = static_cast<std::uint8_t>(note.key + 1);
    accepted(song.session->midi_clips().replace_midi_clip_notes(song.clip, {4 * bar}, notes, drag));
  }
  REQUIRE_EQ(song.session->history().undo_steps().size(), steps + 1, "one drag, one undo step");
  REQUIRE_EQ(song.midi().notes.front().key, progression.front().front() + 3,
             "the last rewrite stands");
  accepted(song.session->history().undo());
  REQUIRE_EQ(song.midi().notes.front().key, progression.front().front(), "undo restores the start");
  accepted(song.session->history().redo());
  accepted(song.session->midi_clips().replace_midi_clip_notes(song.clip, {4 * bar}, chord_notes()));
  REQUIRE_EQ(song.session->history().undo_steps().size(), steps + 2,
             "a rewrite without a gesture is its own step");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "Comp session contract passed",
      {{"a_pattern_keeps_the_musicians_settings", a_pattern_keeps_the_musicians_settings},
       {"applied_rhythm_plays_what_comp_played", applied_rhythm_plays_what_comp_played},
       {"apply_comp_rhythm_is_one_undo_step", apply_comp_rhythm_is_one_undo_step},
       {"a_gesture_coalesces_note_rewrites", a_gesture_coalesces_note_rewrites}});
}
