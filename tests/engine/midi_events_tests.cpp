#include "oscillate/engine/midi_events.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace oscillate;
using namespace oscillate::engine;
using namespace oscillate::project;
using foundation::ErrorCode;
using foundation::Result;
constexpr auto quarter = timeline::ticks_per_quarter;

using oscillate::test::require;
using oscillate::test::take;
template <typename Id> Id id(std::uint64_t number) { return take(Id::from_bits({0, number})); }
timeline::TempoMap tempo(bool ramp = false) {
  if (ramp) {
    return take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::linear_bpm},
                                            {{quarter}, 60.0, timeline::TempoShape::step}},
                                           2));
  }
  return take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
}
timeline::MeterMap meter() { return take(timeline::MeterMap::create({{{0}, 4, 4}}, 1)); }
MidiNote note(std::uint64_t number, std::int64_t start = 0, std::int64_t end = quarter) {
  return {id<NoteId>(number), {start}, {end}, 60, 100};
}
MidiClip clip(std::uint64_t number, Placement placement, bool loop, std::vector<MidiNote> notes) {
  return {id<ClipId>(number), placement, {quarter}, loop, std::move(notes), 1};
}
Snapshot snapshot(timeline::TempoMap map, std::vector<MidiClip> clips) {
  return {id<ProjectId>(1),
          1,
          48'000,
          std::move(map),
          meter(),
          {Track{id<TrackId>(2), {}, TrackKind::instrument, {}, std::move(clips)}},
          {}};
}

void looping_order_and_half_open_blocks() {
  auto state = snapshot(tempo(), {clip(3, MusicalInterval{{0}, {2 * quarter}}, true, {note(4)})});
  const auto plan = take(compile_midi_events(state));
  REQUIRE_EQ(plan.instruments().size(), 1, "instrument plan missing");
  const auto& events = plan.instruments()[0].events();
  REQUIRE_EQ(events.size(), 4,
             "two quarter-note loop timing differs from hand-calculated 120 BPM samples");
  REQUIRE_EQ(events[0].sample.value, 0,
             "two quarter-note loop timing differs from hand-calculated 120 BPM samples");
  REQUIRE_EQ(events[1].sample.value, 24'000,
             "two quarter-note loop timing differs from hand-calculated 120 BPM samples");
  REQUIRE_EQ(events[2].sample.value, 24'000,
             "two quarter-note loop timing differs from hand-calculated 120 BPM samples");
  REQUIRE_EQ(events[3].sample.value, 48'000,
             "two quarter-note loop timing differs from hand-calculated 120 BPM samples");
  REQUIRE_EQ(events[1].kind, MidiEventKind::note_off,
             "same-sample loop boundary did not order off before on");
  REQUIRE_EQ(events[2].kind, MidiEventKind::note_on,
             "same-sample loop boundary did not order off before on");
  REQUIRE_EQ(events[1].note, events[2].note,
             "same-sample loop boundary did not order off before on");
  std::array<BlockMidiEvent, max_midi_events_per_block> storage{};
  auto before = plan.instruments()[0].copy_block({23'999}, 1, storage);
  REQUIRE_EQ(before.status, BlockMidiStatus::ready,
             "block-end event entered the preceding half-open block");
  REQUIRE_EQ(before.count, 0, "block-end event entered the preceding half-open block");
  auto boundary = plan.instruments()[0].copy_block({24'000}, 1, storage);
  REQUIRE_EQ(boundary.status, BlockMidiStatus::ready,
             "boundary block event offsets or ordering wrong");
  REQUIRE_EQ(boundary.count, 2, "boundary block event offsets or ordering wrong");
  REQUIRE_EQ(storage[0].offset, 0, "boundary block event offsets or ordering wrong");
  REQUIRE_EQ(storage[1].offset, 0, "boundary block event offsets or ordering wrong");
  REQUIRE_EQ(storage[0].kind, MidiEventKind::note_off,
             "boundary block event offsets or ordering wrong");
  REQUIRE_EQ(storage[1].kind, MidiEventKind::note_on,
             "boundary block event offsets or ordering wrong");
  storage[0].offset = 777;
  auto empty = plan.instruments()[0].copy_block({2'000}, 512, storage);
  REQUIRE_EQ(empty.status, BlockMidiStatus::ready, "zero-event block wrote caller storage");
  REQUIRE_EQ(empty.count, 0, "zero-event block wrote caller storage");
  REQUIRE_EQ(storage[0].offset, 777, "zero-event block wrote caller storage");
  REQUIRE_EQ(plan.instruments()[0].copy_block({0}, 0, storage).status,
             BlockMidiStatus::invalid_range, "invalid block range accepted");
  REQUIRE_EQ(plan.instruments()[0].copy_block({0}, 8'193, storage).status,
             BlockMidiStatus::invalid_range, "invalid block range accepted");
  REQUIRE_EQ(plan.instruments()[0]
                 .copy_block({std::numeric_limits<std::int64_t>::max()}, 1, storage)
                 .status,
             BlockMidiStatus::invalid_range, "invalid block range accepted");
  REQUIRE_EQ(plan.instruments()[0].copy_block({24'000}, 1, std::span(storage).first(1)).status,
             BlockMidiStatus::insufficient_storage, "undersized callback storage accepted");
}

void crop_and_elapsed_anchor() {
  auto musical =
      snapshot(tempo(), {clip(3, MusicalInterval{{0}, {quarter + quarter / 4}}, true, {note(4)})});
  const auto cropped = take(compile_midi_events(musical));
  const auto& events = cropped.instruments()[0].events();
  REQUIRE_EQ(events.size(), 4, "clip end did not emit synthetic cropped note-off at 30000 samples");
  REQUIRE_EQ(events.back().sample.value, 30'000,
             "clip end did not emit synthetic cropped note-off at 30000 samples");
  REQUIRE_EQ(events.back().kind, MidiEventKind::note_off,
             "clip end did not emit synthetic cropped note-off at 30000 samples");
  REQUIRE_TRUE(events.back().boundary_crop,
               "clip end did not emit synthetic cropped note-off at 30000 samples");

  auto elapsed =
      snapshot(tempo(), {clip(5, ElapsedInterval{{100'001}, {160'001}}, false, {note(6)})});
  const auto anchored = take(compile_midi_events(elapsed));
  REQUIRE_EQ(anchored.instruments()[0].events()[0].sample.value, 100'001,
             "elapsed anchor lost exact sample or quarter-note duration");
  REQUIRE_EQ(anchored.instruments()[0].events()[1].sample.value, 124'001,
             "elapsed anchor lost exact sample or quarter-note duration");
  auto changed =
      snapshot(tempo(true), {clip(5, ElapsedInterval{{100'001}, {160'001}}, false, {note(6)})});
  const auto ramp_anchored = take(compile_midi_events(changed));
  REQUIRE_EQ(ramp_anchored.instruments()[0].events()[0].sample.value, 100'001,
             "elapsed anchor shifted or ignored changed tempo after the ramp");
  REQUIRE_EQ(ramp_anchored.instruments()[0].events()[1].sample.value, 148'001,
             "elapsed anchor shifted or ignored changed tempo after the ramp");
}

void ramp_and_same_sample_order() {
  auto state = snapshot(tempo(true),
                        {clip(3, MusicalInterval{{0}, {2 * quarter}}, true, {note(9), note(4)})});
  const auto plan = take(compile_midi_events(state));
  const auto& events = plan.instruments()[0].events();
  // Integral of 60/(120-60x) over one quarter is ln(2) seconds.
  const auto ramp_samples = static_cast<std::int64_t>(std::llround(std::log(2.0) * 48'000.0));
  REQUIRE_EQ(events.size(), 8, "ramp integral or stable same-sample order failed");
  REQUIRE_EQ(events[0].note, id<NoteId>(4), "ramp integral or stable same-sample order failed");
  REQUIRE_EQ(events[1].note, id<NoteId>(9), "ramp integral or stable same-sample order failed");
  REQUIRE_EQ(events[2].sample.value, ramp_samples,
             "ramp integral or stable same-sample order failed");
  REQUIRE_EQ(events[2].kind, MidiEventKind::note_off,
             "ramp integral or stable same-sample order failed");
  REQUIRE_EQ(events[3].kind, MidiEventKind::note_off,
             "ramp integral or stable same-sample order failed");
  REQUIRE_EQ(events[4].kind, MidiEventKind::note_on,
             "ramp integral or stable same-sample order failed");
  REQUIRE_EQ(events[6].sample.value, ramp_samples + 48'000,
             "ramp integral or stable same-sample order failed");
}

void density_and_checked_boundaries() {
  std::vector<MidiNote> many;
  for (std::uint64_t number = 1; number <= 1'024; ++number) {
    many.push_back(note(number, 0, 327'680'000));
  }
  auto state = snapshot(tempo(), {clip(3, MusicalInterval{{0}, {quarter}}, false, many)});
  auto accepted = take(compile_midi_events(state));
  std::array<BlockMidiEvent, max_midi_events_per_block> storage{};
  auto first = accepted.instruments()[0].copy_block({0}, 1, storage);
  REQUIRE_EQ(first.status, BlockMidiStatus::ready, "exact 1024-event window was not supported");
  REQUIRE_EQ(first.count, 1'024, "exact 1024-event window was not supported");
  auto second = accepted.instruments()[0].copy_block({8'192}, 1, storage);
  REQUIRE_EQ(second.status, BlockMidiStatus::ready,
             "events exactly 8192 samples apart were rejected or merged");
  REQUIRE_EQ(second.count, 1'024, "events exactly 8192 samples apart were rejected or merged");
  REQUIRE_EQ(storage[0].kind, MidiEventKind::note_off,
             "events exactly 8192 samples apart were rejected or merged");
  state.tracks[0].midi_clips[0].notes.push_back(note(1'025));
  auto rejected = compile_midi_events(state);
  REQUIRE_TRUE(!rejected, "1025 coincident events passed density preflight");
  REQUIRE_EQ(rejected.error().code, ErrorCode::resource_exhausted,
             "1025 coincident events passed density preflight");

  auto extreme =
      snapshot(tempo(), {clip(4, MusicalInterval{{0}, {std::numeric_limits<std::int64_t>::max()}},
                              true, {note(2, 0, 1)})});
  extreme.tracks[0].midi_clips[0].pattern_length = {1};
  auto too_many = compile_midi_events(extreme);
  REQUIRE_TRUE(!too_many, "extreme loop count overflowed before rejection");
  REQUIRE_EQ(too_many.error().code, ErrorCode::resource_exhausted,
             "extreme loop count overflowed before rejection");

  // A huge pattern repeats three times, and its third start is past the tick
  // range: pedal lanes, silent or not, are refused cleanly, not overflowed.
  constexpr std::int64_t huge_pattern = (std::int64_t{1} << 62) + 1;
  auto huge = snapshot(
      tempo(),
      {clip(5, MusicalInterval{{0}, {std::numeric_limits<std::int64_t>::max()}}, true, {})});
  huge.tracks[0].midi_clips[0].pattern_length = {huge_pattern};
  for (const auto position : {std::int64_t{0}, huge_pattern}) {
    huge.tracks[0].midi_clips[0].pedals = {{MidiPedal::sustain, {{{position}, 1.0}}}};
    auto refused = compile_midi_events(huge);
    REQUIRE_TRUE(!refused, "a pedal lane on a huge pattern overflowed before rejection");
    REQUIRE_EQ(refused.error().code, ErrorCode::resource_exhausted,
               "a pedal lane on a huge pattern overflowed before rejection");
  }
}

void empty_and_malformed_input() {
  auto blank = snapshot(tempo(), {clip(3, MusicalInterval{{0}, {quarter}}, false, {})});
  auto empty = take(compile_midi_events(blank));
  REQUIRE_EQ(empty.instruments().size(), 1,
             "drawn empty MIDI clip did not compile to a silent instrument");
  REQUIRE_TRUE(empty.instruments()[0].events().empty(),
               "drawn empty MIDI clip did not compile to a silent instrument");
  auto invalid_rate = blank;
  invalid_rate.sample_rate = 1;
  require(!compile_midi_events(invalid_rate), "unsupported project rate accepted");
  auto wrong_kind = blank;
  wrong_kind.tracks[0].kind = TrackKind::audio;
  require(!compile_midi_events(wrong_kind), "audio track with MIDI clips accepted");
  auto ordinary_audio = blank;
  ordinary_audio.tracks[0].kind = TrackKind::audio;
  ordinary_audio.tracks[0].midi_clips.clear();
  require(take(compile_midi_events(ordinary_audio)).instruments().empty(),
          "audio-only project created instrument event state");

  auto bad = blank;
  bad.tracks[0].midi_clips[0].pattern_length = {0};
  require(!compile_midi_events(bad), "zero-length MIDI pattern accepted");
  bad = blank;
  bad.tracks[0].midi_clips[0].placement = MusicalInterval{{-1}, {quarter}};
  require(!compile_midi_events(bad), "negative musical placement accepted");
  bad = blank;
  bad.tracks[0].midi_clips[0].placement = MusicalInterval{{0}, {1}};
  require(!compile_midi_events(bad), "sub-sample musical clip accepted");
  bad = std::move(blank);
  bad.tracks[0].midi_clips[0].placement = ElapsedInterval{{5}, {5}};
  require(!compile_midi_events(bad), "empty elapsed placement accepted");

  bad = snapshot(tempo(), {clip(3, MusicalInterval{{0}, {quarter}}, false, {note(4)})});
  bad.tracks[0].midi_clips[0].notes[0].velocity = 0;
  require(!compile_midi_events(bad), "zero-velocity note accepted");
  bad.tracks[0].midi_clips[0].notes[0] = note(4, 0, 1);
  require(!compile_midi_events(bad), "note quantized to zero samples accepted");
  bad.tracks[0].midi_clips[0].notes[0] = note(4, quarter / 2, quarter);
  bad.tracks[0].midi_clips[0].placement = MusicalInterval{{0}, {quarter / 4}};
  auto outside = take(compile_midi_events(bad));
  require(outside.instruments()[0].events().empty(),
          "note starting outside cropped clip emitted events");

  auto elapsed_loop = snapshot(
      tempo(), {clip(5, ElapsedInterval{{100'001}, {160'001}}, true, {note(6, 0, quarter / 2)})});
  auto looped = take(compile_midi_events(elapsed_loop));
  REQUIRE_EQ(looped.instruments()[0].events().size(), 6,
             "elapsed loop did not repeat using exact sample anchor");
  REQUIRE_EQ(looped.instruments()[0].events()[4].sample.value, 148'001,
             "elapsed loop did not repeat using exact sample anchor");

  auto overflow =
      snapshot(tempo(), {clip(7,
                              MusicalInterval{{std::numeric_limits<std::int64_t>::max() - 100'000},
                                              {std::numeric_limits<std::int64_t>::max()}},
                              false, {note(8, 50'000, 200'000)})});
  require(!compile_midi_events(overflow), "large musical anchor overflowed or passed");
}
} // namespace

// Pedals (ADR-0035): their own stream, repeated per loop, held until the
// next point, released at the clip's end, chased by pedal_state_at, and cut
// with a release for an export end.
void pedal_stream_loops_releases_and_chases() {
  auto looped = clip(3, MusicalInterval{{0}, {2 * quarter}}, true, {note(4)});
  looped.pedals = {{MidiPedal::sustain, {{{quarter / 2}, 1.0}}}};
  const auto plan = take(compile_midi_events(snapshot(tempo(), {looped})));
  const auto& events = plan.instruments()[0];
  const auto& pedals = events.pedals();
  REQUIRE_EQ(pedals.size(), 3, "points repeat per loop and the clip end releases a held pedal");
  REQUIRE_EQ(pedals[0].sample.value, 12'000,
             "points repeat per loop and the clip end releases a held pedal");
  REQUIRE_EQ(pedals[1].sample.value, 36'000,
             "points repeat per loop and the clip end releases a held pedal");
  REQUIRE_EQ(pedals[2].sample.value, 48'000,
             "points repeat per loop and the clip end releases a held pedal");
  REQUIRE_EQ(pedals[0].value, 1.0, "points repeat per loop and the clip end releases a held pedal");
  REQUIRE_EQ(pedals[2].value, 0.0, "points repeat per loop and the clip end releases a held pedal");
  REQUIRE_TRUE(pedals[2].clip_end, "points repeat per loop and the clip end releases a held pedal");
  REQUIRE_EQ(events.events().size(), 4, "notes are unchanged beside the pedals");
  REQUIRE_EQ(events.pedal_state_at({12'000})[0], 0.0,
             "the chase state is the last value before the sample, per pedal");
  REQUIRE_EQ(events.pedal_state_at({12'001})[0], 1.0,
             "the chase state is the last value before the sample, per pedal");
  REQUIRE_EQ(events.pedal_state_at({48'001})[0], 0.0,
             "the chase state is the last value before the sample, per pedal");
  REQUIRE_EQ(events.pedal_state_at({30'000})[1], 0.0,
             "the chase state is the last value before the sample, per pedal");

  std::array<BlockPedalEvent, max_pedal_events_per_block> storage{};
  const auto block = events.copy_pedal_block({12'000}, 128, storage);
  REQUIRE_EQ(block.status, BlockMidiStatus::ready, "a pedal block carries offsets and values");
  REQUIRE_EQ(block.count, 1, "a pedal block carries offsets and values");
  REQUIRE_EQ(storage[0].offset, 0, "a pedal block carries offsets and values");
  REQUIRE_EQ(storage[0].pedal, MidiPedal::sustain, "a pedal block carries offsets and values");
  REQUIRE_EQ(storage[0].value, 1.0F, "a pedal block carries offsets and values");
  REQUIRE_EQ(events.copy_pedal_block({0}, 0, storage).status, BlockMidiStatus::invalid_range,
             "pedal blocks check their range");

  const auto cut = events.pedals_until(40'000);
  REQUIRE_EQ(cut.size(), 3, "an export cut releases a pedal still down");
  REQUIRE_EQ(cut[2].sample.value, 40'000, "an export cut releases a pedal still down");
  REQUIRE_EQ(cut[2].value, 0.0, "an export cut releases a pedal still down");
  REQUIRE_TRUE(cut[2].clip_end, "an export cut releases a pedal still down");
  REQUIRE_EQ(cut[2].clip, id<ClipId>(3), "an export cut releases a pedal still down");
  REQUIRE_EQ(events.pedals_until(20'000).size(), 2, "a cut inside a held pedal releases it");
  require(events.pedals_until(5'000).empty(), "a cut before any pedal adds nothing");
}

void pedal_clip_boundaries_and_density() {
  // Clip A ends with the pedal down where clip B puts it down again: the
  // release sorts first, so the pedal stays down.
  auto first = clip(3, MusicalInterval{{0}, {quarter}}, false, {note(4)});
  first.pedals = {{MidiPedal::sustain, {{{0}, 1.0}}}};
  auto second = clip(5, MusicalInterval{{quarter}, {2 * quarter}}, false, {note(6)});
  second.pedals = {{MidiPedal::sustain, {{{0}, 1.0}}}, {MidiPedal::soft, {{{0}, 0.6}}}};
  const auto plan = take(compile_midi_events(snapshot(tempo(), {first, second})));
  const auto& pedals = plan.instruments()[0].pedals();
  REQUIRE_EQ(pedals.size(), 6, "a clip-end release sorts before a new value at the same sample");
  REQUIRE_EQ(pedals[1].sample.value, 24'000,
             "a clip-end release sorts before a new value at the same sample");
  REQUIRE_TRUE(pedals[1].clip_end,
               "a clip-end release sorts before a new value at the same sample");
  REQUIRE_EQ(pedals[2].sample.value, 24'000,
             "a clip-end release sorts before a new value at the same sample");
  REQUIRE_EQ(pedals[2].value, 1.0,
             "a clip-end release sorts before a new value at the same sample");
  REQUIRE_EQ(plan.instruments()[0].pedal_state_at({24'001})[0], 1.0,
             "the next clip's pedals take over");
  REQUIRE_EQ(plan.instruments()[0].pedal_state_at({24'001})[2], 0.6,
             "the next clip's pedals take over");

  auto dense = clip(7, MusicalInterval{{0}, {quarter}}, false, {note(8)});
  MidiPedalLane lane{MidiPedal::sustain, {}};
  for (std::int64_t tick = 0; tick < 300; ++tick) {
    lane.points.push_back({{tick}, static_cast<double>(tick % 2)});
  }
  dense.pedals = {lane};
  auto refused = compile_midi_events(snapshot(tempo(), {dense}));
  REQUIRE_TRUE(!refused, "more than 256 pedal events in a block window are refused");
  REQUIRE_EQ(refused.error().code, ErrorCode::resource_exhausted,
             "more than 256 pedal events in a block window are refused");

  // Two ticks on one sample keep the lane's order: down, then up, stays up.
  auto same_sample = clip(9, MusicalInterval{{0}, {quarter}}, false, {note(10)});
  same_sample.pedals = {{MidiPedal::sustain, {{{0}, 1.0}, {{1}, 0.0}}}};
  const auto ordered = take(compile_midi_events(snapshot(tempo(), {same_sample})));
  REQUIRE_EQ(ordered.instruments()[0].pedal_state_at({1})[0], 0.0,
             "pedal points on one sample keep their lane order");

  // Out-of-range pedals and unsorted points are refused, not scheduled.
  auto unsorted = same_sample;
  unsorted.pedals = {{MidiPedal::sustain, {{{1}, 1.0}, {{0}, 0.0}}}};
  REQUIRE_TRUE(!compile_midi_events(snapshot(tempo(), {unsorted})),
               "unsorted pedal points are refused");
  auto unknown = same_sample;
  unknown.pedals = {{static_cast<MidiPedal>(midi_pedal_count), {{{0}, 1.0}}}};
  REQUIRE_TRUE(!compile_midi_events(snapshot(tempo(), {unknown})), "an unknown pedal is refused");
}

// Only the pattern plays (PI-06c): notes and pedal points kept after its
// end are silent, and a note crossing it is cut there on every pass.
void notes_and_pedals_past_the_pattern_are_silent() {
  // One-quarter pattern looping over two quarters at 120 BPM (24000 samples
  // a quarter): the crossing note sounds 12000-24000 and 36000-48000.
  auto looped = clip(3, MusicalInterval{{0}, {2 * quarter}}, true,
                     {note(4, quarter / 2, 2 * quarter), note(5, quarter, quarter + quarter / 2)});
  looped.pedals = {{MidiPedal::sustain, {{{0}, 1.0}, {{quarter}, 0.0}}}};
  const auto plan = take(compile_midi_events(snapshot(tempo(), {looped})));
  const auto& events = plan.instruments()[0].events();
  REQUIRE_EQ(events.size(), 4,
             "a looping pattern played a note after its end or did not cut one crossing it");
  REQUIRE_EQ(events[0].sample.value, 12'000,
             "a looping pattern played a note after its end or did not cut one crossing it");
  REQUIRE_EQ(events[1].sample.value, 24'000,
             "a looping pattern played a note after its end or did not cut one crossing it");
  REQUIRE_EQ(events[1].kind, MidiEventKind::note_off,
             "a looping pattern played a note after its end or did not cut one crossing it");
  REQUIRE_TRUE(events[1].boundary_crop,
               "a looping pattern played a note after its end or did not cut one crossing it");
  REQUIRE_EQ(events[2].sample.value, 36'000,
             "a looping pattern played a note after its end or did not cut one crossing it");
  REQUIRE_EQ(events[3].sample.value, 48'000,
             "a looping pattern played a note after its end or did not cut one crossing it");
  REQUIRE_TRUE(
      std::ranges::all_of(events, [](const auto& event) { return event.note == id<NoteId>(4); }),
      "a looping pattern played a note after its end or did not cut one crossing it");
  const auto& pedals = plan.instruments()[0].pedals();
  REQUIRE_EQ(pedals.size(), 3, "a pedal point after the pattern's end played");
  REQUIRE_EQ(pedals[0].sample.value, 0, "a pedal point after the pattern's end played");
  REQUIRE_EQ(pedals[1].sample.value, 24'000, "a pedal point after the pattern's end played");
  REQUIRE_EQ(pedals[1].value, 1.0, "a pedal point after the pattern's end played");
  REQUIRE_EQ(pedals[2].sample.value, 48'000, "a pedal point after the pattern's end played");
  REQUIRE_TRUE(pedals[2].clip_end, "a pedal point after the pattern's end played");

  // Not looping, a clip longer than its pattern is silent after it.
  auto single = clip(6, MusicalInterval{{0}, {2 * quarter}}, false,
                     {note(7, quarter / 2, 2 * quarter), note(8, quarter, 2 * quarter)});
  const auto once = take(compile_midi_events(snapshot(tempo(), {single})));
  const auto& cut = once.instruments()[0].events();
  REQUIRE_EQ(cut.size(), 2, "a non-looping clip played past its pattern");
  REQUIRE_EQ(cut[0].sample.value, 12'000, "a non-looping clip played past its pattern");
  REQUIRE_EQ(cut[1].sample.value, 24'000, "a non-looping clip played past its pattern");
  REQUIRE_TRUE(cut[1].boundary_crop, "a non-looping clip played past its pattern");

  // A note crossing the end by less than a sample is silent, not an error.
  auto sliver =
      clip(9, MusicalInterval{{0}, {quarter}}, false, {note(10, quarter - 1, quarter + 1)});
  const auto quiet = take(compile_midi_events(snapshot(tempo(), {sliver})));
  REQUIRE_LE(quiet.instruments()[0].events().size(), 2, "a sub-sample cut note failed to compile");
}

// A note still sounding when the playhead jumps back without a new
// generation (a planned loop wrap) is released at offset 0 of the jump, so
// every pass plays one voice per note instead of stacking another.
void held_notes_release_at_a_loop_wrap() {
  // One note from 0.75 to 1.5 seconds: its note-off is past a 1 s loop.
  const ScheduledMidiEvent on{{36'000}, id<ClipId>(3), id<NoteId>(4), MidiEventKind::note_on,
                              36,       100,           false};
  const ScheduledMidiEvent off{{72'000}, id<ClipId>(3), id<NoteId>(4), MidiEventKind::note_off, 36,
                               0,        false};
  const InstrumentEvents events(id<TrackId>(2), {on, off});
  HeldNotes held;
  std::vector<BlockMidiEvent> block(HeldNotes::block_capacity);
  std::size_t ons = 0;
  std::size_t offs = 0;
  for (int pass = 0; pass < 3; ++pass) {
    for (std::int64_t sample = 0; sample < 48'000; sample += 1'000) {
      const auto copied = held.copy_block(events, {sample}, 1'000, 1, block);
      REQUIRE_EQ(copied.status, BlockMidiStatus::ready, "the block copies");
      for (std::size_t i = 0; i < copied.count; ++i) {
        (block[i].kind == MidiEventKind::note_on ? ons : offs) += 1;
        if (block[i].kind == MidiEventKind::note_off)
          require(sample == 0 && block[i].offset == 0 && block[i].key == 36,
                  "the held note is released at the wrap");
      }
    }
  }
  REQUIRE_EQ(ons, 3, "each wrap releases the note the last pass started");
  REQUIRE_EQ(offs, 2, "each wrap releases the note the last pass started");
  // A failed jump keeps the held note, so the retried block still releases it.
  require(held.copy_block(events, {0}, 0, 1, block).status != BlockMidiStatus::ready,
          "an empty block is refused");
  const auto retried = held.copy_block(events, {0}, 1'000, 1, block);
  REQUIRE_EQ(retried.count, 1, "a retried jump releases the note the failed one kept");
  REQUIRE_EQ(block[0].kind, MidiEventKind::note_off,
             "a retried jump releases the note the failed one kept");
  // A new generation (a seek) resets the instrument, so nothing is sent.
  const auto seek = held.copy_block(events, {0}, 1'000, 2, block);
  REQUIRE_EQ(seek.status, BlockMidiStatus::ready,
             "a seek forgets held notes instead of releasing them");
  REQUIRE_EQ(seek.count, 0, "a seek forgets held notes instead of releasing them");
  // Continuous blocks send only the plan's own events.
  HeldNotes straight;
  std::size_t total = 0;
  for (std::int64_t sample = 0; sample < 96'000; sample += 1'000)
    total += straight.copy_block(events, {sample}, 1'000, 1, block).count;
  REQUIRE_EQ(total, 2, "continuous playback adds nothing");
}

// A plan replaced while playing (an edit) hands its sounding notes over at
// the block it takes effect: a note the new plan still holds keeps its voice
// and ends at its new note-off; any other is released at offset 0; a pedal
// the edit removed comes up there.
void a_replacing_plan_hands_held_notes_over() {
  constexpr std::int64_t block_samples = 1'000;
  constexpr std::int64_t switch_sample = 36'000;
  constexpr std::int64_t old_end = 72'000;
  constexpr std::int64_t new_end = 60'000;
  constexpr std::uint8_t kept_key = 36;
  constexpr std::uint8_t removed_key = 40;
  constexpr std::uint8_t velocity = 100;
  const auto event = [](std::int64_t sample, std::uint64_t note, MidiEventKind kind,
                        std::uint8_t key) {
    return ScheduledMidiEvent{
        {sample}, id<ClipId>(3), id<NoteId>(note),
        kind,     key,           kind == MidiEventKind::note_on ? velocity : std::uint8_t{0},
        false};
  };
  const ScheduledPedalEvent sustain{{0}, id<ClipId>(3), MidiPedal::sustain, 1.0};
  const InstrumentEvents before(id<TrackId>(2),
                                {event(0, 4, MidiEventKind::note_on, kept_key),
                                 event(0, 5, MidiEventKind::note_on, removed_key),
                                 event(old_end, 4, MidiEventKind::note_off, kept_key),
                                 event(old_end, 5, MidiEventKind::note_off, removed_key)},
                                {sustain});
  const InstrumentEvents after(id<TrackId>(2),
                               {event(0, 4, MidiEventKind::note_on, kept_key),
                                event(new_end, 4, MidiEventKind::note_off, kept_key)});
  const NoteSpans spans(after);
  HeldNotes held;
  std::vector<BlockMidiEvent> block(HeldNotes::block_capacity);
  for (std::int64_t sample = 0; sample < switch_sample; sample += block_samples)
    REQUIRE_EQ(held.copy_block(before, {sample}, block_samples, 1, block).status,
               BlockMidiStatus::ready, "the old plan plays");
  const auto switched = held.copy_block(after, {switch_sample}, block_samples, 1, block, &spans);
  REQUIRE_EQ(switched.count, 1, "only the removed note is released at the switch");
  require(block[0].kind == MidiEventKind::note_off && block[0].offset == 0 &&
              block[0].key == removed_key,
          "only the removed note is released at the switch");
  std::size_t kept_offs = 0;
  for (std::int64_t sample = switch_sample + block_samples; sample < old_end + block_samples;
       sample += block_samples) {
    const auto copied = held.copy_block(after, {sample}, block_samples, 1, block);
    for (std::size_t i = 0; i < copied.count; ++i) {
      require(block[i].kind == MidiEventKind::note_off && block[i].key == kept_key &&
                  sample + block[i].offset == new_end,
              "the kept note ends at its new note-off");
      ++kept_offs;
    }
  }
  REQUIRE_EQ(kept_offs, 1, "the kept note ends at its new note-off");
  // A note is held only under its own key, and only once it has started.
  BlockMidiEvent kept{
      0, id<ClipId>(3).bits(), id<NoteId>(4).bits(), MidiEventKind::note_on, kept_key, velocity};
  require(spans.holds(kept, switch_sample) && !spans.holds(kept, 0) &&
              !spans.holds(kept, new_end + 1),
          "a note is held between its note-on and note-off");
  kept.key = removed_key;
  require(!spans.holds(kept, switch_sample), "a note under another key is not held");
  // The sustain the edit removed is lifted at the switch.
  std::vector<BlockPedalEvent> pedals(max_pedal_events_per_block + midi_pedal_count);
  const auto lifted = after.copy_pedals({switch_sample}, block_samples, false, pedals, &before);
  REQUIRE_EQ(lifted.count, 1, "the removed sustain is lifted at the switch");
  require(pedals[0].offset == 0 && pedals[0].pedal == MidiPedal::sustain && pedals[0].value == 0.0F,
          "the removed sustain is lifted at the switch");
  REQUIRE_EQ(before.copy_pedals({switch_sample}, block_samples, false, pedals, &before).count, 0,
             "an unchanged pedal is not sent again");
}

// The compiler refuses a plan with more notes sounding at once than
// HeldNotes tracks, even when no 8192-sample window is too dense.
void concurrent_notes_are_bounded() {
  constexpr std::int64_t pattern = 2 * quarter;
  constexpr std::int64_t late_start = quarter / 2;
  std::vector<MidiNote> notes;
  for (std::uint64_t number = 1; number <= max_concurrent_midi_notes; ++number)
    notes.push_back(note(number, 0, quarter));
  auto state = snapshot(tempo(), {clip(3, MusicalInterval{{0}, {pattern}}, false, notes)});
  state.tracks[0].midi_clips[0].pattern_length = {pattern};
  require(static_cast<bool>(compile_midi_events(state)),
          "the most concurrent notes an instrument allows were refused");
  state.tracks[0].midi_clips[0].notes.push_back(
      note(max_concurrent_midi_notes + 1, late_start, pattern));
  const auto refused = compile_midi_events(state);
  REQUIRE_TRUE(!refused, "one note past the concurrent-note bound compiled");
  REQUIRE_EQ(refused.error().code, ErrorCode::resource_exhausted,
             "one note past the concurrent-note bound compiled");
}

// Every note the plan allows to sound at once is released at a loop wrap,
// well past the 256 an earlier table held.
void every_held_note_releases_at_a_wrap() {
  constexpr std::int64_t loop_end = 48'000;
  constexpr std::int64_t note_on = 36'000;
  constexpr std::int64_t note_off = 72'000;
  constexpr std::uint32_t block_frames = 1'000;
  constexpr std::uint8_t key = 36;
  constexpr std::uint8_t velocity = 100;
  const auto clip_id = id<ClipId>(3);
  std::vector<ScheduledMidiEvent> scheduled;
  for (std::uint64_t number = 1; number <= max_concurrent_midi_notes; ++number)
    scheduled.push_back(
        {{note_on}, clip_id, id<NoteId>(number), MidiEventKind::note_on, key, velocity, false});
  for (std::uint64_t number = 1; number <= max_concurrent_midi_notes; ++number)
    scheduled.push_back(
        {{note_off}, clip_id, id<NoteId>(number), MidiEventKind::note_off, key, 0, false});
  const InstrumentEvents events(id<TrackId>(2), std::move(scheduled));
  HeldNotes held;
  std::vector<BlockMidiEvent> block(HeldNotes::block_capacity);
  for (std::int64_t sample = 0; sample < loop_end; sample += block_frames)
    REQUIRE_EQ(held.copy_block(events, {sample}, block_frames, 1, block).status,
               BlockMidiStatus::ready, "the first pass copies");
  const auto wrapped = held.copy_block(events, {0}, block_frames, 1, block);
  REQUIRE_EQ(wrapped.status, BlockMidiStatus::ready, "the wrap copies");
  REQUIRE_EQ(wrapped.count, max_concurrent_midi_notes, "every held note is released at the wrap");
  std::vector<foundation::IdBits> released;
  for (const auto& event : std::span(block).first(wrapped.count)) {
    require(event.kind == MidiEventKind::note_off && event.offset == 0,
            "the wrap sends only releases at offset 0");
    released.push_back(event.note_id_bits);
  }
  std::ranges::sort(released);
  require(std::ranges::adjacent_find(released) == released.end(),
          "each held note is released once");
}

int main() {
  try {
    held_notes_release_at_a_loop_wrap();
    a_replacing_plan_hands_held_notes_over();
    concurrent_notes_are_bounded();
    every_held_note_releases_at_a_wrap();
    looping_order_and_half_open_blocks();
    crop_and_elapsed_anchor();
    ramp_and_same_sample_order();
    density_and_checked_boundaries();
    empty_and_malformed_input();
    pedal_stream_loops_releases_and_chases();
    pedal_clip_boundaries_and_density();
    notes_and_pedals_past_the_pattern_are_silent();
  } catch (const std::exception& error) {
    std::cerr << "MIDI events contract: " << error.what() << '\n';
    return 1;
  }
  std::cout << "MIDI events contract passed\n";
}
