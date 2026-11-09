#include "bow_test_library.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/native_dsp/pedal_latch.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/prism.hpp"
#include "oscillate/native_inserts/synth_player.hpp"
#include "oscillate/test/check.hpp"
#include "synth_test_support.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

// Pedals (ADR-0035): the generic latch's semantics, then the runtime
// applying them around every engine that does not model its own.
namespace {
using namespace synth_test;
using oscillate::native_dsp::PedalLatch;
using oscillate::project::MidiPedal;

PedalLatch::Note held(std::uint64_t id) { return {{1, 1}, {0, id}, 60}; }

void sustain_defers_and_flushes() {
  PedalLatch latch;
  std::vector<std::uint64_t> released;
  const auto record = [&](const PedalLatch::Note& n) { released.push_back(n.note.low); };
  (void)latch.note_on(held(1), 100, record);
  require(latch.note_off(held(1)), "with no pedal a note-off is delivered");
  (void)latch.note_on(held(2), 100, record);
  latch.pedal(MidiPedal::sustain, 0.49, record);
  require(latch.note_off(held(2)), "below half a pedal is up");
  (void)latch.note_on(held(3), 100, record);
  latch.pedal(MidiPedal::sustain, 0.5, record);
  REQUIRE_TRUE(!latch.note_off(held(3)), "half or more holds the note");
  REQUIRE_TRUE(latch.sustained(), "half or more holds the note");
  (void)latch.note_on(held(4), 100, record);
  require(!latch.note_off(held(4)), "notes struck under the pedal are held too");
  latch.pedal(MidiPedal::sustain, 0.0, record);
  REQUIRE_EQ(released, (std::vector<std::uint64_t>{3, 4}),
             "lifting releases every held note in order");
  REQUIRE_TRUE(!latch.sustained(), "lifting releases every held note in order");
}

void sostenuto_holds_only_captured_notes() {
  PedalLatch latch;
  std::vector<std::uint64_t> released;
  const auto record = [&](const PedalLatch::Note& n) { released.push_back(n.note.low); };
  (void)latch.note_on(held(1), 100, record);
  latch.pedal(MidiPedal::sostenuto, 1.0, record);
  (void)latch.note_on(held(2), 100, record);
  require(latch.note_off(held(2)), "a note struck after sostenuto is not held");
  require(!latch.note_off(held(1)), "a note held when sostenuto went down is held");
  // Sustain and sostenuto together: the sustain lift keeps the captured note.
  latch.pedal(MidiPedal::sustain, 1.0, record);
  (void)latch.note_on(held(3), 100, record);
  require(!latch.note_off(held(3)), "sustain holds a later note");
  latch.pedal(MidiPedal::sustain, 0.0, record);
  REQUIRE_EQ(released, std::vector<std::uint64_t>{3}, "the sustain lift frees only its own note");
  latch.pedal(MidiPedal::sostenuto, 0.0, record);
  REQUIRE_EQ(released, (std::vector<std::uint64_t>{3, 1}), "the sostenuto lift frees the rest");
}

void soft_restrike_and_capacity() {
  PedalLatch latch;
  std::vector<std::uint64_t> released;
  const auto record = [&](const PedalLatch::Note& n) { released.push_back(n.note.low); };
  latch.pedal(MidiPedal::soft, 1.0, record);
  REQUIRE_EQ(latch.note_on(held(1), 100, record), 75,
             "soft scales velocity by 0.75, never to zero");
  REQUIRE_EQ(latch.note_on(held(2), 1, record), 1, "soft scales velocity by 0.75, never to zero");
  latch.pedal(MidiPedal::soft, 0.0, record);
  REQUIRE_EQ(latch.note_on(held(3), 100, record), 100, "soft up leaves velocity alone");

  latch.pedal(MidiPedal::sustain, 1.0, record);
  require(!latch.note_off(held(3)), "held");
  (void)latch.note_on(held(3), 100, record);
  REQUIRE_EQ(released, std::vector<std::uint64_t>{3},
             "re-striking a held note releases its earlier strike first");

  PedalLatch full;
  full.pedal(MidiPedal::sustain, 1.0, record);
  for (std::uint64_t i = 0; i < PedalLatch::capacity; ++i) {
    (void)full.note_on(held(100 + i), 100, record);
    require(!full.note_off(held(100 + i)), "within capacity notes are held");
  }
  (void)full.note_on(held(999), 100, record);
  require(full.note_off(held(999)), "past capacity a note-off is delivered, never lost");
}

// Every engine: a note released under the sustain pedal keeps sounding
// until the lift, and renders identically under any block split.
void every_engine_sustains_generically() {
  // Bow plays its synthetic library; without one it is silent.
  (void)bow_test::install();
  for (const auto* engine : engines()) {
    if (engine->models_pedals) {
      continue;
    }
    const auto values = preset(*engine->descriptor, engine->descriptor->presets.front().name);
    const std::vector<PedalEvent> pedal{{0, MidiPedal::sustain, 1.0},
                                        {36'000, MidiPedal::sustain, 0.0}};
    auto plain = make_runtime(*engine, values);
    auto pedalled = make_runtime(*engine, values);
    auto split = make_runtime(*engine, values);
    const auto off = render(*plain, note(0, 4'800, 60, 1), 30'000);
    const auto on = render(*pedalled, note(0, 4'800, 60, 1), 30'000, 128, 48'000.0, 120.0, pedal);
    const auto irregular = render(*split, note(0, 4'800, 60, 1), 30'000, 0, 48'000.0, 120.0, pedal);
    const std::string name(engine->descriptor->id);
    require(on.left == irregular.left && on.right == irregular.right,
            name + ": pedals render identically under any block split");
    if (name == "pulse") {
      continue; // Drum hits are one-shots: note-offs do not end them.
    }
    REQUIRE_GE(on.last.active_voices, off.last.active_voices, name + ": sustain keeps the voice");
    REQUIRE_NE(on.left, off.left, name + ": a held note differs from a released one");
  }
}

// Prism's default sustains; with a short release, the release starts at the
// lift, not the note-off: silent without the pedal, still sounding with it.
void release_starts_at_the_lift() {
  const auto& prism = oscillate::native_dsp::synth::prism::engine();
  auto values = preset(*prism.descriptor, "Prism Default");
  values[index(*prism.descriptor, "prism.amp.release")] = 0.05;
  auto plain = make_runtime(prism, values);
  auto pedalled = make_runtime(prism, values);
  const auto off = render(*plain, note(0, 4'800, 57, 1), 48'000);
  const auto on = render(*pedalled, note(0, 4'800, 57, 1), 48'000, 128, 48'000.0, 120.0,
                         {{2'400, MidiPedal::sustain, 1.0}, {24'000, MidiPedal::sustain, 0.0}});
  REQUIRE_GT(peak(on.left, 20'000, 24'000), 100.0 * peak(off.left, 20'000, 24'000) + 1e-6,
             "the pedal holds the note past its note-off");
  REQUIRE_LT(peak(on.left, 47'000), 0.1 * peak(on.left, 20'000, 24'000), "the lift releases it");

  // Sostenuto: only the note held as it goes down rings on.
  auto sostenuto = make_runtime(prism, values);
  auto events = note(0, 4'800, 57, 1);
  const auto later = note(2'400, 2'400, 64, 2);
  events.insert(events.end(), later.begin(), later.end());
  auto only_later = make_runtime(prism, values);
  const auto captured = render(*sostenuto, events, 30'000, 128, 48'000.0, 120.0,
                               {{1'200, MidiPedal::sostenuto, 1.0}});
  const auto alone = render(*only_later, later, 30'000);
  REQUIRE_GT(peak(captured.left, 20'000, 30'000), 100.0 * peak(alone.left, 20'000, 30'000) + 1e-6,
             "sostenuto holds the note that was down, not the one struck after");
}

namespace project = oscillate::project;
namespace timeline = oscillate::timeline;

using oscillate::test::take;
template <typename Id> Id make_id(std::uint64_t value) { return take(Id::from_bits({0, value})); }

// A Prism track with one clip: a note struck at 1.5 quarters that ends at
// 1.6, under a sustain pedal held from the clip's start.
project::Snapshot chase_project(bool pedal) {
  constexpr auto quarter = timeline::ticks_per_quarter;
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  project::Snapshot state{
      make_id<project::ProjectId>(1), 1, 48'000, std::move(tempo), std::move(meter), {}, {}};
  project::Track track{make_id<project::TrackId>(2),
                       {},
                       project::TrackKind::instrument,
                       project::NativeInstrumentPatch{"prism", 1, {{"prism.amp.release", 0.02}}}};
  project::MidiClip clip{
      make_id<project::ClipId>(3),
      project::MusicalInterval{{0}, {8 * quarter}},
      {8 * quarter},
      false,
      {{make_id<project::NoteId>(4), {3 * quarter / 2}, {8 * quarter / 5}, 57, 100}}};
  if (pedal) {
    clip.pedals = {{MidiPedal::sustain, {{{0}, 1.0}}}};
  }
  track.midi_clips.push_back(std::move(clip));
  state.tracks.push_back(std::move(track));
  return state;
}

// Renders [from, to) in 128-frame blocks, after optionally rendering
// [0, warm) under generation 1; the main span is generation `generation`.
std::vector<double> play(const project::Snapshot& state, std::uint64_t begin, std::uint64_t end,
                         std::uint64_t warm, std::uint64_t generation) {
  const auto plan = take(oscillate::engine::compile_midi_events(state));
  auto player = take(oscillate::native_inserts::NativeSynthPlayer::prepare(
      state, state.tracks[0], plan.instruments()[0], {}, 128,
      std::numeric_limits<std::uint64_t>::max()));
  std::vector<oscillate::engine::StereoFrame> buffer(128);
  for (std::uint64_t frame = 0; frame < warm; frame += 128) {
    buffer.assign(128, {});
    require(player.render(buffer, frame, 1), "warm-up block renders");
  }
  std::vector<double> out;
  for (std::uint64_t frame = begin; frame < end; frame += 128) {
    buffer.assign(128, {});
    require(player.render(buffer, frame, generation), "block renders");
    for (const auto& sample : buffer) {
      out.push_back(sample.left);
    }
  }
  return out;
}

// A pedal held across the playhead stays down after starting or seeking
// there (ADR-0035): the player chases it, so the note struck after the
// jump rings past its note-off (38,400), while without the lane it is gone.
void players_chase_held_pedals() {
  const auto pedalled = chase_project(true);
  const auto bare = chase_project(false);
  const auto started = play(pedalled, 30'016, 60'032, 0, 1);
  const auto sought = play(pedalled, 30'016, 60'032, 12'032, 2);
  const auto released = play(bare, 30'016, 60'032, 0, 1);
  // 45,000 is 6,600 frames after the note-off: a 20 ms release is long gone.
  const std::size_t at = 45'000 - 30'016;
  REQUIRE_GT(peak(started, at, at + 4'800), 100.0 * peak(released, at, at + 4'800) + 1e-6,
             "starting inside a held pedal keeps it down");
  // The post chain (chorus phase) runs on across a seek by design, so a
  // sought render is not sample-equal to a fresh start; the pedal is.
  REQUIRE_GT(peak(sought, at, at + 4'800), 100.0 * peak(released, at, at + 4'800) + 1e-6,
             "a seek into a held pedal keeps it down");
  REQUIRE_GT(peak(sought, at, at + 4'800), 0.5 * peak(started, at, at + 4'800),
             "the sought note rings like the fresh start's");
}

} // namespace

// Notes played live (ADR-0057 follow-up: the monitor graph) sound through
// the player alongside the project's notes, release on their key, and a seek
// silences them with the rest.
void players_take_live_notes() {
  auto state = chase_project(false);
  state.tracks[0].midi_clips.clear();
  const auto plan = take(oscillate::engine::compile_midi_events(state));
  const oscillate::engine::InstrumentEvents empty{state.tracks[0].id, {}};
  auto player = take(oscillate::native_inserts::NativeSynthPlayer::prepare(
      state, state.tracks[0], plan.instruments().empty() ? empty : plan.instruments()[0], {}, 128,
      std::numeric_limits<std::uint64_t>::max()));
  std::vector<oscillate::engine::StereoFrame> buffer(128);
  std::uint64_t frame = 0;
  const auto block_peak = [&](std::uint64_t generation) {
    buffer.assign(128, {});
    require(player.render(buffer, frame, generation), "a live block renders");
    frame += 128;
    double peak = 0.0;
    for (const auto& sample : buffer)
      peak = std::max(peak, std::fabs(static_cast<double>(sample.left)));
    return peak;
  };
  REQUIRE_EQ(block_peak(1), 0.0, "a clipless track is silent");
  REQUIRE_TRUE(player.queue_live_note(60, 100, true), "live notes are queued");
  REQUIRE_TRUE(player.queue_live_note(64, 100, true), "live notes are queued");
  double loud = 0.0;
  for (int block = 0; block < 40; ++block)
    loud = std::max(loud, block_peak(1));
  REQUIRE_GT(loud, 0.01, "live notes sound with no clip");
  REQUIRE_TRUE(player.queue_live_note(60, 0, false), "live releases are queued");
  REQUIRE_TRUE(player.queue_live_note(64, 0, false), "live releases are queued");
  double tail = 1.0;
  for (int block = 0; block < 400; ++block)
    tail = block_peak(1);
  REQUIRE_LT(tail, 1e-4, "released live notes fall silent");
  require(player.queue_live_note(67, 100, true), "a held live note");
  for (int block = 0; block < 20; ++block)
    (void)block_peak(1);
  frame = 0;
  double after_seek = 1.0;
  for (int block = 0; block < 200; ++block)
    after_seek = block_peak(2);
  REQUIRE_LT(after_seek, 1e-4, "a seek silences live notes too");
}

int main() {
  try {
    players_take_live_notes();
    sustain_defers_and_flushes();
    sostenuto_holds_only_captured_notes();
    soft_restrike_and_capacity();
    every_engine_sustains_generically();
    release_starts_at_the_lift();
    players_chase_held_pedals();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "pedal contract passed\n";
  return 0;
}
