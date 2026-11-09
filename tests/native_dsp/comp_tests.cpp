#include "oscillate/engine/midi_events.hpp"
#include "oscillate/native_dsp/note_effects/note_effect.hpp"
#include "oscillate/native_inserts/synth_player.hpp"
#include "oscillate/native_inserts/tempo_ticks.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/timeline/timing.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// Comp (ADR-0083, ADR-0084): hand-worked oracles at 120 BPM and 48 kHz,
// where a 16th is exactly 6,000 frames and a frame exactly 40,000 ticks.
// Comp plays the notes held into it; every result is also checked under
// other block splits, with the notes ahead of each block seen as playback
// sees them. Then the chain and the synth player: the same notes live and
// in export, and an edit while playing that goes live without a gap.
namespace {

namespace fx = oscillate::native_dsp::note_effects;
namespace engine = oscillate::engine;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
using oscillate::foundation::IdBits;

using oscillate::test::require;

using oscillate::test::take;

constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
constexpr std::uint64_t sixteenth = 6'000;
constexpr std::uint64_t bar = 16 * sixteenth;
constexpr std::int64_t quarter = timeline::ticks_per_quarter;
constexpr std::int64_t step_ticks = quarter / 4;
constexpr std::int64_t bar_ticks = 4 * quarter;
constexpr IdBits played_clip{7, 7};
constexpr int on_velocity = 100;
constexpr int ghost_velocity = 60;

const timeline::TempoMap& tempo() {
  static const auto map =
      take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  return map;
}

// The clock source the player gives the note effects.
const oscillate::native_inserts::TempoTicks& ticks() {
  static const oscillate::native_inserts::TempoTicks source(tempo(), rate);
  return source;
}

// Chords as held notes (MIDI keys): the bass first.
// NOLINTBEGIN(bugprone-throwing-static-initialization): a throw fails the test at start-up.
const std::vector<int> am7{45, 55, 60, 64};   // A2 G3 C4 E4
const std::vector<int> dm7{50, 57, 60, 65};   // D3 A3 C4 F4
const std::vector<int> fmaj7{41, 57, 60, 64}; // F2 A3 C4 E4
// NOLINTEND(bugprone-throwing-static-initialization)

using Values = std::initializer_list<std::pair<std::string_view, double>>;

// Comp's values: Rhodes comp with no push, no humanise and a straight groove
// unless `set` says otherwise.
std::vector<double> values(Values set = {}) {
  const auto* found = fx::find_engine("comp");
  REQUIRE_NE(found, nullptr, "comp is registered");
  auto dense = oscillate::native_dsp::default_values(*found->descriptor);
  const auto put = [&](std::string_view parameter, double value) {
    const auto index = oscillate::native_dsp::find_parameter(*found->descriptor, parameter);
    REQUIRE_OK(index, std::string(parameter) + " exists");
    dense[*index] = value;
  };
  put("comp.push_chance", 0.0);
  put("comp.humanise", 0.0);
  put("comp.groove", 0.0);
  for (const auto& [parameter, value] : set)
    put(parameter, value);
  return dense;
}

// One lane set to `hits` (`x` on, `o` ghost), the others silent.
using Lanes = std::vector<std::pair<std::string, double>>;
Lanes only(std::string_view lane, std::string_view hits);
std::vector<double> with(const Lanes& lanes, Values more = {});

struct Note {
  std::uint64_t frame = 0;
  bool on = false;
  int key = 0;
  int velocity = 0;
  IdBits clip;
  IdBits note;
  bool operator==(const Note&) const = default;
};

engine::BlockMidiEvent block_event(const Note& note, std::uint64_t first) {
  return {static_cast<std::uint32_t>(note.frame - first),
          note.clip,
          note.note,
          note.on ? engine::MidiEventKind::note_on : engine::MidiEventKind::note_off,
          static_cast<std::uint8_t>(note.key),
          static_cast<std::uint8_t>(note.velocity),
          false};
}

bool event_less(const engine::BlockMidiEvent& x, const engine::BlockMidiEvent& y) {
  return std::tie(x.offset, x.kind, x.note_id_bits) < std::tie(y.offset, y.kind, y.note_id_bits);
}

// The held notes as a compiled plan schedules them, for the lookahead.
std::vector<engine::ScheduledMidiEvent> scheduled(const std::vector<Note>& input) {
  std::vector<engine::ScheduledMidiEvent> events;
  events.reserve(input.size());
  for (const auto& note : input)
    events.push_back({{static_cast<std::int64_t>(note.frame)},
                      take(project::ClipId::from_bits(note.clip)),
                      take(project::NoteId::from_bits(note.note)),
                      note.on ? engine::MidiEventKind::note_on : engine::MidiEventKind::note_off,
                      static_cast<std::uint8_t>(note.key),
                      static_cast<std::uint8_t>(note.velocity),
                      false});
  std::ranges::sort(
      events, [](const engine::ScheduledMidiEvent& x, const engine::ScheduledMidiEvent& y) {
        return std::tie(x.sample.value, x.kind, x.note) < std::tie(y.sample.value, y.kind, y.note);
      });
  return events;
}

// Runs Comp over `input` for `total` frames in blocks of `block` (0:
// irregular blocks), returning every event it writes. Each block sees the
// input ahead of it, unless `blind`.
std::vector<Note> run(const std::vector<double>& dense, const std::vector<Note>& input,
                      std::uint64_t total, std::uint32_t block, bool blind = false) {
  auto kernel = fx::find_engine("comp")->create_kernel({0xF00D, 1});
  const auto plan = scheduled(input);
  std::vector<engine::BlockMidiEvent> in;
  std::vector<engine::BlockMidiEvent> out(fx::block_event_capacity);
  std::vector<fx::AheadEvent> ahead(fx::ahead_event_capacity);
  std::vector<Note> result;
  const std::uint32_t irregular[] = {1, 127, 4'096, 64, 999, 2, 513, 8'192};
  std::size_t turn = 0;
  for (std::uint64_t frame = 0; frame < total;) {
    const std::uint32_t size = block != 0 ? block : irregular[turn++ % std::size(irregular)];
    const auto frames = static_cast<std::uint32_t>(std::min<std::uint64_t>(size, total - frame));
    in.clear();
    for (const auto& note : input) {
      if (note.frame >= frame && note.frame < frame + frames)
        in.push_back(block_event(note, frame));
    }
    std::ranges::sort(in, event_less);
    const fx::NoteClock clock(ticks(), frame, frames);
    require(clock.valid(), "the clock places the block");
    const auto seen = blind ? 0 : fx::copy_ahead(plan, clock, ahead);
    const fx::NoteParameters parameters(dense, {}, std::span(ahead).first(seen));
    const auto count = kernel->process(in, out, clock, parameters);
    for (std::size_t i = 0; i < count; ++i) {
      const auto& event = out[i];
      REQUIRE_LT(event.offset, frames, "events stay in their block");
      result.push_back({frame + event.offset, event.kind == engine::MidiEventKind::note_on,
                        event.key, event.velocity, event.clip_id_bits, event.note_id_bits});
    }
    frame += frames;
  }
  return result;
}

// The same run under several block splits must agree exactly.
std::vector<Note> run_split(const std::vector<double>& dense, const std::vector<Note>& input,
                            std::uint64_t total) {
  const auto reference = run(dense, input, total, 128);
  for (const std::uint32_t block : {64U, 100U, 1'000U, 8'192U, 0U})
    REQUIRE_EQ(run(dense, input, total, block), reference,
               "Comp plays the same notes under any block split");
  return reference;
}

// `keys` held from `from` to `to`, note ids from `first_id`.
std::vector<Note> chord(const std::vector<int>& keys, std::uint64_t begin, std::uint64_t end,
                        std::uint64_t first_id) {
  std::vector<Note> notes;
  for (std::size_t i = 0; i < keys.size(); ++i) {
    notes.push_back({begin, true, keys[i], 90, played_clip, {0, first_id + i}});
    notes.push_back({end, false, keys[i], 0, played_clip, {0, first_id + i}});
  }
  return notes;
}

std::vector<Note> joined(std::vector<Note> x, const std::vector<Note>& y) {
  x.insert(x.end(), y.begin(), y.end());
  return x;
}

using Hits = std::multiset<std::tuple<std::uint64_t, int, int>>; // (frame, key, velocity)

Hits ons(const std::vector<Note>& events) {
  Hits hits;
  for (const auto& event : events) {
    if (event.on)
      hits.insert({event.frame, event.key, event.velocity});
  }
  return hits;
}
Hits ons_at(const std::vector<Note>& events, std::uint64_t frame) {
  Hits hits;
  for (const auto& hit : ons(events)) {
    if (std::get<0>(hit) == frame)
      hits.insert(hit);
  }
  return hits;
}

// Every note-on is ended by a note-off of the same id and key, once.
void every_note_ends(const std::vector<Note>& events) {
  std::multiset<std::tuple<std::uint64_t, std::uint64_t, int>> open;
  for (const auto& event : events) {
    const auto id = std::tuple{event.note.high, event.note.low, event.key};
    if (event.clip != IdBits{0xF00D, 1})
      continue;
    if (event.on) {
      require(!open.contains(id), "a note is never started twice");
      open.insert(id);
    } else {
      require(open.contains(id), "a note-off ends a note Comp started");
      open.erase(open.find(id));
    }
  }
  require(open.empty(), "no note is left sounding");
}

Hits expected(std::uint64_t frame, std::initializer_list<int> keys, int velocity) {
  Hits hits;
  for (const int key : keys)
    hits.insert({frame, key, velocity});
  return hits;
}

Hits keyed(std::uint64_t frame, std::span<const int> keys, int velocity) {
  Hits hits;
  for (const int key : keys)
    hits.insert({frame, key, velocity});
  return hits;
}

// Rhodes comp over one bar of Am7 held: the lowest held note (the bass) on 1
// and 3, the inner notes and the top off the beat on the 4th and 11th 16ths
// with ghosts of the inner ones on the 7th and 14th, each three 16ths long.
void comp_plays_held_notes_on_its_lanes() {
  const auto events = run_split(values(), chord(am7, 0, bar, 1), bar + sixteenth);
  const std::vector<int> inner{55, 60};
  const std::vector<int> upper{55, 60, 64};
  REQUIRE_EQ(ons_at(events, 0), expected(0, {45}, on_velocity), "the bass on the downbeat");
  REQUIRE_EQ(ons_at(events, 8 * sixteenth), expected(8 * sixteenth, {45}, on_velocity),
             "and on beat 3");
  REQUIRE_EQ(ons_at(events, 3 * sixteenth), keyed(3 * sixteenth, upper, on_velocity),
             "the inner notes and the top note answer on the 4th 16th");
  REQUIRE_EQ(ons_at(events, 6 * sixteenth), keyed(6 * sixteenth, inner, ghost_velocity),
             "a ghost of the inner notes on the 7th");
  REQUIRE_EQ(ons_at(events, 10 * sixteenth), keyed(10 * sixteenth, upper, on_velocity),
             "and the chord again on the 11th");
  REQUIRE_EQ(ons_at(events, 13 * sixteenth), keyed(13 * sixteenth, inner, ghost_velocity),
             "and a ghost on the 14th");
  REQUIRE_EQ(ons(events).size(), 12, "nothing else plays");
  for (const auto& event : events) {
    if (!event.on)
      continue;
    const auto off = std::ranges::find_if(
        events, [&](const Note& other) { return !other.on && other.note == event.note; });
    REQUIRE_NE(off, events.end(), "each hit lasts its three 16ths");
    REQUIRE_EQ(off->frame, event.frame + 3 * sixteenth, "each hit lasts its three 16ths");
  }
  every_note_ends(events);
  // Two notes are a bass and a top; one is the bass alone.
  const auto two = run_split(values(), chord({48, 67}, 0, bar, 1), bar);
  REQUIRE_EQ(ons_at(two, 3 * sixteenth), expected(3 * sixteenth, {67}, on_velocity),
             "two held notes: the top answers, no middle");
  REQUIRE_TRUE(ons_at(two, 6 * sixteenth).empty(), "two held notes: the top answers, no middle");
  const auto one = run_split(values(), chord({48}, 0, bar, 1), bar);
  REQUIRE_EQ(ons(one).size(), 2, "one held note: the low lane only");
  REQUIRE_EQ(ons_at(one, 0), expected(0, {48}, on_velocity), "one held note: the low lane only");
  require(run_split(values(), {}, bar).empty(), "nothing held, nothing plays");
}

// Held notes are Comp's input and do not sound; a note-off Comp does not
// hold passes through; nothing plays once the keys are let go.
void comp_holds_its_input_and_passes_strangers() {
  auto input = chord(am7, 0, bar, 1);
  input.push_back({2 * sixteenth, false, 70, 0, played_clip, {0, 99}});
  const auto played = run_split(values(), input, 2 * bar);
  REQUIRE_EQ(
      std::ranges::count_if(played, [](const Note& event) { return event.clip == played_clip; }), 1,
      "only the stranger's note-off is heard of the input");
  for (const auto& [frame, key, velocity] : ons(played))
    REQUIRE_LT(frame, bar, "nothing after the keys are let go");
}

// Am7 then Dm7 a bar later. Pushed, the Dm7 lands an 8th early (the 15th
// 16th, where the lanes are empty, so every voice plays, two 16ths longer)
// and the hit on bar 2's downbeat is dropped. The first chord never pushes
// before the song; a chord struck again is no change; a change Comp cannot
// see coming (no notes ahead, as when played live) is never dropped.
void comp_pushes_changes_by_chance() {
  const auto input = joined(chord(am7, 0, bar, 1), chord(dm7, bar, 2 * bar, 11));
  const auto straight = run_split(values(), input, 2 * bar);
  require(ons_at(straight, 14 * sixteenth).empty(), "unpushed, nothing on the 15th");
  REQUIRE_EQ(ons_at(straight, bar), expected(bar, {50}, on_velocity),
             "unpushed, the Dm7's bass on its downbeat");
  // Run on past bar 2, so its last hits end too.
  const auto push = values({{"comp.push_chance", 1.0}});
  const auto pushed = run_split(push, input, 2 * bar + bar / 4);
  const int pushed_velocity = static_cast<int>(std::lround(on_velocity * 0.9));
  REQUIRE_EQ(ons_at(pushed, 14 * sixteenth), keyed(14 * sixteenth, dm7, pushed_velocity),
             "pushed, the whole Dm7 lands an 8th early");
  REQUIRE_TRUE(ons_at(pushed, 15 * sixteenth).empty(), "and the downbeat hit is dropped");
  REQUIRE_TRUE(ons_at(pushed, bar).empty(), "and the downbeat hit is dropped");
  REQUIRE_EQ(ons_at(pushed, 0), expected(0, {45}, on_velocity),
             "the first chord is not pushed before the song starts");
  const auto off = std::ranges::find_if(pushed, [&](const Note& event) {
    return !event.on && event.key == 50 && event.frame > 14 * sixteenth;
  });
  REQUIRE_NE(off, pushed.end(), "the push lasts its length plus the push");
  REQUIRE_EQ(off->frame, 14 * sixteenth + 5 * sixteenth, "the push lasts its length plus the push");
  every_note_ends(pushed);
  const auto sixteenth_push =
      run_split(values({{"comp.push_chance", 1.0}, {"comp.push_amount", 0}}), input, 2 * bar);
  REQUIRE_EQ(ons_at(sixteenth_push, 15 * sixteenth), keyed(15 * sixteenth, dm7, pushed_velocity),
             "a 16th push lands on the 16th");
  REQUIRE_TRUE(ons_at(sixteenth_push, 14 * sixteenth).empty(), "a 16th push lands on the 16th");
  const auto dotted = run_split(
      with(only("low", "x"), {{"comp.push_chance", 1.0}, {"comp.push_amount", 2}}), input, 2 * bar);
  REQUIRE_EQ(ons_at(dotted, 13 * sixteenth), keyed(13 * sixteenth, dm7, pushed_velocity),
             "a dotted-8th push, the longest, is seen coming too");
  const auto again =
      run_split(push, joined(chord(am7, 0, bar, 1), chord(am7, bar, 2 * bar, 11)), 2 * bar);
  REQUIRE_TRUE(ons_at(again, 14 * sixteenth).empty(), "the same chord struck again is not pushed");
  REQUIRE_EQ(ons_at(again, bar), expected(bar, {45}, on_velocity),
             "the same chord struck again is not pushed");
  const auto unseen = run(push, input, 2 * bar, 128, true);
  REQUIRE_TRUE(ons_at(unseen, 14 * sixteenth).empty(),
               "a change not seen coming plays on its beat");
  REQUIRE_EQ(ons_at(unseen, bar), expected(bar, {50}, on_velocity),
             "a change not seen coming plays on its beat");
}

Lanes only(std::string_view lane, std::string_view hits) {
  Lanes set;
  for (const std::string_view name : {"low", "mid", "top"}) {
    for (std::size_t step = 0; step < 16; ++step) {
      double value = 0.0;
      if (name == lane && step < hits.size())
        value = hits[step] == 'x' ? 2.0 : hits[step] == 'o' ? 1.0 : 0.0;
      set.emplace_back("comp." + std::string(name) + "." + std::to_string(step + 1), value);
    }
  }
  return set;
}

std::vector<double> with(const Lanes& lanes, Values more) {
  auto dense = values(more);
  const auto* found = fx::find_engine("comp");
  for (const auto& [parameter, value] : lanes)
    dense[oscillate::test::take(
        oscillate::native_dsp::find_parameter(*found->descriptor, parameter))] = value;
  return dense;
}

// The top lane cycles every three 16ths against the bar: the highest held
// note in a polymeter over the bass.
void comp_top_lane_runs_its_own_length() {
  const auto events = run_split(with(only("top", "x"), {{"comp.top_length", 3}}),
                                chord(am7, 0, 2 * bar, 1), 2 * bar);
  std::set<std::uint64_t> frames;
  for (const auto& [frame, key, velocity] : ons(events)) {
    REQUIRE_EQ(key, 64, "the top lane plays the highest held note");
    frames.insert(frame);
  }
  std::set<std::uint64_t> wanted;
  for (std::uint64_t step = 0; step < 32; step += 3)
    wanted.insert(step * sixteenth);
  REQUIRE_EQ(frames, wanted, "the top note plays every three 16ths, across the barline");
}

// Am7 for two beats, then Fmaj7: the hits from beat 3 play the F chord,
// its F bass on the low lane.
void comp_changes_chord_mid_bar() {
  const auto input = joined(chord(am7, 0, bar / 2, 1), chord(fmaj7, bar / 2, bar, 11));
  const auto events = run_split(values(), input, bar);
  REQUIRE_EQ(ons_at(events, 3 * sixteenth),
             keyed(3 * sixteenth, std::vector{55, 60, 64}, on_velocity),
             "the A minor chord before the change");
  REQUIRE_EQ(ons_at(events, 8 * sixteenth), expected(8 * sixteenth, {41}, on_velocity),
             "the F chord's bass on beat 3");
  REQUIRE_EQ(ons_at(events, 10 * sixteenth),
             keyed(10 * sixteenth, std::vector{57, 60, 64}, on_velocity),
             "and its chord off the beat");
}

// Grooves swing every second 16th late; humanise only ever delays, by the
// same amount on every pass.
void comp_swings_and_humanises_late_only() {
  const auto held = chord(am7, 0, bar, 1);
  const auto swung =
      run_split(with(only("top", "xxxxxxxxxxxxxxxx"), {{"comp.groove", 4}}), held, bar);
  std::vector<std::uint64_t> frames;
  for (const auto& [frame, key, velocity] : ons(swung))
    frames.push_back(frame);
  REQUIRE_EQ(frames.size(), 16, "every step plays");
  const auto late = static_cast<std::uint64_t>(std::llround((2 * 0.66 - 1) * sixteenth));
  for (std::size_t step = 0; step < 16; ++step)
    REQUIRE_EQ(frames[step], step * sixteenth + (step % 2 == 1 ? late : 0),
               "the 66% groove moves each second 16th late");
  const auto human = values({{"comp.humanise", 1.0}});
  const auto first = run_split(human, held, bar + sixteenth);
  REQUIRE_EQ(first, run(human, held, bar + sixteenth, 333), "humanise repeats exactly");
  const auto straight = ons(run(values(), held, bar + sixteenth, 128));
  auto moved = ons(first);
  REQUIRE_EQ(moved.size(), straight.size(), "humanise plays the same hits");
  auto s = straight.begin();
  for (auto m = moved.begin(); m != moved.end(); ++m, ++s)
    require(std::get<0>(*m) >= std::get<0>(*s) &&
                std::get<0>(*m) - std::get<0>(*s) <=
                    static_cast<std::uint64_t>(0.3 * sixteenth) + 1,
            "and only ever late, by under a third of a 16th");
}

// copy_ahead hands over the plan's events after the block whose frame
// before them is still within a dotted 8th of the block's last tick, each
// with that frame's tick, and no more than fit.
void the_lookahead_sees_a_dotted_eighth() {
  constexpr std::uint64_t ticks_per_frame = 40'000;
  const std::vector<Note> input{{100, true, 60, 90, played_clip, {0, 1}}, // In the block.
                                {128, true, 62, 90, played_clip, {0, 2}}, // Its first frame after.
                                {128 + 17'999, true, 64, 90, played_clip, {0, 3}}, // The last seen.
                                {128 + 18'000, true, 65, 90, played_clip, {0, 4}}};
  const auto plan = scheduled(input);
  std::vector<fx::AheadEvent> ahead(fx::ahead_event_capacity);
  const fx::NoteClock clock(ticks(), 0, 128);
  const auto seen = fx::copy_ahead(plan, clock, ahead);
  REQUIRE_EQ(seen, 2, "the block's own events and those past a dotted 8th are not seen");
  REQUIRE_EQ(ahead[0].key, 62, "the first event after the block carries the block's last tick");
  REQUIRE_EQ(ahead[0].previous_tick, 127 * static_cast<std::int64_t>(ticks_per_frame),
             "the first event after the block carries the block's last tick");
  REQUIRE_EQ(ahead[1].key, 64, "and each its frame-before's tick");
  REQUIRE_EQ(ahead[1].previous_tick,
             static_cast<std::int64_t>((128 + 17'999 - 1) * ticks_per_frame),
             "and each its frame-before's tick");
  REQUIRE_EQ(fx::note_lookahead_ticks, 18'000 * static_cast<std::int64_t>(ticks_per_frame),
             "a dotted 8th at 120 BPM is 18,000 frames");
  REQUIRE_EQ(fx::copy_ahead(plan, clock, std::span(ahead).first(1)), 1, "no more than fit");
}

// --- The chain and the player -------------------------------------------

template <typename Id> Id make_id(std::uint64_t value) { return take(Id::from_bits({0, value})); }

// The chain hands each stage the notes ahead: a Comp in a chain pushes.
void the_chain_hands_stages_the_notes_ahead() {
  const std::vector<project::NoteEffectInsert> inserts{
      {take(project::InsertSlotId::from_bits({0, 1})), true, "comp", 1, {}}};
  auto chain = take(fx::NoteEffectChain::create(inserts));
  const auto dense = values({{"comp.push_chance", 1.0}});
  const auto input = joined(chord(am7, 0, bar, 1), chord(dm7, bar, 2 * bar, 11));
  const auto plan = scheduled(input);
  std::vector<engine::BlockMidiEvent> events(fx::block_event_capacity);
  std::vector<engine::BlockMidiEvent> scratch(fx::block_event_capacity);
  std::vector<fx::AheadEvent> ahead(fx::ahead_event_capacity);
  std::vector<Note> heard;
  for (std::uint64_t frame = 0; frame < 2 * bar; frame += 128) {
    std::size_t count = 0;
    for (const auto& note : input) {
      if (note.frame >= frame && note.frame < frame + 128)
        events[count++] = block_event(note, frame);
    }
    std::sort(events.begin(), events.begin() + static_cast<std::ptrdiff_t>(count), event_less);
    const fx::StageControls control{true, dense, {}};
    const fx::NoteClock clock(ticks(), frame, 128);
    const auto seen = fx::copy_ahead(plan, clock, ahead);
    const auto out = chain->process(events, count, scratch, clock, std::span(&control, 1),
                                    std::span(ahead).first(seen));
    for (const auto& event : out)
      heard.push_back({frame + event.offset, event.kind == engine::MidiEventKind::note_on,
                       event.key, event.velocity, event.clip_id_bits, event.note_id_bits});
  }
  const int pushed_velocity = static_cast<int>(std::lround(on_velocity * 0.9));
  REQUIRE_EQ(ons_at(heard, 14 * sixteenth), keyed(14 * sixteenth, dm7, pushed_velocity),
             "the chain's Comp pushes the change it sees coming");
  require(ons_at(heard, bar).empty(), "and drops its downbeat hit");
}

// A Prism track (fast release) with a Comp on `lanes` (low, middle and top
// on step 1 only, each `length` 16ths), over a clip of `chords` (keys, from
// bar 1, a bar each).
project::Snapshot comp_project(const std::vector<std::vector<int>>& chords, int length,
                               double push_chance, double velocity = 100.0,
                               std::uint64_t revision = 1) {
  auto map = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  project::Snapshot state{
      make_id<project::ProjectId>(1), revision, rate, std::move(map), std::move(meter), {}, {}};
  project::Track track{
      make_id<project::TrackId>(2),
      {},
      project::TrackKind::instrument,
      project::NativeInstrumentPatch{
          "prism", 1, {{"prism.amp.attack", 0.0005}, {"prism.amp.release", 0.005}}}};
  std::vector<project::NativeInstrumentParameter> pads{
      {"comp.push_chance", push_chance}, {"comp.humanise", 0.0}, {"comp.velocity", velocity}};
  for (const std::string_view lane : {"low", "mid", "top"}) {
    for (int step = 1; step <= 16; ++step)
      pads.push_back(
          {"comp." + std::string(lane) + "." + std::to_string(step), step == 1 ? 2.0 : 0.0});
  }
  for (int step = 1; step <= 16; ++step)
    pads.push_back({"comp.length." + std::to_string(step), static_cast<double>(length)});
  track.note_effects.push_back({make_id<project::InsertSlotId>(9), true, "comp", 1, pads});
  std::vector<project::MidiNote> notes;
  std::uint64_t id = 1;
  for (std::size_t index = 0; index < chords.size(); ++index) {
    const auto at = static_cast<std::int64_t>(index) * bar_ticks;
    for (const int key : chords[index])
      notes.push_back({make_id<project::NoteId>(id++),
                       {at},
                       {at + bar_ticks},
                       static_cast<std::uint8_t>(key),
                       90});
  }
  const auto length_ticks =
      static_cast<std::int64_t>(std::max<std::size_t>(chords.size(), 1)) * bar_ticks;
  track.midi_clips.emplace_back(make_id<project::ClipId>(5),
                                project::MusicalInterval{{0}, {length_ticks}},
                                timeline::TickPosition{length_ticks}, false, std::move(notes));
  state.tracks.push_back(std::move(track));
  state.next_insert_slot_serial = 10;
  return state;
}

// Below this a released note has died away (the release is exponential).
constexpr double silent = 1e-9;

double peak(const std::vector<double>& x, std::size_t begin, std::size_t end) {
  double level = 0.0;
  for (std::size_t i = begin; i < end && i < x.size(); ++i)
    level = std::max(level, std::abs(x[i]));
  return level;
}

std::vector<double> render(const project::Snapshot& state, std::uint32_t block,
                           std::uint64_t total) {
  const auto plan = take(engine::compile_midi_events(state));
  auto player = take(oscillate::native_inserts::NativeSynthPlayer::prepare(
      state, state.tracks[0], plan.instruments()[0], {}, 8'192,
      std::numeric_limits<std::uint64_t>::max()));
  std::vector<engine::StereoFrame> buffer;
  std::vector<double> out;
  for (std::uint64_t frame = 0; frame < total; frame += block) {
    const auto frames = std::min<std::uint64_t>(block, total - frame);
    buffer.assign(frames, {});
    require(player.render(buffer, frame, 1), "the block renders");
    for (const auto& sample : buffer)
      out.push_back(sample.left);
  }
  return out;
}

// The player plays a clip's chords through Comp, seeing the next chord
// coming from its own MIDI plan: a short hit on bar 1, silence, then the
// Dm7 pushed in an 8th early. The same audio under any block split, as
// export plays it.
void the_player_pushes_the_clip_chords() {
  const auto state = comp_project({am7, dm7}, 2, 1.0);
  const auto live = render(state, 128, 2 * bar);
  REQUIRE_GT(peak(live, 0, 1'000), 0.01, "the hit sounds from the downbeat");
  REQUIRE_LT(peak(live, 8 * sixteenth, 14 * sixteenth), silent, "then silence");
  REQUIRE_GT(peak(live, 14 * sixteenth, 14 * sixteenth + 1'000), 0.01,
             "the Dm7 sounds an 8th before bar 2");
  REQUIRE_EQ(render(state, 100, 2 * bar), live, "the same audio under any block split");
  REQUIRE_EQ(render(state, 8'192, 2 * bar), live, "the same audio under any block split");
  const auto unpushed = render(comp_project({am7, dm7}, 2, 0.0), 128, 2 * bar);
  REQUIRE_LT(peak(unpushed, 14 * sixteenth, bar), silent, "unpushed, it waits for bar 2");
  REQUIRE_GT(peak(unpushed, bar, bar + 1'000), 0.01, "unpushed, it waits for bar 2");
}

// A Comp edit while playing goes live in the next block: the pad that is
// sounding keeps sounding (no gap, no reset) and the edit is applied.
void a_comp_edit_goes_live_without_a_gap() {
  const auto before = comp_project({am7, am7}, 16, 0.0, 100.0, 1);
  const auto after = comp_project({am7, am7}, 16, 0.0, 80.0, 2);
  const auto plan = take(engine::compile_midi_events(before));
  auto player = take(oscillate::native_inserts::NativeSynthPlayer::prepare(
      before, before.tracks[0], plan.instruments()[0], {}, 128,
      std::numeric_limits<std::uint64_t>::max()));
  std::vector<engine::StereoFrame> buffer(128);
  std::vector<double> out;
  for (std::uint64_t frame = 0; frame < 2 * bar; frame += 128) {
    if (frame == bar / 2) {
      const auto edited = take(engine::compile_midi_events(after));
      auto offered = player.offer_plan(after, after.tracks[0], edited.instruments()[0], {},
                                       std::numeric_limits<std::uint64_t>::max());
      REQUIRE_OK(offered, "the edit is offered to the running player");
    }
    buffer.assign(128, {});
    require(player.render(buffer, frame, 1), "every block renders");
    for (const auto& sample : buffer)
      out.push_back(sample.left);
  }
  REQUIRE_EQ(player.applied_plan_revision(), 2, "the player took the edit");
  for (std::uint64_t window = 0; window + 1'000 <= 2 * bar; window += 1'000)
    REQUIRE_GT(peak(out, window, window + 1'000), 0.01,
               "sound in every millisecond window: no gap");
}

} // namespace

// Motion keeps a loop moving in 4-bar phrases over a held Am7: the chord's
// upper voices on the off-beats, the bass on the downbeat.
Lanes offbeat_chords() {
  Lanes set;
  for (const std::string_view name : {"low", "mid", "top"})
    for (std::size_t step = 0; step < 16; ++step) {
      const bool hit = name == "low" ? step == 0 : step % 4 == 2;
      set.emplace_back("comp." + std::string(name) + "." + std::to_string(step + 1),
                       hit ? 2.0 : 0.0);
    }
  return set;
}
// Bar `index`'s hits as (16th within the bar, key, velocity).
Hits bar_hits(const std::vector<Note>& events, std::uint64_t index) {
  Hits hits;
  for (const auto& [frame, key, level] : ons(events))
    if (frame >= index * bar && frame < (index + 1) * bar)
      hits.insert({(frame - index * bar) / sixteenth, key, level});
  return hits;
}
Hits without_velocity(const Hits& hits) {
  Hits out;
  for (const auto& [step, key, level] : hits)
    out.insert({step, key, 0});
  return out;
}

void comp_motion_keeps_a_loop_moving() {
  constexpr std::uint64_t phrases = 4;
  constexpr std::uint64_t phrase = 4;
  const auto held = chord(am7, 0, phrases * phrase * bar, 1);
  const auto length = phrases * phrase * bar;
  // Off by default: every bar plays the same.
  const auto plain = run_split(with(offbeat_chords()), held, length);
  for (std::uint64_t b = 1; b < phrases * phrase; ++b)
    REQUIRE_EQ(bar_hits(plain, b), bar_hits(plain, 0), "without Motion every bar is the same");

  // Turnaround: only each phrase's last bar changes, and the bass keeps its place.
  const auto turned =
      run_split(with(offbeat_chords(), {{"comp.phrase", 0}, {"comp.turnaround", 1}}), held, length);
  std::set<Hits> turns;
  for (std::uint64_t b = 0; b < phrases * phrase; ++b) {
    const auto hits = bar_hits(turned, b);
    require(hits.contains({0, 45, on_velocity}), "the bass stays on the downbeat");
    if (b % phrase != phrase - 1)
      REQUIRE_EQ(hits, bar_hits(plain, 0), "bars before the phrase's last play the pattern");
    else
      turns.insert(hits);
  }
  require(std::ranges::any_of(turns, [&](const Hits& hits) { return hits != bar_hits(plain, 0); }),
          "the phrase's last bar turns around");
  REQUIRE_EQ(
      run(with(offbeat_chords(), {{"comp.phrase", 0}, {"comp.turnaround", 1}}), held, length, 128),
      turned, "the same seed turns the same way");

  // Variation: some bars vary, never the bass or a bar's first chord hit.
  const auto varied = run_split(
      with(offbeat_chords(), {{"comp.phrase", 0}, {"comp.variation", 1.0}}), held, length);
  int changed = 0;
  for (std::uint64_t b = 0; b < phrases * phrase; ++b) {
    const auto hits = bar_hits(varied, b);
    changed += hits != bar_hits(plain, 0) ? 1 : 0;
    REQUIRE_TRUE(hits.contains({0, 45, on_velocity}),
                 "variation keeps the bass and the bar's first chord");
    REQUIRE_TRUE(hits.contains({2, 64, on_velocity}),
                 "variation keeps the bass and the bar's first chord");
  }
  REQUIRE_GT(changed, 0, "full variation changes some bars, not all");
  REQUIRE_LT(changed, static_cast<int>(phrases * phrase),
             "full variation changes some bars, not all");

  // Top drift: the second half of each phrase has another note on top.
  const auto drifted =
      run_split(with(offbeat_chords(), {{"comp.phrase", 0}, {"comp.drift", 1}}), held, length);
  REQUIRE_TRUE(bar_hits(drifted, 0).contains({2, 64, on_velocity}),
               "for the phrase's second half the lowest upper voice goes up an octave");
  REQUIRE_TRUE(bar_hits(drifted, 2).contains({2, 67, on_velocity}),
               "for the phrase's second half the lowest upper voice goes up an octave");
  REQUIRE_TRUE(!bar_hits(drifted, 2).contains({2, 55, on_velocity}),
               "for the phrase's second half the lowest upper voice goes up an octave");

  // Lift: the top voice doubled an octave up for the phrase's second half.
  const auto lifted =
      run_split(with(offbeat_chords(), {{"comp.phrase", 0}, {"comp.lift", 1}}), held, length);
  REQUIRE_TRUE(!bar_hits(lifted, 1).contains({2, 76, on_velocity}),
               "lift adds the top an octave up in the phrase's second half");
  REQUIRE_TRUE(bar_hits(lifted, 2).contains({2, 76, on_velocity}),
               "lift adds the top an octave up in the phrase's second half");
  REQUIRE_TRUE(bar_hits(lifted, 2).contains({2, 64, on_velocity}),
               "lift adds the top an octave up in the phrase's second half");

  // Articulation shortens every hit, never below a 32nd.
  const auto staccato =
      run_split(with(offbeat_chords(), {{"comp.articulation", 0.25}}), held, length);
  for (const auto& event : staccato) {
    if (!event.on)
      continue;
    const auto off = std::ranges::find_if(
        staccato, [&](const Note& other) { return !other.on && other.note == event.note; });
    REQUIRE_NE(off, staccato.end(), "articulation shortens the three-16th hits");
    REQUIRE_LT(off->frame - event.frame, 3 * sixteenth,
               "articulation shortens the three-16th hits");
    REQUIRE_GE(off->frame - event.frame, sixteenth / 2,
               "articulation shortens the three-16th hits");
  }

  // Swell: quieter at the phrase's turn than in its middle, same notes.
  const auto swelled =
      run_split(with(offbeat_chords(), {{"comp.phrase", 0}, {"comp.swell", 1.0}}), held, length);
  const auto level = [&](std::uint64_t b) {
    for (const auto& [step, key, velocity] : bar_hits(swelled, b))
      if (step == 2 && key == 64)
        return velocity;
    return 0;
  };
  REQUIRE_LT(level(0), level(2), "the swell peaks mid-phrase");
  REQUIRE_LE(level(2), on_velocity, "the swell peaks mid-phrase");
  REQUIRE_EQ(without_velocity(bar_hits(swelled, 1)), without_velocity(bar_hits(plain, 1)),
             "the swell changes only velocity");
}

// Density thins the chord's lanes towards the 8ths or fills ghost 16ths
// after hits, the same in every bar; the middle leaves the pattern alone.
void comp_density_thins_and_fills() {
  Lanes every_sixteenth;
  for (const std::string_view name : {"low", "mid", "top"})
    for (std::size_t step = 0; step < 16; ++step)
      every_sixteenth.emplace_back("comp." + std::string(name) + "." + std::to_string(step + 1),
                                   name == "low" ? (step == 0 ? 2.0 : 0.0) : 2.0);
  const auto held = chord(am7, 0, 4 * bar, 1);
  const auto thin = run_split(with(every_sixteenth, {{"comp.length.1", 1}, {"comp.density", 0.0}}),
                              held, 4 * bar);
  for (std::uint64_t b = 0; b < 4; ++b) {
    for (const auto& [step, key, velocity] : bar_hits(thin, b))
      REQUIRE_EQ(step % 2, 0, "at the bottom only the 8ths play");
    REQUIRE_EQ(bar_hits(thin, b), bar_hits(thin, 0), "and every bar agrees");
  }
  const auto middle = run_split(with(offbeat_chords(), {{"comp.density", 0.5}}), held, 4 * bar);
  const auto plain = run_split(with(offbeat_chords()), held, 4 * bar);
  REQUIRE_EQ(middle, plain, "the middle leaves the pattern as written");
  const auto full = run_split(with(offbeat_chords(), {{"comp.density", 1.0}}), held, 4 * bar);
  const auto ghosts = [&](std::uint64_t b) {
    int count = 0;
    for (const auto& [step, key, velocity] : bar_hits(full, b))
      count += velocity == ghost_velocity ? 1 : 0;
    return count;
  };
  REQUIRE_GT(ghosts(0), 0, "at the top ghost 16ths fill after hits");
  for (std::uint64_t b = 1; b < 4; ++b)
    REQUIRE_EQ(bar_hits(full, b), bar_hits(full, 0), "the same in every bar");
  for (const auto& [step, key, velocity] : bar_hits(full, 0))
    if (velocity == ghost_velocity)
      REQUIRE_EQ(step % 4, 3, "a fill follows its hit by a 16th");
}

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "Comp contract passed",
      {{"comp_plays_held_notes_on_its_lanes", comp_plays_held_notes_on_its_lanes},
       {"comp_holds_its_input_and_passes_strangers", comp_holds_its_input_and_passes_strangers},
       {"comp_pushes_changes_by_chance", comp_pushes_changes_by_chance},
       {"comp_top_lane_runs_its_own_length", comp_top_lane_runs_its_own_length},
       {"comp_changes_chord_mid_bar", comp_changes_chord_mid_bar},
       {"comp_swings_and_humanises_late_only", comp_swings_and_humanises_late_only},
       {"the_lookahead_sees_a_dotted_eighth", the_lookahead_sees_a_dotted_eighth},
       {"the_chain_hands_stages_the_notes_ahead", the_chain_hands_stages_the_notes_ahead},
       {"the_player_pushes_the_clip_chords", the_player_pushes_the_clip_chords},
       {"a_comp_edit_goes_live_without_a_gap", a_comp_edit_goes_live_without_a_gap},
       {"comp_motion_keeps_a_loop_moving", comp_motion_keeps_a_loop_moving},
       {"comp_density_thins_and_fills", comp_density_thins_and_fills}});
}
