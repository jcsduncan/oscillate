#include "oscillate/engine/midi_events.hpp"
#include "oscillate/native_dsp/note_effects/note_effect.hpp"
#include "oscillate/native_dsp/synth/player.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"
#include "oscillate/timeline/timing.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

// Note effects (ADR-0059): hand-worked oracles for each engine at 120 BPM
// and 48 kHz, where a sixteenth is exactly 6,000 frames; every result is
// also checked under other block splits. Then the synth player running a
// chain, as live playback and export both do.
namespace {

namespace fx = oscillate::native_dsp::note_effects;
namespace synth = oscillate::native_dsp::synth;
namespace engine = oscillate::engine;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
using oscillate::foundation::IdBits;

using oscillate::test::require;

using oscillate::test::take;

constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
constexpr std::uint64_t sixteenth = 6'000;
constexpr IdBits played_clip{7, 7};

const timeline::TempoMap& tempo() {
  static const auto map =
      take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  return map;
}

struct Note {
  std::uint64_t frame = 0;
  bool on = false;
  int key = 0;
  int velocity = 0;
  IdBits clip;
  IdBits note;
  bool operator==(const Note&) const = default;
};

struct Played {
  std::uint64_t start;
  std::uint64_t end;
  int key;
  int velocity = 100;
};

std::vector<Note> notes(std::initializer_list<Played> played) {
  std::vector<Note> events;
  std::uint64_t id = 0;
  for (const auto& note : played) {
    ++id;
    events.push_back({note.start, true, note.key, note.velocity, played_clip, {0, id}});
    events.push_back({note.end, false, note.key, 0, played_clip, {0, id}});
  }
  return events;
}

std::vector<double> values(std::string_view id,
                           std::initializer_list<std::pair<std::string_view, double>> set = {}) {
  const auto* found = fx::find_engine(id);
  REQUIRE_NE(found, nullptr, "engine registered");
  auto dense = oscillate::native_dsp::default_values(*found->descriptor);
  for (const auto& [parameter, value] : set) {
    const auto index = oscillate::native_dsp::find_parameter(*found->descriptor, parameter);
    REQUIRE_OK(index, "parameter exists");
    dense[*index] = value;
  }
  return dense;
}

engine::BlockMidiEvent block_event(const Note& note, std::uint64_t first) {
  return {static_cast<std::uint32_t>(note.frame - first),
          note.clip,
          note.note,
          note.on ? engine::MidiEventKind::note_on : engine::MidiEventKind::note_off,
          static_cast<std::uint8_t>(note.key),
          static_cast<std::uint8_t>(note.velocity),
          false};
}

bool event_less(const engine::BlockMidiEvent& a, const engine::BlockMidiEvent& b) {
  return std::tie(a.offset, a.kind, a.note_id_bits) < std::tie(b.offset, b.kind, b.note_id_bits);
}

// Runs one kernel over `input` for `total` frames in blocks of `block`
// (0: irregular blocks), returning every event it writes.
std::vector<Note> run(std::string_view id, const std::vector<double>& dense,
                      const std::vector<Note>& input, std::uint64_t total, std::uint32_t block) {
  auto kernel = fx::find_engine(id)->create_kernel({0xF00D, 1});
  std::vector<engine::BlockMidiEvent> in;
  std::vector<engine::BlockMidiEvent> out(fx::block_event_capacity);
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
    const fx::NoteClock clock(tempo(), rate, frame, frames);
    require(clock.valid(), "the clock places the block");
    const fx::NoteParameters parameters(dense, {});
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
std::vector<Note> run_split(std::string_view id, const std::vector<double>& dense,
                            const std::vector<Note>& input, std::uint64_t total) {
  const auto reference = run(id, dense, input, total, 128);
  for (const std::uint32_t block : {64U, 100U, 1'000U, 8'192U, 0U})
    REQUIRE_EQ(run(id, dense, input, total, block), reference,
               std::string(id) + " renders the same notes under any block split");
  return reference;
}

// Note-ons (or offs) as (frame, key), by frame then key.
std::vector<std::pair<std::uint64_t, int>> starts(const std::vector<Note>& events, bool on) {
  std::vector<std::pair<std::uint64_t, int>> list;
  for (const auto& event : events) {
    if (event.on == on)
      list.emplace_back(event.frame, event.key);
  }
  std::ranges::sort(list);
  return list;
}
std::vector<std::pair<std::uint64_t, int>> ons(const std::vector<Note>& events) {
  return starts(events, true);
}
std::vector<std::pair<std::uint64_t, int>> offs(const std::vector<Note>& events) {
  return starts(events, false);
}

using Steps = std::vector<std::pair<std::uint64_t, int>>;

void the_registry_is_complete() {
  const auto all = fx::engines();
  REQUIRE_EQ(all.size(), 6, "six note effects");
  std::set<std::string_view> ids;
  for (const auto* engine : all) {
    ids.insert(engine->descriptor->id);
    REQUIRE_OK(oscillate::native_dsp::validate_engine(*engine->descriptor),
               std::string(engine->descriptor->id) + " has a valid table");
    for (const auto& preset : engine->descriptor->presets)
      REQUIRE_OK(oscillate::native_dsp::preset_values(*engine->descriptor, preset),
                 std::string(preset.name) + " resolves");
    require(!engine->descriptor->presets.empty(), "every note effect has presets");
  }
  REQUIRE_EQ(ids, (std::set<std::string_view>{"arp", "chord", "comp", "echo", "random", "scale"}),
             "the ids are stable");
  require(!fx::resolve_note_effect("arpeggio", 1, {}), "an unknown engine is refused");
  require(!fx::resolve_note_effect("arp", 2, {}), "a newer version is refused");
  const std::vector<project::NativeInstrumentParameter> wild{{"arp.octaves", 9.0}};
  require(!fx::resolve_note_effect("arp", 1, wild), "an out-of-range value is refused");
  const auto& arp = *fx::find_engine("arp");
  const auto sparse = fx::sparse_note_effect_parameters(arp, values("arp", {{"arp.octaves", 3}}));
  REQUIRE_EQ(sparse.size(), 1, "only values off their default are saved");
  REQUIRE_EQ(sparse.front().id, "arp.octaves", "only values off their default are saved");
  REQUIRE_EQ(sparse.front().value, 3.0, "only values off their default are saved");
  const auto stepped = take(fx::note_effect_automation_domain(arp, "arp.octaves"));
  const auto smooth = take(fx::note_effect_automation_domain(arp, "arp.gate"));
  REQUIRE_TRUE(stepped.discrete, "domains follow the table");
  REQUIRE_EQ(stepped.step_count, 3, "domains follow the table");
  REQUIRE_TRUE(!smooth.discrete, "domains follow the table");
  REQUIRE_EQ(smooth.minimum, 0.05, "domains follow the table");
  REQUIRE_EQ(smooth.maximum, 2.0, "domains follow the table");
}

void the_arp_plays_up_on_the_sixteenth_grid() {
  const auto chord = notes({{0, 48'000, 60}, {0, 48'000, 64}, {0, 48'000, 67}});
  const auto played = run_split("arp", values("arp"), chord, 60'000);
  const Steps expected{{0, 60},      {6'000, 64},  {12'000, 67}, {18'000, 60},
                       {24'000, 64}, {30'000, 67}, {36'000, 60}, {42'000, 64}};
  REQUIRE_EQ(ons(played), expected, "Up plays low to high, one per sixteenth");
  Steps released;
  for (const auto& [frame, key] : expected)
    released.emplace_back(frame + sixteenth / 2, key);
  REQUIRE_EQ(offs(played), released, "a half gate ends each note half a step later");
  require(std::ranges::all_of(played, [](const Note& n) { return n.clip == IdBits{0xF00D, 1}; }),
          "the held notes are the arp's input, not heard themselves");
}

void the_arp_modes_order_the_notes() {
  const auto chord = notes({{0, 36'000, 60}, {0, 36'000, 64}, {0, 36'000, 67}});
  const auto keys = [&](std::initializer_list<std::pair<std::string_view, double>> set) {
    std::vector<int> list;
    for (const auto& [frame, key] : ons(run_split("arp", values("arp", set), chord, 40'000)))
      list.push_back(key);
    return list;
  };
  REQUIRE_EQ(keys({{"arp.mode", 1}}), (std::vector<int>{67, 64, 60, 67, 64, 60}), "Down");
  REQUIRE_EQ(keys({{"arp.mode", 2}, {"arp.octaves", 2}}),
             (std::vector<int>{60, 64, 67, 72, 76, 79}), "Up-Down climbs every octave first");
  const auto long_chord = notes({{0, 72'000, 60}, {0, 72'000, 64}, {0, 72'000, 67}});
  std::vector<int> up_down;
  for (const auto& [frame, key] : ons(run_split(
           "arp", values("arp", {{"arp.mode", 2}, {"arp.octaves", 2}}), long_chord, 72'000)))
    up_down.push_back(key);
  REQUIRE_EQ(up_down, (std::vector<int>{60, 64, 67, 72, 76, 79, 76, 72, 67, 64, 60, 64}),
             "and comes down without repeating the top or bottom");
  REQUIRE_EQ(keys({{"arp.mode", 3}}), (std::vector<int>{67, 64, 60, 64, 67, 64}), "Down-Up");
  REQUIRE_EQ(keys({{"arp.steps", 2}}), (std::vector<int>{60, 64, 60, 64, 60, 64}),
             "Steps restarts the pattern");
  const auto stabs = ons(run_split("arp", values("arp", {{"arp.mode", 6}}), chord, 13'000));
  REQUIRE_EQ(stabs,
             (Steps{{0, 60},
                    {0, 64},
                    {0, 67},
                    {6'000, 60},
                    {6'000, 64},
                    {6'000, 67},
                    {12'000, 60},
                    {12'000, 64},
                    {12'000, 67}}),
             "Chord plays every held note on each step");
  // As Played keeps arrival order.
  const auto arrivals = notes({{0, 30'000, 67}, {100, 30'000, 60}, {200, 30'000, 64}});
  std::vector<int> as_played;
  for (const auto& [frame, key] :
       ons(run_split("arp", values("arp", {{"arp.mode", 4}}), arrivals, 24'500)))
    as_played.push_back(key);
  REQUIRE_EQ(as_played, (std::vector<int>{67, 60, 64, 67, 60}), "As Played");
  // Random is seeded: the same choices every time, others for another seed.
  const auto random = run_split("arp", values("arp", {{"arp.mode", 5}}), chord, 36'000);
  const auto reseeded =
      run_split("arp", values("arp", {{"arp.mode", 5}, {"arp.seed", 2}}), chord, 36'000);
  REQUIRE_EQ(random, run_split("arp", values("arp", {{"arp.mode", 5}}), chord, 36'000),
             "Random is reproducible and follows its seed");
  REQUIRE_NE(ons(random), ons(reseeded), "Random is reproducible and follows its seed");
}

void the_arp_swings_starts_at_once_and_latches() {
  const auto held = notes({{0, 30'000, 60}});
  std::vector<std::uint64_t> swung;
  for (const auto& [frame, key] :
       ons(run_split("arp", values("arp", {{"arp.swing", 0.75}}), held, 30'000)))
    swung.push_back(frame);
  REQUIRE_EQ(swung, (std::vector<std::uint64_t>{0, 9'000, 12'000, 21'000, 24'000}),
             "every second step of a pair is late by half a step at 75%");
  std::vector<std::uint64_t> late;
  for (const auto& [frame, key] :
       ons(run_split("arp", values("arp"), notes({{1'000, 20'000, 60}}), 20'000)))
    late.push_back(frame);
  REQUIRE_EQ(late, (std::vector<std::uint64_t>{1'000, 6'000, 12'000, 18'000}),
             "a first note plays at once, then the grid carries on");
  const auto latched = ons(run_split("arp", values("arp", {{"arp.latch", 1}}),
                                     notes({{0, 3'000, 60}, {30'000, 33'000, 64}}), 43'000));
  REQUIRE_EQ(latched,
             (Steps{{0, 60},
                    {6'000, 60},
                    {12'000, 60},
                    {18'000, 60},
                    {24'000, 60},
                    {30'000, 64},
                    {36'000, 64},
                    {42'000, 64}}),
             "Latch plays on after release, and a new chord replaces the old");
  const auto accented = run_split("arp", values("arp", {{"arp.accent", 2}}), held, 24'000);
  std::vector<int> velocities;
  for (const auto& event : accented) {
    if (event.on)
      velocities.push_back(event.velocity);
  }
  REQUIRE_EQ(velocities, (std::vector<int>{124, 100, 124, 100}), "Accent Every lifts every second");
  const auto fixed = run_split("arp", values("arp", {{"arp.velocity", 1}, {"arp.level", 80}}),
                               notes({{0, 12'000, 60, 30}}), 12'000);
  require(std::ranges::all_of(fixed, [](const Note& n) { return !n.on || n.velocity == 80; }),
          "Fixed velocity ignores the played velocity");
}

void the_chord_adds_intervals_or_degrees() {
  const auto played = run_split("chord", values("chord"), notes({{0, 12'000, 60}}), 13'000);
  REQUIRE_EQ(ons(played), (Steps{{0, 60}, {0, 64}, {0, 67}}),
             "a major triad sounds and ends with the played note");
  REQUIRE_EQ(offs(played), (Steps{{12'000, 60}, {12'000, 64}, {12'000, 67}}),
             "a major triad sounds and ends with the played note");
  REQUIRE_EQ(std::ranges::count_if(
                 played, [](const Note& n) { return n.clip == played_clip && n.key == 60; }),
             2, "the played note keeps its ids");
  const auto in_key = values("chord", {{"chord.mode", 1}});
  REQUIRE_EQ(ons(run_split("chord", in_key, notes({{0, 6'000, 62}}), 7'000)),
             (Steps{{0, 62}, {0, 65}, {0, 69}}), "D in C major is D minor");
  REQUIRE_EQ(ons(run_split("chord", values("chord", {{"chord.mode", 1}, {"chord.shape", 1}}),
                           notes({{0, 6'000, 67}}), 7'000)),
             (Steps{{0, 67}, {0, 71}, {0, 74}, {0, 77}}), "G in C major's seventh is dominant");
  const auto inverted =
      ons(run_split("chord", values("chord", {{"chord.mode", 1}, {"chord.inversion", 1}}),
                    notes({{0, 6'000, 62}}), 7'000));
  REQUIRE_EQ(inverted, (Steps{{0, 65}, {0, 69}, {0, 74}}), "the first inversion lifts the root");
  const auto soft = run_split("chord", values("chord", {{"chord.velocity", 0.5}}),
                              notes({{0, 6'000, 60, 100}}), 7'000);
  require(std::ranges::all_of(
              soft, [](const Note& n) { return !n.on || n.velocity == (n.key == 60 ? 100 : 50); }),
          "added tones take the added velocity");
}

void the_echo_repeats_on_the_grid() {
  const auto echoed = run_split("echo", values("echo"), notes({{0, 12'000, 60}}), 50'000);
  REQUIRE_EQ(ons(echoed), (Steps{{0, 60}, {12'000, 60}, {24'000, 60}, {36'000, 60}}),
             "three repeats an eighth apart, each as long as the note");
  REQUIRE_EQ(offs(echoed), (Steps{{12'000, 60}, {24'000, 60}, {36'000, 60}, {48'000, 60}}),
             "three repeats an eighth apart, each as long as the note");
  std::vector<int> velocities;
  for (const auto& event : echoed) {
    if (event.on)
      velocities.push_back(event.velocity);
  }
  REQUIRE_EQ(velocities, (std::vector<int>{100, 70, 49, 34}), "each repeat 70% of the last");
  const auto rising =
      run_split("echo", values("echo", {{"echo.pitch", 7}}), notes({{0, 3'000, 60}}), 40'000);
  REQUIRE_EQ(ons(rising), (Steps{{0, 60}, {12'000, 67}, {24'000, 74}, {36'000, 81}}),
             "Pitch moves each repeat");
  REQUIRE_LT(std::abs(fx::find_engine("echo")->tail_beats(values("echo")) - 1.5),
             tolerance::double_rounding, "the tail covers the repeats");
}

void random_is_seeded_and_in_key() {
  std::initializer_list<Played> line{
      {0, 3'000, 60},       {6'000, 9'000, 62},   {12'000, 15'000, 64}, {18'000, 21'000, 65},
      {24'000, 27'000, 67}, {30'000, 33'000, 69}, {36'000, 39'000, 71}, {42'000, 45'000, 72}};
  const auto input = notes(line);
  const auto major = values("random", {{"random.chance", 1.0}, {"random.scale", 0}});
  const auto moved = run_split("random", major, input, 50'000);
  require(std::ranges::all_of(ons(moved),
                              [](const auto& on) {
                                const int pitch = on.second % 12;
                                return pitch == 0 || pitch == 2 || pitch == 4 || pitch == 5 ||
                                       pitch == 7 || pitch == 9 || pitch == 11;
                              }),
          "every moved note is in C major");
  REQUIRE_NE(
      ons(moved),
      ons(run_split(
          "random",
          values("random", {{"random.chance", 1.0}, {"random.scale", 0}, {"random.seed", 5}}),
          input, 50'000)),
      "another seed makes other choices");
  // Velocity and timing: inside their bounds, and a late note keeps its length.
  const auto feel =
      values("random", {{"random.chance", 0.0}, {"random.velocity", 1.0}, {"random.timing", 1.0}});
  const auto loose = run_split("random", feel, input, 60'000);
  std::size_t index = 0;
  for (const auto& note : line) {
    const auto on = std::ranges::find_if(
        loose, [&](const Note& n) { return n.on && n.note == IdBits{0, index + 1}; });
    const auto off = std::ranges::find_if(
        loose, [&](const Note& n) { return !n.on && n.note == IdBits{0, index + 1}; });
    REQUIRE_NE(on, loose.end(), "every note sounds and ends");
    REQUIRE_NE(off, loose.end(), "every note sounds and ends");
    REQUIRE_GE(on->frame, note.start, "a note moves at most a sixteenth late");
    REQUIRE_LE(on->frame, note.start + sixteenth, "a note moves at most a sixteenth late");
    REQUIRE_EQ(off->frame - on->frame, note.end - note.start, "a late note keeps its length");
    REQUIRE_EQ(on->key, note.key, "velocity moves at most half the range");
    REQUIRE_GE(on->velocity, 36, "velocity moves at most half the range");
    REQUIRE_LE(on->velocity, 127, "velocity moves at most half the range");
    ++index;
  }
}

void scale_snaps_and_moves() {
  const auto input = notes({{0, 6'000, 61}});
  const auto snapped = run_split("scale", values("scale"), input, 7'000);
  REQUIRE_EQ(ons(snapped), (Steps{{0, 62}}),
             "C# goes to D in C major (ties go up), and ends there");
  REQUIRE_EQ(offs(snapped), (Steps{{6'000, 62}}),
             "C# goes to D in C major (ties go up), and ends there");
  REQUIRE_EQ(ons(run_split("scale", values("scale", {{"scale.snap", 2}}), input, 7'000)),
             (Steps{{0, 60}}), "Down snaps below");
  REQUIRE_EQ(ons(run_split("scale", values("scale", {{"scale.transpose", 2}}),
                           notes({{0, 6'000, 60}}), 7'000)),
             (Steps{{0, 64}}), "two scale steps up from C is E");
  REQUIRE_EQ(
      ons(run_split("scale",
                    values("scale", {{"scale.root", 9}, {"scale.scale", 1}, {"scale.octave", -1}}),
                    notes({{0, 6'000, 68}}), 7'000)),
      (Steps{{0, 57}}), "G# in A minor goes to A, an octave down");
}

void the_chain_releases_what_it_made() {
  const std::vector<project::NoteEffectInsert> inserts{
      {take(project::InsertSlotId::from_bits({0, 1})), true, "arp", 1, {}},
      {take(project::InsertSlotId::from_bits({0, 2})),
       true,
       "scale",
       1,
       {{"scale.transpose", 1.0}}}};
  auto chain = take(fx::NoteEffectChain::create(inserts));
  REQUIRE_EQ(chain->size(), 2, "the chain keeps the slots");
  REQUIRE_TRUE(chain->same_layout(inserts), "the chain keeps the slots");
  const auto arp_values = values("arp", {{"arp.gate", 2.0}});
  const auto scale_values = values("scale", {{"scale.transpose", 1}});
  std::vector<engine::BlockMidiEvent> events(fx::block_event_capacity);
  std::vector<engine::BlockMidiEvent> scratch(fx::block_event_capacity);
  std::vector<fx::StageControls> controls{{true, arp_values, {}}, {true, scale_values, {}}};
  events[0] = block_event({0, true, 60, 100, played_clip, {0, 1}}, 0);
  const fx::NoteClock first(tempo(), rate, 0, 128);
  auto out = chain->process(events, 1, scratch, first, controls);
  REQUIRE_EQ(out.size(), 1, "the arp's note passes the scale: C moves up a step to D");
  REQUIRE_EQ(out[0].key, 62, "the arp's note passes the scale: C moves up a step to D");
  REQUIRE_EQ(out[0].kind, engine::MidiEventKind::note_on,
             "the arp's note passes the scale: C moves up a step to D");
  // Switched off mid-note, the arp releases the note it was sounding.
  controls[0].enabled = false;
  const fx::NoteClock second(tempo(), rate, 128, 128);
  out = chain->process(events, 0, scratch, second, controls);
  REQUIRE_EQ(out.size(), 1, "switching the arp off ends its note, through the scale");
  REQUIRE_EQ(out[0].kind, engine::MidiEventKind::note_off,
             "switching the arp off ends its note, through the scale");
  REQUIRE_EQ(out[0].key, 62, "switching the arp off ends its note, through the scale");
  std::vector<project::NoteEffectInsert> moved{inserts[1], inserts[0]};
  require(!chain->same_layout(moved), "a reordered chain is another layout");
}

// --- The synth player -------------------------------------------------

template <typename Id> Id make_id(std::uint64_t value) { return take(Id::from_bits({0, value})); }

// A Prism track (fast release) holding one note for two beats, through an
// arp at a quarter gate when `arp` is set.
project::Snapshot arp_project(bool arp) {
  constexpr auto quarter = timeline::ticks_per_quarter;
  auto map = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  project::Snapshot state{
      make_id<project::ProjectId>(1), 1, rate, std::move(map), std::move(meter), {}, {}};
  project::Track track{
      make_id<project::TrackId>(2),
      {},
      project::TrackKind::instrument,
      project::NativeInstrumentPatch{
          "prism", 1, {{"prism.amp.attack", 0.0005}, {"prism.amp.release", 0.005}}}};
  project::MidiClip clip{make_id<project::ClipId>(3),
                         project::MusicalInterval{{0}, {4 * quarter}},
                         {4 * quarter},
                         false,
                         {{make_id<project::NoteId>(4), {0}, {2 * quarter}, 57, 100}}};
  track.midi_clips.push_back(std::move(clip));
  if (arp)
    track.note_effects.push_back(
        {make_id<project::InsertSlotId>(9), true, "arp", 1, {{"arp.gate", 0.25}}});
  state.tracks.push_back(std::move(track));
  state.next_insert_slot_serial = 10;
  return state;
}

std::vector<double> render(const project::Snapshot& state, std::uint32_t block,
                           std::uint64_t total) {
  const auto plan = take(engine::compile_midi_events(state));
  auto player =
      take(synth::NativeSynthPlayer::prepare(state, state.tracks[0], plan.instruments()[0], {},
                                             8'192, std::numeric_limits<std::uint64_t>::max()));
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

double peak(const std::vector<double>& x, std::size_t begin, std::size_t end) {
  double level = 0.0;
  for (std::size_t i = begin; i < end && i < x.size(); ++i)
    level = std::max(level, std::abs(x[i]));
  return level;
}

void the_player_arpeggiates_a_clip() {
  const auto plain = render(arp_project(false), 128, 30'000);
  const auto arped = render(arp_project(true), 128, 30'000);
  // A quarter gate of a sixteenth ends each step at 1,500 frames; with a
  // 5 ms release the rest of the step is near silence.
  for (std::uint64_t step = 0; step < 4; ++step) {
    const auto start = step * sixteenth;
    REQUIRE_GT(peak(arped, start, start + 1'000), 0.01, "each step sounds on its sixteenth");
    REQUIRE_GT(peak(arped, start, start + 64), 0.1 * peak(arped, start, start + 1'000), "at once");
    REQUIRE_LT(peak(arped, start + 3'500, start + 5'900), 0.02 * peak(arped, start, start + 1'000),
               "and is quiet after its gate");
    REQUIRE_GT(peak(plain, start + 2'000, start + 5'900), 0.01, "the held note alone sustains");
    REQUIRE_LT(peak(arped, start - (step == 0 ? 0 : 1), start),
               0.02 * peak(arped, start, start + 1'000), "a step starts exactly on its frame");
  }
  REQUIRE_EQ(render(arp_project(true), 100, 30'000), arped,
             "the player renders identically under any block split, as export does");
  REQUIRE_EQ(render(arp_project(true), 8'192, 30'000), arped,
             "the player renders identically under any block split, as export does");
}

void the_player_arpeggiates_live_notes() {
  auto state = arp_project(true);
  state.tracks[0].midi_clips.clear();
  const engine::InstrumentEvents empty{state.tracks[0].id, {}};
  auto player = take(synth::NativeSynthPlayer::prepare(state, state.tracks[0], empty, {}, 128,
                                                       std::numeric_limits<std::uint64_t>::max()));
  require(player.queue_live_note(57, 100, true), "a live note is queued");
  std::vector<engine::StereoFrame> buffer(128);
  std::vector<double> out;
  for (std::uint64_t frame = 0; frame < 3 * sixteenth; frame += 128) {
    buffer.assign(128, {});
    require(player.render(buffer, frame, 1), "the live block renders");
    for (const auto& sample : buffer)
      out.push_back(sample.left);
  }
  REQUIRE_GT(peak(out, 6'000, 7'000), 0.01, "a held live note is arpeggiated on the grid");
  REQUIRE_LT(peak(out, 9'500, 11'900), 0.02 * peak(out, 6'000, 7'000),
             "a held live note is arpeggiated on the grid");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "Note effect contract passed",
      {{"the_registry_is_complete", the_registry_is_complete},
       {"the_arp_plays_up_on_the_sixteenth_grid", the_arp_plays_up_on_the_sixteenth_grid},
       {"the_arp_modes_order_the_notes", the_arp_modes_order_the_notes},
       {"the_arp_swings_starts_at_once_and_latches", the_arp_swings_starts_at_once_and_latches},
       {"the_chord_adds_intervals_or_degrees", the_chord_adds_intervals_or_degrees},
       {"the_echo_repeats_on_the_grid", the_echo_repeats_on_the_grid},
       {"random_is_seeded_and_in_key", random_is_seeded_and_in_key},
       {"scale_snaps_and_moves", scale_snaps_and_moves},
       {"the_chain_releases_what_it_made", the_chain_releases_what_it_made},
       {"the_player_arpeggiates_a_clip", the_player_arpeggiates_a_clip},
       {"the_player_arpeggiates_live_notes", the_player_arpeggiates_live_notes}});
}
