#include "oscillate/foundation/id.hpp"
#include "oscillate/native_dsp/synth/nova/arp.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"
#include "oscillate/timeline/timing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <new>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {

using namespace oscillate;
using namespace oscillate::native_dsp::synth;
using namespace oscillate::native_dsp::synth::nova;

constexpr double sample_rate = oscillate::test::sample_rate_48k_hz;
constexpr foundation::IdBits played_clip{1, 1};
constexpr double quarter = static_cast<double>(timeline::ticks_per_quarter);

using oscillate::test::require;

// A played note event at an absolute frame.
struct Press {
  std::uint64_t frame = 0;
  bool on = true;
  std::uint8_t key = 60;
  std::uint8_t velocity = 100;
  std::uint64_t id = 0;
};

std::vector<Press> hold(std::uint64_t start, std::uint64_t end, std::uint8_t key, std::uint64_t id,
                        std::uint8_t velocity = 100) {
  return {{start, true, key, velocity, id}, {end, false, key, 0, id}};
}

std::vector<Press> join(std::initializer_list<std::vector<Press>> parts) {
  std::vector<Press> all;
  for (const auto& part : parts) {
    all.insert(all.end(), part.begin(), part.end());
  }
  return all;
}

// A generated note at an absolute frame.
struct Note {
  std::uint64_t frame = 0;
  bool on = true;
  std::uint8_t key = 0;
  std::uint8_t velocity = 0;
  foundation::IdBits clip;
  foundation::IdBits id;
  float mod = 0.0F;
  bool accent = false;
};

// bpm 0: no tempo (the generator's free 120 BPM clock).
struct Song {
  double bpm = 120.0;
  double start_beat = 0.0;
  [[nodiscard]] double beats_per_frame() const { return bpm / 60.0 / sample_rate; }
};

using BeforeBlock = std::function<void(std::uint64_t, NoteGenerator&)>;

struct Render {
  Render(std::vector<Press> presses_, std::uint64_t frames_, Song song_ = {},
         std::vector<std::uint32_t> blocks_ = {512}, BeforeBlock before_block_ = {},
         std::uint64_t first_frame_ = 0)
      : presses(std::move(presses_)), frames(frames_), song(song_), blocks(std::move(blocks_)),
        before_block(std::move(before_block_)), first_frame(first_frame_) {}
  std::vector<Press> presses;
  std::uint64_t frames = 0;
  Song song;
  std::vector<std::uint32_t> blocks; // Cycled.
  BeforeBlock before_block;
  std::uint64_t first_frame = 0; // Absolute frame of the render's frame 0.
};

std::vector<Note> render(NoteGenerator& generator, Render job) {
  std::stable_sort(job.presses.begin(), job.presses.end(), [](const Press& a, const Press& b) {
    return a.frame < b.frame || (a.frame == b.frame && !a.on && b.on);
  });
  std::vector<Note> notes;
  std::vector<GeneratedNote> played;
  std::array<GeneratedNote, max_generated_notes> out{};
  std::size_t next = 0;
  std::size_t block = 0;
  for (std::uint64_t frame = 0; frame < job.frames;) {
    const auto length = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(job.blocks[block++ % job.blocks.size()], job.frames - frame));
    if (job.before_block) {
      job.before_block(frame, generator);
    }
    played.clear();
    while (next < job.presses.size() && job.presses[next].frame < frame + length) {
      const auto& press = job.presses[next++];
      played.push_back({static_cast<std::uint32_t>(press.frame - frame), press.on,
                        NoteEvent{played_clip, {0, press.id}, press.key, press.velocity}});
    }
    SynthTransport transport;
    if (job.song.bpm > 0.0) {
      const double per_frame = job.song.beats_per_frame();
      transport.beat_at_block_start =
          job.song.start_beat + per_frame * static_cast<double>(job.first_frame + frame);
      transport.beats_per_frame = per_frame;
      transport.bar_at_block_start = transport.beat_at_block_start / 4.0;
      transport.beats_per_bar = 4.0;
    }
    counting_allocations = true;
    const std::size_t count = generator.process(played, transport, length, out);
    counting_allocations = false;
    REQUIRE_LE(count, max_generated_notes, "never more than max_generated_notes per call");
    for (std::size_t i = 0; i < count; ++i) {
      const auto& note = out[i];
      REQUIRE_LT(note.offset, length, "output inside the block");
      if (i > 0) {
        const auto& previous = out[i - 1];
        require(previous.offset < note.offset ||
                    (previous.offset == note.offset && (!previous.on || note.on)),
                "output sorted, offs before ons at one frame");
      }
      notes.push_back({frame + note.offset, note.on, note.note.key, note.note.velocity,
                       note.note.clip, note.note.note, note.mod, note.accent});
    }
    frame += length;
  }
  return notes;
}

ArpSettings arp_with(ArpMode mode) {
  ArpSettings arp;
  arp.on = true;
  arp.mode = mode;
  return arp;
}

// One step of one lane.
void set_step(ArpSettings& arp, ArpLane lane, std::size_t step, double value) {
  arp.lane(lane).values[step] = value;
}

// A lane's first steps and its length.
void set_lane(ArpSettings& arp, ArpLane lane, std::initializer_list<double> values) {
  auto& target = arp.lane(lane);
  target.length = static_cast<std::uint32_t>(values.size());
  std::size_t i = 0;
  for (const double value : values) {
    target.values[i++] = value;
  }
}

std::vector<Note> ons(const std::vector<Note>& notes) {
  std::vector<Note> result;
  std::copy_if(notes.begin(), notes.end(), std::back_inserter(result),
               [](const Note& note) { return note.on; });
  return result;
}

std::vector<int> keys(const std::vector<Note>& notes) {
  std::vector<int> result;
  for (const auto& note : notes) {
    result.push_back(note.key);
  }
  return result;
}

using Signature = std::tuple<std::uint64_t, bool, int, int>;
std::vector<Signature> signature(const std::vector<Note>& notes) {
  std::vector<Signature> result;
  for (const auto& note : notes) {
    result.emplace_back(note.frame, note.on, note.key, note.velocity);
  }
  return result;
}

// Requires equal notes, naming the first difference.
void require_same(const std::vector<Note>& actual, const std::vector<Note>& expected,
                  std::string_view what) {
  const auto a = signature(actual);
  const auto b = signature(expected);
  for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
    if (a[i] != b[i]) {
      const auto describe = [](const Signature& s) {
        return std::to_string(std::get<0>(s)) + (std::get<1>(s) ? " on " : " off ") +
               std::to_string(std::get<2>(s)) + " v" + std::to_string(std::get<3>(s));
      };
      throw std::runtime_error(std::string(what) + ": note " + std::to_string(i) + " is " +
                               describe(a[i]) + ", expected " + describe(b[i]));
    }
  }
  REQUIRE_EQ(a.size(), b.size(),
             std::string(what) + ": " + std::to_string(a.size()) + " notes, expected " +
                 std::to_string(b.size()));
}

// Every id alternates on, off, on, off...; each off comes after its on; with
// `closed`, nothing is left sounding.
void require_paired(const std::vector<Note>& notes, bool closed, std::string_view what) {
  std::map<std::pair<foundation::IdBits, foundation::IdBits>, const Note*> sounding;
  std::set<std::pair<foundation::IdBits, foundation::IdBits>> generated_seen;
  for (const auto& note : notes) {
    const auto id = std::make_pair(note.clip, note.id);
    const auto found = sounding.find(id);
    if (note.on) {
      REQUIRE_EQ(found, sounding.end(), std::string(what) + ": an id sounds twice");
      if (note.clip != played_clip) {
        require(generated_seen.insert(id).second, std::string(what) + ": a minted id is reused");
      }
      sounding[id] = &note;
    } else {
      REQUIRE_NE(found, sounding.end(),
                 std::string(what) + ": an off with no on (frame " + std::to_string(note.frame) +
                     ", key " + std::to_string(note.key) +
                     (note.clip == played_clip ? ", played)" : ", generated)"));
      require(found->second->frame <= note.frame && found->second->key == note.key,
              std::string(what) + ": an off before its on or on another key");
      sounding.erase(found);
    }
  }
  if (closed) {
    require(sounding.empty(), std::string(what) + ": a note never got its off");
  }
}

std::vector<std::uint32_t> random_blocks(std::uint32_t seed, std::size_t count) {
  std::vector<std::uint32_t> sizes;
  std::uint32_t state = seed;
  for (std::size_t i = 0; i < count; ++i) {
    state = state * 1664525U + 1013904223U;
    sizes.push_back(1 + (state >> 8U) % 1499U);
  }
  return sizes;
}

std::int64_t grid_frame(double beat, const Song& song) {
  return static_cast<std::int64_t>(
      std::ceil((beat - song.start_beat) / song.beats_per_frame() - 1e-4));
}

project::NativeSequenceNote seq_note(double start, double length, std::uint8_t key,
                                     std::uint8_t velocity = 100) {
  return {timeline::TickPosition{static_cast<std::int64_t>(start * quarter)},
          timeline::TickPosition{static_cast<std::int64_t>(length * quarter)}, key, velocity};
}

project::NativeSequence sequence(std::string id, double length,
                                 std::vector<project::NativeSequenceNote> notes) {
  return {std::move(id), timeline::TickPosition{static_cast<std::int64_t>(length * quarter)},
          std::move(notes)};
}

// ---- Cases ----------------------------------------------------------------

constexpr std::uint64_t step_frames = 6'000; // 1/16 at 120 BPM, 48 kHz.

void compiles_document_clips() {
  project::NativeSynthDocument document;
  document.sequences.push_back(sequence(
      "clip.3", 8.0, {seq_note(2.0, 0.5, 64), seq_note(0.0, 0.25, 60), seq_note(9.0, 1.0, 70)}));
  const auto clips = compile_sequencer_clips(document);
  REQUIRE_EQ(clips.length_beats[0], 4.0, "a missing clip is one bar");
  REQUIRE_TRUE(clips.notes[0].empty(), "a missing clip is one bar");
  REQUIRE_EQ(clips.length_beats[2], 8.0, "clip.3 length in beats");
  REQUIRE_EQ(clips.notes[2].size(), 2, "notes past the clip end are dropped");
  REQUIRE_EQ(clips.notes[2][0].key, 60, "notes sorted by start, in beats");
  REQUIRE_EQ(clips.notes[2][0].start_beats, 0.0, "notes sorted by start, in beats");
  REQUIRE_EQ(clips.notes[2][0].length_beats, 0.25, "notes sorted by start, in beats");
  REQUIRE_EQ(clips.notes[2][1].key, 64, "second note in beats");
  REQUIRE_EQ(clips.notes[2][1].start_beats, 2.0, "second note in beats");
}

void modes_play_in_order() {
  // Pressed in the order E, C, G at one frame.
  const auto chord =
      join({hold(0, 1'000'000, 64, 1), hold(0, 1'000'000, 60, 2), hold(0, 1'000'000, 67, 3)});
  const auto run = [&](ArpMode mode, std::uint64_t steps) {
    NoteGenerator generator;
    generator.prepare(sample_rate);
    generator.set(arp_with(mode), {});
    return ons(render(generator, {chord, steps * step_frames}));
  };
  const std::vector<std::pair<ArpMode, std::vector<int>>> expected{
      {ArpMode::up, {60, 64, 67, 60, 64, 67, 60, 64}},
      {ArpMode::down, {67, 64, 60, 67, 64, 60, 67, 64}},
      {ArpMode::up_down, {60, 64, 67, 64, 60, 64, 67, 64}},
      {ArpMode::played, {64, 60, 67, 64, 60, 67, 64, 60}},
  };
  for (const auto& [mode, order] : expected) {
    const auto played = run(mode, 8);
    REQUIRE_EQ(keys(played), order, "mode plays in its order");
    for (std::size_t i = 0; i < played.size(); ++i) {
      REQUIRE_EQ(played[i].frame, i * step_frames, "one note per 1/16 step");
    }
  }
  const auto chord_steps = run(ArpMode::chord, 4);
  REQUIRE_EQ(chord_steps.size(), 12, "chord mode plays every held note each step");
  for (std::size_t i = 0; i < chord_steps.size(); ++i) {
    REQUIRE_EQ(chord_steps[i].frame, (i / 3) * step_frames, "chord notes share the step");
  }
  const auto random = run(ArpMode::random, 32);
  REQUIRE_EQ(keys(random), keys(run(ArpMode::random, 32)), "random is deterministic");
  const auto random_keys = keys(random);
  const std::set<int> distinct(random_keys.begin(), random_keys.end());
  REQUIRE_GE(distinct.size(), 2, "random picks among the held notes");
  REQUIRE_LE(distinct.size(), 3, "random picks among the held notes");
  REQUIRE_GE(*distinct.begin(), 60, "random picks among the held notes");
  REQUIRE_LE(*distinct.rbegin(), 67, "random picks among the held notes");
}

void steps_lock_to_the_beat_grid() {
  for (const double bpm : {90.0, 120.0, 174.0}) {
    const Song song{bpm, 3.1};
    NoteGenerator generator;
    generator.prepare(sample_rate);
    generator.set(arp_with(ArpMode::up), {});
    const auto played = ons(render(generator, {hold(0, 10'000'000, 60, 1), 200'000, song, {333}}));
    REQUIRE_GE(played.size(), 10, "steps play");
    for (std::size_t i = 0; i < 10; ++i) {
      // Step 13 (beat 3.25) is the first after the song position 3.1.
      const double beat = static_cast<double>(13 + i) * 0.25;
      REQUIRE_EQ(static_cast<std::int64_t>(played[i].frame), grid_frame(beat, song),
                 "step n starts on beat n x rate at " + std::to_string(bpm) + " BPM");
    }
  }
  // Without a tempo: 120 BPM from the first held note.
  NoteGenerator generator;
  generator.prepare(sample_rate);
  generator.set(arp_with(ArpMode::up), {});
  const auto free = ons(render(generator, {hold(100, 10'000'000, 60, 1), 60'000, Song{0.0}}));
  REQUIRE_EQ(free.size(), 10, "free-running steps");
  for (std::size_t i = 0; i < free.size(); ++i) {
    REQUIRE_EQ(free[i].frame, 100 + i * step_frames, "free steps from the first held note");
  }
}

void swing_delays_every_second_step() {
  auto arp = arp_with(ArpMode::up);
  arp.swing = 0.5;
  arp.gate = 1.0;
  NoteGenerator generator;
  generator.prepare(sample_rate);
  generator.set(arp, {});
  const auto notes = render(generator, {hold(0, 10'000'000, 60, 1), 8 * step_frames});
  const auto played = ons(notes);
  REQUIRE_EQ(played.size(), 8, "eight steps");
  for (std::size_t i = 0; i < played.size(); ++i) {
    const std::uint64_t late = i % 2 == 1 ? step_frames / 2 : 0;
    REQUIRE_EQ(played[i].frame, i * step_frames + late, "odd steps late by swing x step");
  }
  // A swung step is shorter: its full gate ends on the next step.
  for (const auto& note : notes) {
    if (!note.on && note.frame > 6'000 && note.frame < 13'000) {
      REQUIRE_EQ(note.frame, 12'000, "a late step's note ends at the next step");
    }
  }
}

void gate_lengths_follow_gate_times_step_gate() {
  auto arp = arp_with(ArpMode::up);
  arp.gate = 0.5;
  set_step(arp, ArpLane::gate, 1, 0.5);
  NoteGenerator generator;
  generator.prepare(sample_rate);
  generator.set(arp, {});
  const auto notes = render(generator, {hold(0, 10'000'000, 60, 1), 2 * step_frames});
  REQUIRE_EQ(notes.size(), 4, "two notes, two offs");
  REQUIRE_TRUE(notes[0].on, "gate 0.5 of a step");
  REQUIRE_EQ(notes[0].frame, 0, "gate 0.5 of a step");
  REQUIRE_TRUE(!notes[1].on, "gate 0.5 of a step");
  REQUIRE_EQ(notes[1].frame, 3'000, "gate 0.5 of a step");
  REQUIRE_TRUE(notes[2].on, "step gate multiplies the arp gate");
  REQUIRE_EQ(notes[2].frame, 6'000, "step gate multiplies the arp gate");
  REQUIRE_TRUE(!notes[3].on, "step gate multiplies the arp gate");
  REQUIRE_EQ(notes[3].frame, 7'500, "step gate multiplies the arp gate");
}

void ties_hold_into_the_next_step() {
  auto arp = arp_with(ArpMode::up);
  set_step(arp, ArpLane::tie, 0, 1.0);
  {
    NoteGenerator generator;
    generator.prepare(sample_rate);
    generator.set(arp, {});
    const auto notes = render(generator, {hold(0, 10'000'000, 60, 1), 3 * step_frames});
    // One note: tied into step 1, which continues it to its gate end.
    REQUIRE_EQ(notes.size(), 4, "a tie into the same key does not retrigger");
    REQUIRE_TRUE(notes[0].on, "tied note starts");
    REQUIRE_EQ(notes[0].frame, 0, "tied note starts");
    REQUIRE_TRUE(!notes[1].on, "the tied note ends at the next step's gate");
    REQUIRE_EQ(notes[1].frame, step_frames + 3'600, "the tied note ends at the next step's gate");
    REQUIRE_EQ(notes[1].id, notes[0].id, "the tied note ends at the next step's gate");
    REQUIRE_TRUE(notes[2].on, "step 2 plays fresh");
    REQUIRE_EQ(notes[2].frame, 2 * step_frames, "step 2 plays fresh");
  }
  {
    NoteGenerator generator;
    generator.prepare(sample_rate);
    generator.set(arp, {});
    const auto notes =
        render(generator,
               {join({hold(0, 10'000'000, 60, 1), hold(0, 10'000'000, 64, 2)}), step_frames + 10});
    REQUIRE_EQ(notes.size(), 3, "first note");
    REQUIRE_TRUE(notes[0].on, "first note");
    REQUIRE_EQ(notes[0].key, 60, "first note");
    REQUIRE_TRUE(!notes[1].on, "a tie holds to the next step");
    REQUIRE_EQ(notes[1].key, 60, "a tie holds to the next step");
    REQUIRE_EQ(notes[1].frame, step_frames, "a tie holds to the next step");
    REQUIRE_TRUE(notes[2].on, "the next key starts as the tie ends");
    REQUIRE_EQ(notes[2].key, 64, "the next key starts as the tie ends");
    REQUIRE_EQ(notes[2].frame, step_frames, "the next key starts as the tie ends");
  }
}

void octaves_extend_the_pattern() {
  auto arp = arp_with(ArpMode::up);
  arp.octaves = 3;
  NoteGenerator generator;
  generator.prepare(sample_rate);
  generator.set(arp, {});
  REQUIRE_EQ(keys(ons(render(generator, {hold(0, 10'000'000, 60, 1), 6 * step_frames}))),
             (std::vector<int>{60, 72, 84, 60, 72, 84}), "up over three octaves");
  arp.mode = ArpMode::down;
  arp.octaves = 2;
  NoteGenerator down;
  down.prepare(sample_rate);
  down.set(arp, {});
  REQUIRE_EQ(keys(ons(render(down, {hold(0, 10'000'000, 60, 1), 6 * step_frames}))),
             (std::vector<int>{72, 60, 72, 60, 72, 60}), "down over two octaves");
}

void latch_holds_until_a_new_chord() {
  auto arp = arp_with(ArpMode::up);
  arp.latch = true;
  NoteGenerator generator;
  generator.prepare(sample_rate);
  generator.set(arp, {});
  const auto presses = join({hold(0, 1'000, 60, 1), hold(0, 1'000, 64, 2),
                             hold(5 * step_frames, 5 * step_frames + 1'000, 67, 3)});
  bool unlatched = false;
  const auto notes =
      render(generator,
             {presses, 12 * step_frames, Song{}, {512}, [&](std::uint64_t frame, NoteGenerator& g) {
                if (!unlatched && frame >= 9 * step_frames - 1'000) {
                  auto off = arp;
                  off.latch = false;
                  g.set(off, {});
                  unlatched = true;
                }
              }});
  const auto played = ons(notes);
  REQUIRE_EQ(played.size(), 9, "latched notes play until latch goes off");
  for (const auto& note : played) {
    if (note.frame < 5 * step_frames) {
      require(note.key == 60 || note.key == 64, "the released chord keeps playing");
    } else {
      REQUIRE_EQ(note.key, 67, "a new chord replaces the latched one");
    }
  }
  require_paired(notes, true, "latch");
}

// Every mode with the pattern engine's lanes at work (ratchets, ties,
// chance, conditions, the chord lane, a groove and variation bars).
ArpSettings busy_arp(ArpMode mode) {
  auto arp = arp_with(mode);
  arp.swing = 0.3;
  arp.octaves = 2;
  arp.velocity = ArpVelocity::pattern;
  arp.seed = 17;
  arp.variation = 2;
  arp.strum = 0.6;
  arp.euclid_hits = 7;
  arp.euclid_rotate = 3;
  set_step(arp, ArpLane::tie, 2, 1.0);
  set_step(arp, ArpLane::gate, 3, 0.0);
  set_step(arp, ArpLane::gate, 5, 0.3);
  set_step(arp, ArpLane::velocity, 7, 0.4);
  set_step(arp, ArpLane::tie, 9, 1.0);
  set_lane(arp, ArpLane::ratchet, {1, 1, 3, 1, 2});
  set_lane(arp, ArpLane::note, {0, 2, -1, 1, 0, 3, 1});
  set_lane(arp, ArpLane::octave, {0, 0, 1, 0, -1, 0});
  set_lane(arp, ArpLane::chance, {1, 0.6, 1, 0.8, 1, 1, 0.4, 1, 1, 1, 1});
  set_lane(arp, ArpLane::condition,
           {0, static_cast<double>(ArpCondition::one_of_two), 0,
            static_cast<double>(ArpCondition::fill), 0,
            static_cast<double>(ArpCondition::not_first)});
  set_lane(arp, ArpLane::chord,
           {0, 0, static_cast<double>(ArpChord::triad), 0, 0, static_cast<double>(ArpChord::fifth),
            0, 0, 0});
  set_lane(arp, ArpLane::accent, {1, 0, 0});
  set_lane(arp, ArpLane::mod, {0.1, 0.5, 0.9, 0.3, 0.7});
  arp.groove.steps = 4;
  arp.groove.timing = {0.0, 0.2, -0.1, 0.3};
  arp.groove.velocity = {1.0, 0.7, 1.2, 0.8};
  arp.groove.amount = 0.8;
  arp.rate = 12; // 1/32
  return arp;
}

void block_splits_do_not_change(const ArpSettings& arp, const std::string& name) {
  const auto presses = join({hold(777, 90'000, 60, 1), hold(4'321, 70'001, 63, 2),
                             hold(50'003, 150'000, 70, 3), hold(110'000, 190'000, 55, 4)});
  project::NativeSynthDocument document;
  document.sequences.push_back(sequence(
      "clip.1", 2.0, {seq_note(0.0, 0.3, 60), seq_note(0.5, 0.75, 67), seq_note(1.25, 0.2, 62)}));
  const auto clips = compile_sequencer_clips(document);
  SequencerSettings sequencer;
  sequencer.on = true;
  for (const Song song : {Song{137.0, 1.37}, Song{0.0}}) {
    for (const bool with_sequencer : {false, true}) {
      const auto run = [&](std::vector<std::uint32_t> blocks) {
        NoteGenerator generator;
        generator.prepare(sample_rate);
        generator.adopt(clips);
        generator.set(arp, with_sequencer ? sequencer : SequencerSettings{});
        return render(generator, {presses, 220'000, song, std::move(blocks)});
      };
      const auto whole = run({4'096});
      require(!ons(whole).empty(), name + ": the scenario plays");
      require_paired(whole, false, name + ": split scenario");
      for (const std::uint32_t seed : {1U, 7U, 99U}) {
        require_same(run(random_blocks(seed, 400)), whole,
                     name + ": random block splits give identical notes (" +
                         std::to_string(song.bpm) + " BPM, sequencer " +
                         std::to_string(with_sequencer) + ", seed " + std::to_string(seed) + ")");
      }
      require_same(run({1, 2, 3, 5, 8, 13, 21, 34, 55, 89}), whole,
                   name + ": tiny blocks give identical notes");
    }
  }
}

void block_splits_do_not_change_notes() {
  for (std::size_t m = 0; m < arp_mode_count; ++m) {
    block_splits_do_not_change(busy_arp(static_cast<ArpMode>(m)), std::string(arp_mode_names[m]));
  }
}

void seek_reproduces_the_song_position() {
  auto arp = arp_with(ArpMode::random);
  arp.retrigger = false; // The pattern locks to the song's step count.
  arp.octaves = 2;
  const Song song{128.0, 0.0};
  const auto chord = [](std::uint64_t at) {
    return join(
        {hold(at, 10'000'000, 60, 1), hold(at, 10'000'000, 64, 2), hold(at, 10'000'000, 67, 3)});
  };
  NoteGenerator uninterrupted;
  uninterrupted.prepare(sample_rate);
  uninterrupted.set(arp, {});
  const auto whole = ons(render(uninterrupted, {chord(0), 200'000, song}));

  // A generator that played something else, then seeks to beat 3.3.
  const double seek_beat = 3.3;
  NoteGenerator seeked;
  seeked.prepare(sample_rate);
  seeked.set(arp, {});
  (void)render(seeked, {join({hold(0, 30'000, 50, 9), hold(100, 30'000, 53, 8)}), 20'000,
                        Song{128.0, 50.0}});
  seeked.reset();
  const Song from{128.0, seek_beat};
  const auto after = render(seeked, {chord(0), 120'000, from, {700}});

  NoteGenerator fresh;

  fresh.prepare(sample_rate);
  fresh.set(arp, {});
  REQUIRE_EQ(signature(after), signature(render(fresh, {chord(0), 120'000, from, {700}})),
             "reset then a later position equals a fresh render from there");

  const double per_frame = song.beats_per_frame();
  std::vector<std::pair<double, int>> expected;
  for (const auto& note : whole) {
    const double beat = static_cast<double>(note.frame) * per_frame;
    if (beat > seek_beat && beat < seek_beat + 120'000 * per_frame) {
      expected.emplace_back(beat, note.key);
    }
  }
  const auto seeked_ons = ons(after);
  REQUIRE_EQ(seeked_ons.size(), expected.size(), "the same steps play after the seek");
  REQUIRE_TRUE(!expected.empty(), "the same steps play after the seek");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const double beat = seek_beat + static_cast<double>(seeked_ons[i].frame) * per_frame;
    REQUIRE_LT(std::fabs(beat - expected[i].first), 1.5 * per_frame,
               "after a seek the notes match the uninterrupted render");
    REQUIRE_EQ(seeked_ons[i].key, expected[i].second,
               "after a seek the notes match the uninterrupted render");
  }
}

void every_on_has_one_off() {
  SequencerSettings sequencer;
  project::NativeSynthDocument document;
  document.sequences.push_back(
      sequence("clip.1", 1.0, {seq_note(0.0, 0.5, 60), seq_note(0.5, 0.5, 65)}));
  const auto clips = compile_sequencer_clips(document);
  const auto presses = join({hold(10, 150'000, 60, 1), hold(20, 120'000, 64, 2),
                             hold(30'000, 200'000, 67, 3), hold(170'000, 230'000, 62, 4)});
  NoteGenerator generator;
  generator.prepare(sample_rate);
  generator.adopt(clips);
  auto arp = arp_with(ArpMode::up_down);
  set_step(arp, ArpLane::tie, 1, 1.0);
  set_step(arp, ArpLane::tie, 4, 1.0);
  const auto notes = render(
      generator, {presses, 320'000, Song{}, {997}, [&](std::uint64_t frame, NoteGenerator& g) {
                    if (frame == 0) {
                      g.set(arp, {});
                    } else if (frame == 40 * 997) {
                      auto changed = arp;
                      changed.mode = ArpMode::chord;
                      changed.octaves = 2;
                      g.set(changed, {});
                    } else if (frame == 80 * 997) {
                      g.set({}, {}); // Off: pass-through again.
                    } else if (frame == 120 * 997) {
                      sequencer.on = true;
                      g.set(arp, sequencer);
                    } else if (frame == 170 * 997) {
                      g.set(arp, {});
                    } else if (frame == 200 * 997) {
                      g.set({}, {});
                    }
                  }});
  require_paired(notes, true, "settings changes and releases");

  // A settings change releases what sounds, at the block start.
  NoteGenerator changing;
  changing.prepare(sample_rate);
  changing.set(arp_with(ArpMode::up), {});
  const auto released = render(changing, {hold(0, 1'000'000, 60, 1),
                                          2'000,
                                          Song{},
                                          {1'000},
                                          [](std::uint64_t frame, NoteGenerator& g) {
                                            if (frame == 1'000) {
                                              g.set(arp_with(ArpMode::down), {});
                                            }
                                          }});
  REQUIRE_EQ(released.size(), 2, "a pattern change releases the sounding note at once");
  REQUIRE_TRUE(!released[1].on, "a pattern change releases the sounding note at once");
  REQUIRE_EQ(released[1].frame, 1'000, "a pattern change releases the sounding note at once");
  REQUIRE_EQ(released[1].id, released[0].id, "a pattern change releases the sounding note at once");

  // Many keys at a fast rate: the cap holds and offs still pair.
  NoteGenerator crowded;
  crowded.prepare(sample_rate);
  auto chord = arp_with(ArpMode::chord);
  chord.rate = 14; // 1/64.
  crowded.set(chord, {});
  std::vector<Press> all;
  for (std::uint8_t key = 0; key < 128; ++key) {
    const auto pressed = hold(0, 60'000, key, 1'000 + key);
    all.insert(all.end(), pressed.begin(), pressed.end());
  }
  const auto heavy = render(crowded, {all, 120'000, Song{}, {48'000}});
  require(!heavy.empty(), "the crowded chord plays");
  require_paired(heavy, true, "crowded");
}

void sequencer_loops_with_the_transport() {
  project::NativeSynthDocument document;
  document.sequences.push_back(
      sequence("clip.1", 4.0, {seq_note(2.0, 0.5, 64), seq_note(0.0, 0.5, 60)}));
  const auto clips = compile_sequencer_clips(document);
  SequencerSettings sequencer;
  sequencer.on = true;
  NoteGenerator generator;
  generator.prepare(sample_rate);
  generator.adopt(clips);
  generator.set({}, sequencer);
  const Song song{120.0, 5.0}; // 24 000 frames a beat.
  const auto notes = render(generator, {{}, 144'000, song, {1'000}});
  const auto played = ons(notes);
  REQUIRE_EQ(keys(played), (std::vector<int>{64, 60, 64}), "the clip loops with the song");
  REQUIRE_EQ(played[0].frame, 24'000, "clip position is the song beat mod the clip length");
  REQUIRE_EQ(played[1].frame, 72'000, "clip position is the song beat mod the clip length");
  REQUIRE_EQ(played[2].frame, 120'000, "clip position is the song beat mod the clip length");
  std::vector<std::uint64_t> offs;
  for (const auto& note : notes) {
    if (!note.on) {
      offs.push_back(note.frame);
    }
  }
  REQUIRE_EQ(offs, (std::vector<std::uint64_t>{36'000, 84'000, 132'000}), "half-beat notes");
  REQUIRE_EQ(generator.playing_clip(), 0, "telemetry: clip 1 playing");
  REQUIRE_LT(std::fabs(generator.clip_playhead_beats() - 3.0), tolerance::float_round_trip,
             "telemetry: playhead at beat 11 mod 4");
  REQUIRE_EQ(generator.arp_step(), -1, "telemetry: no arp step");
}

void note_trigger_quantises_to_a_sixteenth() {
  project::NativeSynthDocument document;
  document.sequences.push_back(sequence("clip.2", 1.0, {seq_note(0.0, 0.25, 72)}));
  const auto clips = compile_sequencer_clips(document);
  SequencerSettings sequencer;
  sequencer.on = true;
  sequencer.trigger = SequencerTrigger::note;
  sequencer.follow = false;
  NoteGenerator generator;
  generator.prepare(sample_rate);
  generator.adopt(clips);
  generator.set({}, sequencer);
  // Key 25 (root 24 + 1) plays clip 2: pressed at beat 1.1, released at 80 000.
  int playing = -2;
  const auto notes = render(generator, {hold(26'400, 80'000, 25, 1),
                                        140'000,
                                        Song{},
                                        {1'000},
                                        [&](std::uint64_t frame, NoteGenerator& g) {
                                          if (frame == 50'000) {
                                            playing = g.playing_clip();
                                          }
                                        }});
  const auto played = ons(notes);
  REQUIRE_EQ(keys(played), (std::vector<int>{72, 72, 72}), "the clip loops while held");
  REQUIRE_EQ(played[0].frame, 30'000, "the clip starts at the next 1/16 after the press");
  REQUIRE_EQ(played[1].frame, 54'000, "the clip starts at the next 1/16 after the press");
  REQUIRE_EQ(played[2].frame, 78'000, "the clip starts at the next 1/16 after the press");
  REQUIRE_TRUE(!notes.back().on, "release stops the clip at once");
  REQUIRE_EQ(notes.back().frame, 80'000, "release stops the clip at once");
  REQUIRE_EQ(playing, 1, "telemetry: clip 2 playing while held");
  REQUIRE_EQ(generator.playing_clip(), -1, "telemetry: nothing after release");

  // Without a tempo the press starts the free clock, so the clip starts at once.
  NoteGenerator free;
  free.prepare(sample_rate);
  free.adopt(clips);
  free.set({}, sequencer);
  const auto free_on = ons(render(free, {hold(1'000, 30'000, 25, 1), 30'000, Song{0.0}}));
  REQUIRE_TRUE(!free_on.empty(), "free clip starts on the press");
  REQUIRE_EQ(free_on[0].frame, 1'000, "free clip starts on the press");
}

void follow_transposes_by_the_held_key() {
  project::NativeSynthDocument document;
  document.sequences.push_back(sequence("clip.1", 4.0, {seq_note(2.0, 0.5, 64)}));
  const auto clips = compile_sequencer_clips(document);
  SequencerSettings sequencer;
  sequencer.on = true;
  {
    NoteGenerator generator;
    generator.prepare(sample_rate);
    generator.adopt(clips);
    generator.set({}, sequencer);
    const auto played =
        ons(render(generator, {hold(0, 1'000'000, 31, 1), 30'000, Song{120.0, 5.0}}));
    REQUIRE_EQ(played.size(), 1, "follow: root + 7 transposes up 7");
    REQUIRE_EQ(played[0].key, 71, "follow: root + 7 transposes up 7");
    REQUIRE_EQ(played[0].frame, 24'000, "follow: root + 7 transposes up 7");
  }
  {
    sequencer.follow = false;
    NoteGenerator generator;
    generator.prepare(sample_rate);
    generator.adopt(clips);
    generator.set({}, sequencer);
    const auto played =
        ons(render(generator, {hold(0, 1'000'000, 31, 5), 30'000, Song{120.0, 5.0}}));
    REQUIRE_EQ(played.size(), 2, "without follow the key plays along");
    REQUIRE_EQ(played[0].key, 31, "the played key passes through with its id");
    REQUIRE_EQ(played[0].clip, played_clip, "the played key passes through with its id");
    REQUIRE_EQ(played[0].id, (foundation::IdBits{0, 5}),
               "the played key passes through with its id");
    REQUIRE_EQ(played[1].key, 64, "the clip is not transposed");
  }
}

void sequencer_feeds_the_arpeggiator() {
  project::NativeSynthDocument document;
  document.sequences.push_back(
      sequence("clip.1", 4.0, {seq_note(0.0, 4.0, 60), seq_note(0.0, 4.0, 64)}));
  const auto clips = compile_sequencer_clips(document);
  SequencerSettings sequencer;
  sequencer.on = true;
  NoteGenerator generator;
  generator.prepare(sample_rate);
  generator.adopt(clips);
  generator.set(arp_with(ArpMode::up), sequencer);
  const auto played = ons(render(generator, {{}, 4 * step_frames, Song{}, {700}}));
  REQUIRE_EQ(keys(played), (std::vector<int>{60, 64, 60, 64}),
             "the sequencer's notes are the arpeggiator's chord");
  REQUIRE_EQ(played[0].frame, 0, "on the step grid");
  REQUIRE_EQ(played[3].frame, 3 * step_frames, "on the step grid");
}

void passes_through_when_off() {
  NoteGenerator generator;
  generator.prepare(sample_rate);
  require(!generator.active(), "off by default");
  const auto presses = join({hold(5, 900, 60, 11, 90), hold(5, 400, 64, 12, 70)});
  const auto notes = render(generator, {presses, 2'000, Song{}, {256}});
  REQUIRE_EQ(notes.size(), 4, "every played note passes");
  REQUIRE_EQ(notes[0].frame, 5, "unchanged, with its own id");
  REQUIRE_TRUE(notes[0].on, "unchanged, with its own id");
  REQUIRE_EQ(notes[0].key, 60, "unchanged, with its own id");
  REQUIRE_EQ(notes[0].velocity, 90, "unchanged, with its own id");
  REQUIRE_EQ(notes[0].clip, played_clip, "unchanged, with its own id");
  REQUIRE_EQ(notes[0].id, (foundation::IdBits{0, 11}), "unchanged, with its own id");
  REQUIRE_EQ(notes[1].key, 64, "second note");
  REQUIRE_EQ(notes[1].id, (foundation::IdBits{0, 12}), "second note");
  REQUIRE_TRUE(!notes[2].on, "offs pass through");
  REQUIRE_EQ(notes[2].frame, 400, "offs pass through");
  REQUIRE_TRUE(!notes[3].on, "offs pass through");
  REQUIRE_EQ(notes[3].frame, 900, "offs pass through");

  // Turning the arp on releases a passed note; turning it off plays it again.
  NoteGenerator toggled;
  toggled.prepare(sample_rate);
  const auto both = render(
      toggled,
      {hold(0, 100'000, 60, 7), 40'000, Song{}, {1'000}, [](std::uint64_t frame, NoteGenerator& g) {
         if (frame == 10'000) {
           g.set(arp_with(ArpMode::up), {});
         } else if (frame == 30'000) {
           g.set({}, {});
         }
       }});
  REQUIRE_TRUE(both[0].on, "passed through first");
  REQUIRE_EQ(both[0].clip, played_clip, "passed through first");
  REQUIRE_TRUE(!both[1].on, "arp on releases the passed note");
  REQUIRE_EQ(both[1].frame, 10'000, "arp on releases the passed note");
  REQUIRE_EQ(both[1].clip, played_clip, "arp on releases the passed note");
  REQUIRE_TRUE(both.back().on, "arp off plays the held note again with its id");
  REQUIRE_EQ(both.back().frame, 30'000, "arp off plays the held note again with its id");
  REQUIRE_EQ(both.back().clip, played_clip, "arp off plays the held note again with its id");
  REQUIRE_EQ(both.back().id, (foundation::IdBits{0, 7}),
             "arp off plays the held note again with its id");
  require_paired(both, false, "toggling");
}

void telemetry_reports_steps() {
  NoteGenerator generator;
  generator.prepare(sample_rate);
  generator.set(arp_with(ArpMode::up), {});
  REQUIRE_EQ(generator.arp_step(), -1, "idle telemetry");
  REQUIRE_EQ(generator.playing_clip(), -1, "idle telemetry");
  REQUIRE_EQ(generator.clip_playhead_beats(), 0.0, "idle telemetry");
  std::vector<int> steps;
  (void)render(generator, {hold(0, 20 * step_frames - 1'000, 60, 1),
                           20 * step_frames,
                           Song{},
                           {static_cast<std::uint32_t>(step_frames)},
                           [&](std::uint64_t frame, NoteGenerator& g) {
                             if (frame > 0) {
                               steps.push_back(g.arp_step());
                             }
                           }});
  for (std::size_t i = 0; i < steps.size(); ++i) {
    REQUIRE_EQ(steps[i], static_cast<int>(i), "the pattern step playing, counted from the start");
  }
  (void)render(
      generator,
      {{}, step_frames, Song{}, {static_cast<std::uint32_t>(step_frames)}, {}, 20 * step_frames});
  REQUIRE_EQ(generator.arp_step(), -1, "no step once released");
  generator.reset();
  REQUIRE_EQ(generator.arp_step(), -1, "reset clears");
  REQUIRE_EQ(generator.playing_clip(), -1, "reset clears");
}

// ---- The pattern engine (ADR-0078) -------------------------------------

const std::vector<Press> c_major =
    join({hold(0, 10'000'000, 64, 1), hold(0, 10'000'000, 60, 2), hold(0, 10'000'000, 67, 3)});

std::vector<Note> play(const ArpSettings& arp, const std::vector<Press>& presses,
                       std::uint64_t steps, Song song = {}) {
  NoteGenerator generator;
  generator.prepare(sample_rate);
  generator.set(arp, {});
  return render(generator, {presses, steps * step_frames, song});
}

std::vector<std::uint64_t> frames_of(const std::vector<Note>& notes) {
  std::vector<std::uint64_t> result;
  for (const auto& note : notes) {
    result.push_back(note.frame);
  }
  return result;
}

void new_modes_play_in_order() {
  const std::vector<std::pair<ArpMode, std::vector<int>>> expected{
      {ArpMode::down_up, {67, 64, 60, 64, 67, 64, 60, 64}},
      {ArpMode::converge, {60, 67, 64, 60, 67, 64, 60, 67}},
      {ArpMode::diverge, {64, 67, 60, 64, 67, 60, 64, 67}},
      {ArpMode::thumb, {60, 64, 60, 67, 60, 64, 60, 67}},
      {ArpMode::pinky, {67, 60, 67, 64, 67, 60, 67, 64}},
  };
  for (const auto& [mode, order] : expected) {
    REQUIRE_EQ(keys(ons(play(arp_with(mode), c_major, 8))), order,
               std::string(arp_mode_names[static_cast<std::size_t>(mode)]) + " plays in its order");
  }
  // Shape: the Note lane is the place in the held notes.
  auto shape = arp_with(ArpMode::shape);
  set_lane(shape, ArpLane::note, {2, 0, 1});
  REQUIRE_EQ(keys(ons(play(shape, c_major, 6))), (std::vector<int>{67, 60, 64, 67, 60, 64}),
             "shape draws the note order");
  // Shuffle: every note once per cycle, in a seeded order.
  auto shuffle = arp_with(ArpMode::shuffle);
  shuffle.seed = 5;
  const auto shuffled = keys(ons(play(shuffle, c_major, 9)));
  REQUIRE_EQ(shuffled.size(), 9, "shuffle plays every step");
  for (std::size_t cycle = 0; cycle < 3; ++cycle) {
    std::set<int> notes(shuffled.begin() + static_cast<std::ptrdiff_t>(3 * cycle),
                        shuffled.begin() + static_cast<std::ptrdiff_t>(3 * cycle + 3));
    REQUIRE_EQ(notes, (std::set<int>{60, 64, 67}), "each shuffle cycle plays every note once");
  }
  REQUIRE_EQ(shuffled, keys(ons(play(shuffle, c_major, 9))), "shuffle is deterministic");
  // Random walk: from the first note, a place or two at a time.
  auto walk = arp_with(ArpMode::walk);
  walk.octaves = 3;
  const auto walked = keys(ons(play(walk, c_major, 32)));
  const std::vector<int> places{60, 64, 67, 72, 76, 79, 84, 88, 91};
  REQUIRE_EQ(walked.front(), 60, "a walk starts on the first note");
  for (std::size_t i = 1; i < walked.size(); ++i) {
    const auto a = std::find(places.begin(), places.end(), walked[i - 1]) - places.begin();
    const auto b = std::find(places.begin(), places.end(), walked[i]) - places.begin();
    REQUIRE_NEAR(a, b, 2, "a walk moves at most two places");
    REQUIRE_LT(b, static_cast<std::ptrdiff_t>(places.size()), "a walk moves at most two places");
  }
  // Strum: the chord spread over strum × the step, alternately up and down.
  const auto strummed = ons(play(arp_with(ArpMode::strum), c_major, 2));
  REQUIRE_EQ(keys(strummed), (std::vector<int>{60, 64, 67, 67, 64, 60}), "strum goes up then down");
  REQUIRE_EQ(frames_of(strummed), (std::vector<std::uint64_t>{0, 700, 1'400, 6'000, 6'700, 7'400}),
             "strummed notes spread over the strum");
}

void lane_lengths_make_a_polymeter() {
  auto arp = arp_with(ArpMode::up);
  arp.velocity = ArpVelocity::pattern;
  set_lane(arp, ArpLane::velocity, {1.0, 0.5, 0.25});
  set_lane(arp, ArpLane::gate, {1.0, 1.0, 1.0, 0.0});
  const auto played = ons(play(arp, {hold(0, 10'000'000, 60, 1)}, 12));
  const std::vector<std::uint64_t> steps{0, 1, 2, 4, 5, 6, 8, 9, 10};
  REQUIRE_EQ(played.size(), steps.size(), "the gate lane rests every fourth step");
  const std::array<int, 3> velocities{127, 64, 32};
  for (std::size_t i = 0; i < steps.size(); ++i) {
    REQUIRE_EQ(played[i].frame, steps[i] * step_frames, "steps where the gate lane plays");
    REQUIRE_EQ(played[i].velocity, velocities[steps[i] % 3], "the velocity lane loops at 3");
  }
  REQUIRE_EQ(arp_period(arp), 48, "the pattern repeats at lcm(3, 4, 16)");
}

void ratchets_split_a_step() {
  auto arp = arp_with(ArpMode::up);
  arp.velocity = ArpVelocity::pattern;
  set_step(arp, ArpLane::ratchet, 0, 3);
  const auto notes = play(arp, {hold(0, 10'000'000, 60, 1)}, 2);
  const auto played = ons(notes);
  REQUIRE_EQ(frames_of(played), (std::vector<std::uint64_t>{0, 2'000, 4'000, 6'000}),
             "three hits share the first step");
  REQUIRE_EQ(played[0].velocity, 127, "later hits play softer");
  REQUIRE_EQ(played[1].velocity, 102, "later hits play softer");
  REQUIRE_EQ(played[2].velocity, 102, "later hits play softer");
  REQUIRE_EQ(played[3].velocity, 127, "later hits play softer");
  for (const auto& note : notes) {
    if (!note.on && note.frame < 6'000) {
      REQUIRE_EQ(note.frame % 2'000, 1'200, "a hit's gate is the gate of its share");
    }
  }
  require_paired(notes, false, "ratchets");
}

void conditions_follow_the_lane_cycle() {
  auto arp = arp_with(ArpMode::up);
  set_lane(
      arp, ArpLane::condition,
      {static_cast<double>(ArpCondition::one_of_two), static_cast<double>(ArpCondition::fill)});
  const std::vector<Press> note{hold(0, 10'000'000, 60, 1)};
  REQUIRE_EQ(frames_of(ons(play(arp, note, 8))), (std::vector<std::uint64_t>{0, 24'000}),
             "1:2 plays every other pass, Fill rests without a fill");
  arp.fill = true;
  REQUIRE_EQ(frames_of(ons(play(arp, note, 8))),
             (std::vector<std::uint64_t>{0, 6'000, 18'000, 24'000, 30'000, 42'000}),
             "Fill steps play while Fill is held");
}

void chance_is_seeded() {
  auto arp = arp_with(ArpMode::up);
  arp.seed = 3;
  arp.lane(ArpLane::chance).values.fill(0.5);
  const std::vector<Press> note{hold(0, 10'000'000, 60, 1)};
  const auto first = frames_of(ons(play(arp, note, 64)));
  REQUIRE_EQ(first, frames_of(ons(play(arp, note, 64))), "the same seed rolls the same steps");
  REQUIRE_GE(first.size(), 16, "about half the steps play at 50 %");
  REQUIRE_LE(first.size(), 48, "about half the steps play at 50 %");
  arp.seed = 4;
  REQUIRE_NE(first, frames_of(ons(play(arp, note, 64))), "another seed rolls differently");
}

void euclid_places_its_hits() {
  auto arp = arp_with(ArpMode::euclid);
  const auto played = ons(play(arp, c_major, 16));
  REQUIRE_EQ(frames_of(played),
             (std::vector<std::uint64_t>{0, 4 * step_frames, 7 * step_frames, 10 * step_frames,
                                         13 * step_frames}),
             "five hits spread over sixteen steps");
  REQUIRE_EQ(keys(played), (std::vector<int>{60, 64, 67, 60, 64}), "the notes move on hits only");
  REQUIRE_TRUE(euclid_hit(3, 5, 16, 4), "rotate turns the ring");
  REQUIRE_TRUE(!euclid_hit(1, 5, 16, 4), "rotate turns the ring");
  REQUIRE_EQ(arp_period(arp), 16, "a Euclidean pattern's period includes its cycle");
}

void passing_notes_fill_the_scale() {
  auto arp = arp_with(ArpMode::up);
  arp.scale = 1; // Major
  arp.root = 0;  // C
  arp.passing = true;
  REQUIRE_EQ(arp_scales[arp.scale].name, "Major", "the major scale");
  const auto presses = join({hold(0, 10'000'000, 48, 1), hold(0, 10'000'000, 55, 2)});
  REQUIRE_EQ(keys(ons(play(arp, presses, 6))), (std::vector<int>{48, 50, 52, 53, 55, 48}),
             "the scale's tones between the held notes join them");
}

void note_lane_moves_by_held_notes_or_degrees() {
  auto held = arp_with(ArpMode::up);
  set_lane(held, ArpLane::note, {1});
  REQUIRE_EQ(keys(ons(play(held, c_major, 3))), (std::vector<int>{64, 67, 60}),
             "+1 is the next held note");
  auto degrees = arp_with(ArpMode::up);
  degrees.note_mode = ArpNoteMode::scale;
  degrees.scale = 2; // Minor
  REQUIRE_EQ(arp_scales[degrees.scale].name, "Minor", "the minor scale");
  set_lane(degrees, ArpLane::note, {0, 1, 2, -1});
  REQUIRE_EQ(keys(ons(play(degrees, {hold(0, 10'000'000, 60, 1)}, 4))),
             (std::vector<int>{60, 62, 63, 58}), "scale degrees of C minor");
}

void chord_lane_stacks_notes() {
  auto arp = arp_with(ArpMode::up);
  set_lane(arp, ArpLane::chord, {static_cast<double>(ArpChord::triad)});
  const auto stacked = ons(play(arp, c_major, 2));
  REQUIRE_EQ(keys(stacked), (std::vector<int>{60, 64, 67, 64, 67, 72}),
             "a triad from the held chord's tones");
  REQUIRE_EQ(stacked[2].frame, 0, "stacked notes share a step");
  REQUIRE_EQ(stacked[3].frame, step_frames, "stacked notes share a step");
  auto diatonic = arp_with(ArpMode::up);
  diatonic.scale = 1; // Major
  set_lane(diatonic, ArpLane::chord, {static_cast<double>(ArpChord::seventh)});
  REQUIRE_EQ(keys(ons(play(diatonic, {hold(0, 10'000'000, 60, 1)}, 1))),
             (std::vector<int>{60, 64, 67, 71}), "a diatonic seventh on C");
  auto fifth = arp_with(ArpMode::up);
  set_lane(fifth, ArpLane::chord, {static_cast<double>(ArpChord::fifth)});
  REQUIRE_EQ(keys(ons(play(fifth, {hold(0, 10'000'000, 60, 1)}, 1))), (std::vector<int>{60, 67}),
             "a fifth over the note");
}

void groove_moves_steps_and_velocity() {
  auto arp = arp_with(ArpMode::up);
  arp.velocity = ArpVelocity::pattern;
  arp.groove.steps = 2;
  arp.groove.grid = arp_default_groove_grid; // 1/16
  arp.groove.timing[1] = 0.2;
  arp.groove.velocity[1] = 0.5;
  const std::vector<Press> note{hold(0, 10'000'000, 60, 1)};
  const auto full = ons(play(arp, note, 3));
  REQUIRE_EQ(frames_of(full), (std::vector<std::uint64_t>{0, 7'200, 12'000}),
             "the groove's late step is late by its timing of a groove step");
  REQUIRE_EQ(full[1].velocity, 64, "the groove's velocity");
  REQUIRE_EQ(full[2].velocity, 127, "the groove's velocity");
  arp.groove.amount = 0.5;
  const auto half = ons(play(arp, note, 2));
  REQUIRE_EQ(half[1].frame, 6'600, "the amount scales both");
  REQUIRE_EQ(half[1].velocity, 95, "the amount scales both");
}

void accent_and_mod_ride_on_notes() {
  auto arp = arp_with(ArpMode::up);
  arp.velocity = ArpVelocity::pattern;
  set_lane(arp, ArpLane::velocity, {0.5});
  set_lane(arp, ArpLane::accent, {1, 0});
  set_lane(arp, ArpLane::mod, {0.25, 0.75});
  const auto played = ons(play(arp, {hold(0, 10'000'000, 60, 1)}, 2));
  REQUIRE_TRUE(played[0].accent, "an accented step: louder, flagged, its mod value");
  REQUIRE_EQ(played[0].velocity, 89, "an accented step: louder, flagged, its mod value");
  REQUIRE_EQ(played[0].mod, 0.25F, "an accented step: louder, flagged, its mod value");
  REQUIRE_TRUE(!played[1].accent, "the next step's mod value");
  REQUIRE_EQ(played[1].velocity, 64, "the next step's mod value");
  REQUIRE_EQ(played[1].mod, 0.75F, "the next step's mod value");
}

void variation_bars_play_the_fill() {
  auto arp = arp_with(ArpMode::up);
  arp.variation = 2;
  set_lane(arp, ArpLane::condition, {static_cast<double>(ArpCondition::fill)});
  bool varied = false;
  NoteGenerator generator;
  generator.prepare(sample_rate);
  generator.set(arp, {});
  const auto played = ons(render(generator, {{hold(0, 10'000'000, 60, 1)},
                                             32 * step_frames,
                                             Song{},
                                             {static_cast<std::uint32_t>(step_frames)},
                                             [&](std::uint64_t frame, NoteGenerator& g) {
                                               if (frame == 20 * step_frames) {
                                                 varied = g.arp_variation();
                                               }
                                             }}));
  REQUIRE_EQ(played.size(), 16, "only the second bar of two plays its Fill steps");
  REQUIRE_EQ(played.front().frame, 16 * step_frames,
             "only the second bar of two plays its Fill steps");
  require(varied, "telemetry marks the variation bar");
}

void processing_does_not_allocate() {
  project::NativeSynthDocument document;
  document.sequences.push_back(
      sequence("clip.1", 2.0, {seq_note(0.0, 0.5, 60), seq_note(1.0, 0.5, 67)}));
  const auto clips = compile_sequencer_clips(document);
  SequencerSettings sequencer;
  sequencer.on = true;
  auto arp = arp_with(ArpMode::played);
  arp.octaves = 4;
  NoteGenerator generator;
  generator.prepare(sample_rate);
  generator.adopt(clips);
  generator.set(arp, sequencer);
  counted_allocations = 0;
  (void)render(
      generator,
      {join({hold(0, 50'000, 40, 1), hold(10, 60'000, 43, 2)}), 100'000, Song{140.0, 2.0}, {333}});
  REQUIRE_EQ(counted_allocations, 0, "the callback never allocates");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "nova arpeggiator contract passed",
      {
          {"compiles_document_clips", [] { compiles_document_clips(); }},
          {"modes_play_in_order", [] { modes_play_in_order(); }},
          {"steps_lock_to_the_beat_grid", [] { steps_lock_to_the_beat_grid(); }},
          {"swing_delays_every_second_step", [] { swing_delays_every_second_step(); }},
          {"gate_lengths_follow_gate_times_step_gate",
           [] { gate_lengths_follow_gate_times_step_gate(); }},
          {"ties_hold_into_the_next_step", [] { ties_hold_into_the_next_step(); }},
          {"octaves_extend_the_pattern", [] { octaves_extend_the_pattern(); }},
          {"latch_holds_until_a_new_chord", [] { latch_holds_until_a_new_chord(); }},
          {"block_splits_do_not_change_notes", [] { block_splits_do_not_change_notes(); }},
          {"seek_reproduces_the_song_position", [] { seek_reproduces_the_song_position(); }},
          {"every_on_has_one_off", [] { every_on_has_one_off(); }},
          {"sequencer_loops_with_the_transport", [] { sequencer_loops_with_the_transport(); }},
          {"note_trigger_quantises_to_a_sixteenth",
           [] { note_trigger_quantises_to_a_sixteenth(); }},
          {"follow_transposes_by_the_held_key", [] { follow_transposes_by_the_held_key(); }},
          {"sequencer_feeds_the_arpeggiator", [] { sequencer_feeds_the_arpeggiator(); }},
          {"passes_through_when_off", [] { passes_through_when_off(); }},
          {"telemetry_reports_steps", [] { telemetry_reports_steps(); }},
          {"new_modes_play_in_order", [] { new_modes_play_in_order(); }},
          {"lane_lengths_make_a_polymeter", [] { lane_lengths_make_a_polymeter(); }},
          {"ratchets_split_a_step", [] { ratchets_split_a_step(); }},
          {"conditions_follow_the_lane_cycle", [] { conditions_follow_the_lane_cycle(); }},
          {"chance_is_seeded", [] { chance_is_seeded(); }},
          {"euclid_places_its_hits", [] { euclid_places_its_hits(); }},
          {"passing_notes_fill_the_scale", [] { passing_notes_fill_the_scale(); }},
          {"note_lane_moves_by_held_notes_or_degrees",
           [] { note_lane_moves_by_held_notes_or_degrees(); }},
          {"chord_lane_stacks_notes", [] { chord_lane_stacks_notes(); }},
          {"groove_moves_steps_and_velocity", [] { groove_moves_steps_and_velocity(); }},
          {"accent_and_mod_ride_on_notes", [] { accent_and_mod_ride_on_notes(); }},
          {"variation_bars_play_the_fill", [] { variation_bars_play_the_fill(); }},
          {"processing_does_not_allocate", [] { processing_does_not_allocate(); }},
      });
}
