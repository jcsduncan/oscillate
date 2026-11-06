#include "oscillate/project/model.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace oscillate;
using namespace oscillate::project;
using foundation::ErrorCode;
using foundation::Result;
using timeline::MeterMap;
using timeline::TempoMap;
constexpr auto quarter = timeline::ticks_per_quarter;

using oscillate::test::require;
using oscillate::test::take;
template <typename Id> Id id(std::uint64_t value) { return take(Id::from_bits({0, value})); }
TempoMap tempo(double bpm = 120.0, std::uint64_t revision = 1) {
  return take(TempoMap::create({{{0}, bpm, timeline::TempoShape::step}}, revision));
}
MeterMap meter() { return take(MeterMap::create({{{0}, 4, 4}}, 1)); }
struct Gate final : TransportGate {
  struct Lease final : StoppedTransportLease {};
  Result<std::unique_ptr<StoppedTransportLease>> acquire_stopped() override {
    return Result<std::unique_ptr<StoppedTransportLease>>::success(std::make_unique<Lease>());
  }
};
ProjectModel create(Gate& gate, std::uint32_t rate = oscillate::test::sample_rate_48k) {
  return take(ProjectModel::create(id<ProjectId>(1), rate, tempo(), meter(), gate));
}
MidiNote note(std::uint64_t number, std::int64_t start = 0, std::int64_t end = quarter / 2) {
  return {id<NoteId>(number), {start}, {end}, 60, 100};
}
MidiClip clip(std::uint64_t number, Placement placement, bool loop = false) {
  return {id<ClipId>(number), placement, {quarter}, loop, {}, 1};
}

void instrument_commands_and_reopen() {
  Gate gate;
  auto project = create(gate);
  const auto track = id<TrackId>(2);
  const NativeInstrumentPatch patch{"prism", 1, {{"prism.filter.cutoff", 900.0}}};
  auto original = project.snapshot();
  auto added = take(
      project.apply({AddTrack{track, TrackKind::instrument}, SetNativeInstrumentPatch{track, patch},
                     AddMidiClip{track, clip(3, MusicalInterval{{0}, {2 * quarter}}, true)},
                     AddMidiNote{id<ClipId>(3), note(4)}}));
  REQUIRE_TRUE(original->tracks.empty(),
               "compound mutated earlier revision or published more than once");
  REQUIRE_EQ(added.snapshot->revision, 2,
             "compound mutated earlier revision or published more than once");
  REQUIRE_EQ(added.snapshot->tracks[0].kind, TrackKind::instrument,
             "instrument state was not published");
  REQUIRE_EQ(std::get<NativeInstrumentPatch>(added.snapshot->tracks[0].instrument_source), patch,
             "instrument state was not published");
  REQUIRE_EQ(added.snapshot->tracks[0].midi_clips[0].notes[0], note(4),
             "instrument state was not published");
  REQUIRE_EQ(added.changes.tracks.size(), 1, "MIDI changes omitted stable IDs");
  REQUIRE_EQ(added.changes.clips.size(), 1, "MIDI changes omitted stable IDs");
  auto reopened = take(ProjectModel::open(*added.snapshot, gate));
  REQUIRE_EQ(reopened.snapshot()->tracks[0], added.snapshot->tracks[0],
             "model rehydration discarded instrument state");
  auto undone = take(project.undo());
  REQUIRE_TRUE(undone.snapshot->tracks.empty(), "MIDI compound undo was incomplete");
  REQUIRE_EQ(undone.changes.clips.size(), 1, "MIDI compound undo was incomplete");
  auto redone = take(project.redo());
  REQUIRE_EQ(redone.snapshot->tracks[0].midi_clips[0].notes[0].id, id<NoteId>(4),
             "MIDI redo changed stable note identity");
  auto removed = take(project.apply({RemoveClip{id<ClipId>(3)}}));
  REQUIRE_TRUE(removed.snapshot->tracks[0].midi_clips.empty(),
               "common clip removal did not remove MIDI clip");
  REQUIRE_EQ(removed.changes.clips.size(), 1, "common clip removal did not remove MIDI clip");
}

void validation_and_atomicity() {
  Gate gate;
  auto project = create(gate);
  const auto audio = id<TrackId>(2);
  const auto instrument = id<TrackId>(3);
  REQUIRE_OK(project.apply({AddTrack{audio}, AddTrack{instrument, TrackKind::instrument}}));
  require(!project.apply({SetNativeInstrumentPatch{
              audio, NativeInstrumentPatch{"prism", 1, {{"prism.filter.cutoff", 900.0}}}}}),
          "audio track accepted an instrument patch");
  require(!project.apply({AddMidiClip{audio, clip(4, MusicalInterval{{0}, {quarter}})}}),
          "audio track accepted MIDI clip");
  auto baseline = project.snapshot();
  auto failed = project.apply({AddMidiClip{instrument, clip(4, MusicalInterval{{0}, {quarter}})},
                               AddMidiNote{id<ClipId>(4), note(7, 0, 0)}});
  REQUIRE_TRUE(!failed, "invalid MIDI compound was not atomic");
  REQUIRE_EQ(project.snapshot(), baseline, "invalid MIDI compound was not atomic");
  REQUIRE_EQ(project.undo_depth(), 1, "invalid MIDI compound was not atomic");
  REQUIRE_OK(project.apply({AddMidiClip{instrument, clip(4, MusicalInterval{{0}, {quarter}})},
                            AddMidiNote{id<ClipId>(4), note(7)}}));
  require(!project.apply({AddMidiNote{id<ClipId>(4), note(7)}}), "duplicate NoteId accepted");
  require(
      !project.apply({AddMidiClip{instrument, clip(4, MusicalInterval{{quarter}, {2 * quarter}})}}),
      "duplicate ClipId accepted");
  require(!project.apply(
              {AddMidiClip{instrument, clip(5, MusicalInterval{{quarter / 2}, {2 * quarter}})}}),
          "overlapping MIDI clips accepted");
  require(
      !project.apply({AddMidiClip{instrument, clip(5, MusicalInterval{{quarter}, {2 * quarter}})},
                      AddMidiNote{id<ClipId>(5), note(7)}}),
      "duplicate global NoteId accepted");
  // A shortened pattern keeps notes and pedal points past its end, silent,
  // for when it is lengthened again.
  auto trimmed = take(project.apply(
      {SetMidiPattern{id<ClipId>(4), {quarter / 4}, false},
       AddMidiNote{id<ClipId>(4), note(9, quarter / 2, quarter)},
       SetMidiPedalLane{id<ClipId>(4), MidiPedal::sustain, {{{0}, 1.0}, {{quarter / 2}, 0.0}}}}));
  REQUIRE_EQ(trimmed.snapshot->tracks[1].midi_clips[0].notes.size(), 2,
             "notes or pedal points past the pattern's end were rejected");
  REQUIRE_EQ(trimmed.snapshot->tracks[1].midi_clips[0].pedals.front().points.size(), 2,
             "notes or pedal points past the pattern's end were rejected");
  REQUIRE_TRUE(!project.apply({AddMidiNote{id<ClipId>(4), note(10, -1, quarter)}}),
               "a note or pedal point before the clip's start was accepted");
  REQUIRE_TRUE(!project.apply({SetMidiPedalLane{id<ClipId>(4), MidiPedal::soft, {{{-1}, 1.0}}}}),
               "a note or pedal point before the clip's start was accepted");
  REQUIRE_OK(project.undo());
  auto enormous =
      clip(6, MusicalInterval{{quarter}, {std::numeric_limits<std::int64_t>::max()}}, true);
  enormous.pattern_length = {1};
  enormous.notes.push_back(note(8, 0, 1));
  auto too_many = project.apply({AddMidiClip{instrument, enormous}});
  REQUIRE_TRUE(!too_many, "extreme loop duration overflowed or published");
  REQUIRE_EQ(too_many.error().code, ErrorCode::resource_exhausted,
             "extreme loop duration overflowed or published");
  REQUIRE_EQ(project.snapshot()->tracks[1].midi_clips.size(), 1,
             "extreme loop duration overflowed or published");
  auto invalid_patch = *project.snapshot();
  invalid_patch.tracks[1].instrument_source =
      NativeInstrumentPatch{"prism", 1, {{"prism.filter.cutoff", NAN}}};
  require(!ProjectModel::open(invalid_patch, gate), "nonfinite patch value accepted on open");
  invalid_patch = *project.snapshot();
  invalid_patch.tracks[1].midi_clips[0].notes[0].velocity = 0;
  require(!ProjectModel::open(invalid_patch, gate), "invalid decoded MIDI note accepted");
}

void timing_rate_and_undo() {
  Gate gate;
  auto project = create(gate, 96'000);
  const auto track = id<TrackId>(2);
  auto item = clip(3, ElapsedInterval{{96'000}, {144'000}});
  item.notes.push_back(note(4, 0, 16'000));
  REQUIRE_OK(project.apply({AddTrack{track, TrackKind::instrument}, AddMidiClip{track, item}}));
  auto before = project.snapshot();
  auto reduced = project.apply({SetSampleRate{44'100}});
  REQUIRE_TRUE(!reduced, "rate edit that quantizes a note to zero published");
  REQUIRE_EQ(project.snapshot(), before, "rate edit that quantizes a note to zero published");
  auto tempo_changed = project.apply({ReplaceTempoMap{tempo(960.0, 2)}});
  REQUIRE_TRUE(!tempo_changed, "tempo edit that quantizes a note to zero published");
  REQUIRE_EQ(project.snapshot(), before, "tempo edit that quantizes a note to zero published");
  auto longer = note(4, 0, quarter / 2);
  REQUIRE_OK(project.apply({SetMidiNote{id<ClipId>(3), longer}}));
  auto changed = take(project.apply({SetSampleRate{48'000}}));
  REQUIRE_EQ(changed.snapshot->sample_rate, 48'000,
             "elapsed MIDI anchor did not preserve elapsed time");
  REQUIRE_EQ(
      std::get<ElapsedInterval>(changed.snapshot->tracks[0].midi_clips[0].placement).start.value,
      48'000, "elapsed MIDI anchor did not preserve elapsed time");
  REQUIRE_TRUE(changed.changes.sample_rate, "rate change did not announce MIDI clip");
  REQUIRE_EQ(changed.changes.clips.size(), 1, "rate change did not announce MIDI clip");
  auto undone = take(project.undo());
  REQUIRE_EQ(
      std::get<ElapsedInterval>(undone.snapshot->tracks[0].midi_clips[0].placement).start.value,
      96'000, "rate undo did not restore exact MIDI anchor");
  REQUIRE_EQ(undone.snapshot->sample_rate, 96'000, "rate undo did not restore exact MIDI anchor");
}

void midi_edit_sequence_and_undo() {
  Gate gate;
  auto project = create(gate);
  const auto track = id<TrackId>(2);
  const auto clip_id = id<ClipId>(3);
  auto item = clip(3, MusicalInterval{{0}, {quarter}});
  item.notes.push_back(note(4));
  REQUIRE_OK(project.apply({AddTrack{track, TrackKind::instrument}, AddMidiClip{track, item}}));

  auto same = take(project.apply({SetMidiClipPlacement{clip_id, item.placement}}));
  REQUIRE_EQ(same.snapshot->tracks[0].midi_clips[0].settings_generation, 1,
             "unchanged placement regenerated MIDI processing settings");
  auto moved =
      take(project.apply({SetMidiClipPlacement{clip_id, MusicalInterval{{quarter}, {2 * quarter}}},
                          SetMidiPattern{clip_id, {2 * quarter}, false},
                          SetMidiNote{clip_id, note(4, quarter / 4, quarter)}}));
  const auto& edited = moved.snapshot->tracks[0].midi_clips[0];
  REQUIRE_EQ(edited.settings_generation, 4,
             "compound MIDI placement, pattern and note edit was incomplete");
  REQUIRE_EQ(edited.notes[0].end.value, quarter,
             "compound MIDI placement, pattern and note edit was incomplete");
  REQUIRE_EQ(std::get<MusicalInterval>(edited.placement).start.value, quarter,
             "compound MIDI placement, pattern and note edit was incomplete");
  auto undone = take(project.undo());
  REQUIRE_EQ(undone.snapshot->tracks[0].midi_clips[0], item,
             "compound MIDI edit did not undo atomically");
  auto redone = take(project.redo());
  REQUIRE_EQ(redone.snapshot->tracks[0].midi_clips[0], edited,
             "compound MIDI edit did not redo atomically");
  require(!project.apply({RemoveMidiNote{clip_id, id<NoteId>(99)}}),
          "removing a missing MIDI note succeeded");
  auto removed = take(project.apply({RemoveMidiNote{clip_id, id<NoteId>(4)}}));
  require(removed.snapshot->tracks[0].midi_clips[0].notes.empty(),
          "MIDI note removal did not preserve the empty clip");
  require(!project.apply({SetMidiNote{clip_id, note(4)}}), "editing a removed MIDI note succeeded");
}
} // namespace

// Pedal lanes (ADR-0035): set, replace and remove through one command, kept
// in pedal order, validated, counted as events, and undone with the clip.
void pedal_lanes_set_validate_and_undo() {
  Gate gate;
  auto project = create(gate);
  const auto track = id<TrackId>(2);
  const auto clip_id = id<ClipId>(3);
  (void)take(project.apply({AddTrack{track, TrackKind::instrument},
                            AddMidiClip{track, clip(3, MusicalInterval{{0}, {2 * quarter}}, true)},
                            AddMidiNote{clip_id, note(4)}}));
  const auto before = project.snapshot();
  const std::vector<MidiPedalPoint> down_up{{{0}, 1.0}, {{quarter / 2}, 0.0}};
  auto soft = take(project.apply({SetMidiPedalLane{clip_id, MidiPedal::soft, {{{0}, 1.0}}}}));
  auto both = take(project.apply({SetMidiPedalLane{clip_id, MidiPedal::sustain, down_up}}));
  const auto& pedals = both.snapshot->tracks[0].midi_clips[0].pedals;
  REQUIRE_EQ(pedals.size(), 2, "lanes are added in pedal order");
  REQUIRE_EQ(pedals[0].pedal, MidiPedal::sustain, "lanes are added in pedal order");
  REQUIRE_EQ(pedals[0].points, down_up, "lanes are added in pedal order");
  REQUIRE_EQ(pedals[1].pedal, MidiPedal::soft, "lanes are added in pedal order");
  REQUIRE_GT(both.snapshot->tracks[0].midi_clips[0].settings_generation,
             soft.snapshot->tracks[0].midi_clips[0].settings_generation,
             "a pedal edit bumps the clip generation");
  REQUIRE_EQ(both.changes.clips.size(), 1, "a pedal edit reports its clip");
  REQUIRE_EQ(both.changes.clips[0], clip_id, "a pedal edit reports its clip");
  const auto generation = both.snapshot->tracks[0].midi_clips[0].settings_generation;
  auto same = take(project.apply({SetMidiPedalLane{clip_id, MidiPedal::sustain, down_up}}));
  REQUIRE_EQ(same.snapshot->tracks[0].midi_clips[0].settings_generation, generation,
             "an unchanged lane keeps the generation");

  for (const auto& bad : std::vector<std::vector<MidiPedalPoint>>{
           {{{0}, 1.0}, {{0}, 0.0}},
           {{{-1}, 1.0}},
           {{{0}, 1.5}},
           {{{0}, std::numeric_limits<double>::quiet_NaN()}}}) {
    auto rejected = project.apply({SetMidiPedalLane{clip_id, MidiPedal::sostenuto, bad}});
    REQUIRE_TRUE(!rejected, "invalid pedal points are rejected");
    REQUIRE_EQ(rejected.error().code, ErrorCode::invalid_argument,
               "invalid pedal points are rejected");
  }
  REQUIRE_EQ(project.snapshot()->tracks[0].midi_clips[0].pedals, pedals,
             "a rejected edit leaves the lanes untouched");
  // A point past the pattern end is kept, silent, for when the clip grows.
  auto past_end =
      project.apply({SetMidiPedalLane{clip_id, MidiPedal::sostenuto, {{{quarter}, 1.0}}}});
  REQUIRE_OK(past_end, "a pedal point past the pattern end was refused");
  (void)take(project.undo());

  auto removed = take(project.apply({SetMidiPedalLane{clip_id, MidiPedal::soft, {}}}));
  REQUIRE_EQ(removed.snapshot->tracks[0].midi_clips[0].pedals.size(), 1,
             "an empty point list removes the lane");
  auto missing = project.apply({SetMidiPedalLane{id<ClipId>(99), MidiPedal::soft, {{{0}, 1.0}}}});
  REQUIRE_TRUE(!missing, "an unknown clip is reported");
  REQUIRE_EQ(missing.error().code, ErrorCode::not_found, "an unknown clip is reported");
  (void)take(project.undo());
  (void)take(project.undo());
  (void)take(project.undo());
  (void)take(project.undo());
  REQUIRE_EQ(project.snapshot()->tracks[0].midi_clips[0], before->tracks[0].midi_clips[0],
             "undo restores the clip without pedals");
}

int main() {
  try {
    instrument_commands_and_reopen();
    validation_and_atomicity();
    timing_rate_and_undo();
    midi_edit_sequence_and_undo();
    pedal_lanes_set_validate_and_undo();
  } catch (const std::exception& error) {
    std::cerr << "MIDI project contract: " << error.what() << '\n';
    return 1;
  }
  std::cout << "MIDI project contract passed\n";
}
