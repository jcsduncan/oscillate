#include "oscillate/harmony/clip.hpp"
#include "oscillate/harmony/harmony.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// Chords as notes (ADR-0083): reading a clip's chords and rewriting its notes.
// Expected chords come from the notes as written (music theory), and rewrite
// oracles compare the rhythm and the untouched notes with the input.
namespace {

using namespace oscillate;
using namespace oscillate::harmony;
using enum ChordQuality;
using project::KeyMode;
using project::MidiNote;

constexpr std::int64_t step = recognition_step_ticks;
constexpr std::int64_t beat = timeline::ticks_per_quarter;
constexpr std::int64_t bar = 4 * beat;
constexpr std::uint8_t A = 9, B = 11, C = 0, D = 2, E = 4, F = 5, G = 7, b_flat = 10;
constexpr std::uint8_t velocity = 90;
// The cleanliness rules every voicing keeps (as in harmony_tests.cpp).
constexpr int semitone = 1;
constexpr int minor_ninth_span = 13;
constexpr int fifth_floor = 48;
constexpr int third_floor = 53;
constexpr int perfect_fifth = 7;
constexpr int minor_third = 3;
constexpr int drone_headroom = 3;
const MusicalKey a_minor{A, KeyMode::minor};
const MusicalKey c_minor{C, KeyMode::minor};

using oscillate::test::require;

ChordSymbol chord(std::uint8_t root, ChordQuality q,
                  std::optional<std::uint8_t> bass = std::nullopt) {
  return {root, q, bass};
}

std::vector<ChordRegion> lane_of(std::initializer_list<ChordSymbol> chords,
                                 std::int64_t length = bar) {
  std::vector<ChordRegion> out;
  std::int64_t at = 0;
  for (const auto& c : chords) {
    out.push_back({{at}, length, c, std::nullopt, ChordDevice::none});
    at += length;
  }
  return out;
}

std::int64_t span_end(std::span<const ChordRegion> lane) {
  return lane.back().position.value + lane.back().length_ticks;
}

std::string names(std::span<const ChordRegion> lane) {
  std::string out;
  for (const auto& r : lane) {
    out += (out.empty() ? "" : " · ") + name(r.chord, a_minor) +
           (r.top ? "^" + std::string(pitch_name(*r.top, a_minor)) : "") + "@" +
           std::to_string(r.position.value / step) + "+" + std::to_string(r.length_ticks / step);
  }
  return out;
}

MidiNote note(std::int64_t from_step, std::int64_t steps, std::uint8_t key, std::uint64_t id) {
  return {project::NoteId::from_bits({0, id}).value(),
          {from_step * step},
          {(from_step + steps) * step},
          key,
          velocity};
}

std::vector<MidiNote> sorted(std::vector<MidiNote> notes) {
  std::stable_sort(notes.begin(), notes.end(), [](const MidiNote& x, const MidiNote& y) {
    return x.start != y.start ? x.start < y.start : x.key < y.key;
  });
  return notes;
}

// What a region sounds like: its pitch classes (drone included) and bass.
std::pair<std::uint16_t, std::uint8_t> sound(const ChordSymbol& c,
                                             std::optional<std::uint8_t> drone) {
  auto set = pitch_class_set(c);
  if (drone)
    set = static_cast<std::uint16_t>(set | (1U << *drone));
  return {set, c.bass.value_or(c.root)};
}

bool sounds_alike(const ChordRegion& x, const ChordRegion& y) {
  return sound(x.chord, x.top) == sound(y.chord, y.top);
}

// The notes starting in [from, to).
std::vector<MidiNote> starting_in(std::span<const MidiNote> notes, std::int64_t from,
                                  std::int64_t to) {
  std::vector<MidiNote> out;
  for (const auto& n : notes)
    if (n.start.value >= from && n.start.value < to)
      out.push_back(n);
  return out;
}

std::set<std::pair<std::int64_t, std::int64_t>> rhythm(std::span<const MidiNote> notes) {
  std::set<std::pair<std::int64_t, std::int64_t>> out;
  for (const auto& n : notes)
    out.insert({n.start.value, n.end.value});
  return out;
}

// Each onset's upper voices keep voice()'s rules; the notes are well formed.
void check_notes(std::span<const MidiNote> notes, std::int64_t length, bool flat_ninth,
                 const std::string& what) {
  std::map<std::int64_t, std::vector<int>> groups;
  std::set<std::pair<std::int64_t, int>> seen;
  for (std::size_t i = 0; i < notes.size(); ++i) {
    const auto& n = notes[i];
    require(n.start.value >= 0 && n.end.value <= length && n.end.value > n.start.value,
            what + ": a note inside the clip");
    require(seen.insert({n.start.value, n.key}).second, what + ": one note per start and key");
    if (i > 0)
      require(notes[i - 1].start < n.start ||
                  (notes[i - 1].start == n.start && notes[i - 1].key < n.key),
              what + ": sorted by start then key");
    if (n.key >= bass_role_below)
      groups[n.start.value].push_back(n.key);
  }
  for (const auto& [at, keys] : groups) {
    const auto where = what + " at step " + std::to_string(at / step);
    for (std::size_t i = 1; i < keys.size(); ++i) {
      const int low = keys[i - 1], gap = keys[i] - low;
      REQUIRE_NE(gap, semitone, where + ": no neighbouring semitone");
      require(low >= fifth_floor || gap >= perfect_fifth, where + ": a fifth apart below C3");
      require(low >= third_floor || gap >= minor_third, where + ": a third apart below F3");
    }
    if (!flat_ninth)
      for (std::size_t i = 0; i < keys.size(); ++i)
        for (std::size_t j = i + 1; j < keys.size(); ++j)
          require(!((keys[j] - keys[i]) % 12 == semitone && keys[j] - keys[i] >= minor_ninth_span),
                  where + ": no minor ninth");
  }
}

// A comping part from held chords: the bass on beats 1 and 3, the chord on
// the off-beats, the top voice alone on beat 3's 16th.
std::vector<MidiNote> comp_part(std::span<const MidiNote> held) {
  std::vector<MidiNote> out;
  std::uint64_t id = 1;
  constexpr std::int64_t half_bar = 8;
  constexpr std::array<std::int64_t, 4> stabs{2, 6, 10, 14};
  constexpr std::int64_t stab_length = 2;
  constexpr std::int64_t top_alone = 9;
  for (const auto& n : held) {
    const auto first = n.start.value / step;
    const auto steps = (n.end.value - n.start.value) / step;
    for (std::int64_t b = 0; b < steps; b += 2 * half_bar) {
      if (n.key < bass_role_below) {
        out.push_back(note(first + b, half_bar, n.key, id++));
        out.push_back(note(first + b + half_bar, half_bar, n.key, id++));
        continue;
      }
      for (const auto s : stabs)
        out.push_back(note(first + b + s, stab_length, n.key, id++));
      std::int64_t top = 0;
      for (const auto& m : held)
        if (m.start == n.start)
          top = std::max<std::int64_t>(top, m.key);
      if (n.key == top)
        out.push_back(note(first + b + top_alone, 1, n.key, id++));
    }
  }
  return sorted(out);
}

std::vector<MidiNote> with_ids(std::vector<MidiNote> notes) {
  std::uint64_t id = 1;
  for (auto& n : notes)
    n.id = project::NoteId::from_bits({0, id++}).value();
  return notes;
}

// ---- identify ----
void identify_reads_every_voiced_chord() {
  for (std::uint8_t q = 1; q <= last_chord_quality; ++q) {
    for (std::uint8_t root = 0; root < pitch_classes; ++root) {
      for (const auto style : {VoicingStyle::close, VoicingStyle::drop2, VoicingStyle::spread}) {
        VoicingOptions o;
        o.style = style;
        o.lowest = bass_role_below; // The upper voices above the bass, as clips voice them.
        const auto c = chord(root, static_cast<ChordQuality>(q));
        const auto v = voice(c, std::nullopt, o, nullptr);
        std::vector<std::uint8_t> keys(v.notes.begin(), v.notes.begin() + v.count);
        keys.push_back(bass_note(c, comping_bass_lowest));
        const auto got = identify(keys);
        require(got && *got == c, name(c, a_minor) + " style " +
                                      std::to_string(static_cast<int>(style)) + " read as " +
                                      (got ? name(*got, a_minor) : "nothing"));
      }
    }
  }
  // Inversions: the lowest note is the slash bass.
  for (const auto& c :
       {chord(C, major, E), chord(C, major, G), chord(A, minor, C), chord(A, minor, E),
        chord(G, dominant_seventh, B), chord(G, dominant_seventh, D), chord(F, major_seventh, A),
        chord(F, major_seventh, C)}) {
    const auto v = voice(c, std::nullopt, VoicingOptions{}, nullptr);
    std::vector<std::uint8_t> keys(v.notes.begin(), v.notes.begin() + v.count);
    keys.push_back(bass_note(c, comping_bass_lowest));
    const auto got = identify(keys);
    require(got && *got == c,
            name(c, a_minor) + " read as " + (got ? name(*got, a_minor) : "nothing"));
  }
  // A missing fifth still reads; a lone pitch class is no chord.
  REQUIRE_EQ(identify(std::vector<std::uint8_t>{48, 52, 58}), chord(C, dominant_seventh),
             "C E B♭ is C7");
  REQUIRE_EQ(identify(std::vector<std::uint8_t>{45, 60, 67}), chord(A, minor_seventh),
             "A C G is Am7");
  REQUIRE_EQ(identify(std::vector<std::uint8_t>{45, 57, 69}), std::nullopt,
             "octaves of A are no chord");
  REQUIRE_EQ(identify(std::vector<std::uint8_t>{}), std::nullopt, "silence is no chord");
  // A first-inversion triad is the triad, not a sixth chord without its fifth.
  REQUIRE_EQ(identify(std::vector<std::uint8_t>{48, 57, 64}), chord(A, minor, C), "C A E is Am/C");
}

// ---- recognise ----
void recognise_reads_written_lab_lanes() {
  int compared = 0, exact = 0, regions = 0;
  for (const double brightness : {0.0, 0.3, 0.6, 0.9}) {
    for (std::size_t s = 0; s < shape_count; ++s) {
      for (const auto colour : {Colour::natural, Colour::triads, Colour::add9, Colour::sus2,
                                Colour::sevenths, Colour::lush}) {
        for (const DeviceSet devices :
             {DeviceSet{0}, device_bit(Device::drone), device_bit(Device::pedal),
              static_cast<DeviceSet>(device_bit(Device::walk) | device_bit(Device::passing)),
              static_cast<DeviceSet>(device_bit(Device::suspension) |
                                     device_bit(Device::pull_home) | device_bit(Device::borrowed)),
              device_bit(Device::slide)}) {
          for (std::uint64_t seed = 0; seed < 2; ++seed) {
            LabOptions o;
            o.mood.brightness = brightness;
            o.shape = static_cast<Shape>(s);
            o.colour = colour;
            o.devices = devices;
            o.seed = seed;
            for (const auto& suggestion : suggest(o)) {
              const auto& lane = suggestion.chords;
              const auto length = span_end(lane);
              for (const auto& voicing : {VoicingOptions{}, open_voicing()}) {
                const auto notes = with_ids(write(lane, voicing));
                check_notes(notes, length, false, "written " + names(lane));
                // Expected: the regions as written, neighbours that sound the
                // same merged. A reading must name every written pitch class
                // (adding at most its fifth) over the written bass.
                struct Written {
                  ChordRegion region;
                  std::uint16_t pcs = 0;
                  std::uint8_t bass = 0;
                };
                std::vector<Written> expected;
                for (const auto& r : lane) {
                  Written w{r, 0, 0};
                  int lowest = 128;
                  for (const auto& n :
                       starting_in(notes, r.position.value, r.position.value + r.length_ticks)) {
                    w.pcs = static_cast<std::uint16_t>(w.pcs | (1U << (n.key % pitch_classes)));
                    lowest = std::min<int>(lowest, n.key);
                  }
                  w.bass = static_cast<std::uint8_t>(lowest % pitch_classes);
                  if (!expected.empty() && expected.back().pcs == w.pcs &&
                      expected.back().bass == w.bass)
                    expected.back().region.length_ticks += r.length_ticks;
                  else
                    expected.push_back(w);
                }
                const auto got = recognise(notes, {length});
                const auto what = names(lane) + " read as " + names(got);
                REQUIRE_EQ(got.size(), expected.size(), what);
                for (std::size_t i = 0; i < got.size(); ++i) {
                  const auto& e = expected[i];
                  require(got[i].position == e.region.position &&
                              got[i].length_ticks == e.region.length_ticks,
                          what + ": positions");
                  const auto [set, bass] = sound(got[i].chord, got[i].top);
                  const auto fifth = static_cast<std::uint16_t>(
                      1U << ((got[i].chord.root + perfect_fifth) % pitch_classes));
                  require((set & e.pcs) == e.pcs && (set & ~e.pcs & ~fifth) == 0 && bass == e.bass,
                          what + ": region " + std::to_string(i) + " sounds as written");
                  REQUIRE_EQ(got[i].device, ChordDevice::none, what + ": no device");
                  // Without devices nothing is ambiguous: the very chord.
                  if (devices == 0)
                    require(got[i].chord == e.region.chord && !got[i].top,
                            what + ": exact at " + std::to_string(i));
                  exact += got[i].chord == e.region.chord ? 1 : 0;
                  ++regions;
                }
                ++compared;
              }
            }
          }
        }
      }
    }
  }
  REQUIRE_GT(compared, 0, "lanes compared");
  std::cout << exact << " of " << regions << " regions read as the very chord\n";
}

void recognise_ignores_passing_notes_and_inherits() {
  constexpr std::int64_t steps_per_bar = bar / step;
  std::vector<MidiNote> notes{
      // Bar 1: Am, with a passing B for one 16th.
      note(0, 16, 45, 1),
      note(0, 16, 57, 2),
      note(0, 16, 60, 3),
      note(0, 16, 64, 4),
      note(5, 1, 71, 5),
      // Bar 2: Dm for two beats, then the bass alone.
      note(16, 16, 50, 6),
      note(16, 8, 57, 7),
      note(16, 8, 62, 8),
      note(16, 8, 65, 9),
      // Bar 3: F for two beats, then a rest.
      note(32, 8, 41, 10),
      note(32, 8, 57, 11),
      note(32, 8, 60, 12),
      note(32, 8, 65, 13),
      // Bar 4: G.
      note(48, 16, 43, 14),
      note(48, 16, 59, 15),
      note(48, 16, 62, 16),
      note(48, 16, 67, 17),
  };
  const auto got = recognise(notes, {4 * bar});
  const auto want = lane_of({chord(A, minor), chord(D, minor), chord(F, major), chord(G, major)});
  REQUIRE_EQ(got, want, "passing note and rests: " + names(got));
  // Leading silence takes the first chord.
  const auto late = recognise(
      std::vector<MidiNote>{note(8, 8, 57, 1), note(8, 8, 60, 2), note(8, 8, 64, 3)}, {bar});
  REQUIRE_EQ(late, lane_of({chord(A, minor)}), "leading silence: " + names(late));
  // Slash chords from a moving bass under a held Am, then a held C.
  std::vector<MidiNote> walk{
      note(0, 16, 57, 1), note(0, 16, 60, 2),  note(0, 16, 64, 3),  note(0, 8, 45, 4),
      note(8, 8, 43, 5),  note(16, 16, 55, 6), note(16, 16, 60, 7), note(16, 16, 64, 8),
      note(16, 8, 48, 9), note(24, 8, 40, 10),
  };
  const auto slash = recognise(walk, {2 * bar});
  const auto half = bar / 2;
  const std::vector<ChordRegion> expected{
      {{0}, half, chord(A, minor), std::nullopt, ChordDevice::none},
      {{half}, half, chord(A, minor_seventh, G), std::nullopt, ChordDevice::none},
      {{bar}, half, chord(C, major), std::nullopt, ChordDevice::none},
      {{bar + half}, half, chord(C, major, E), std::nullopt, ChordDevice::none},
  };
  REQUIRE_EQ(slash, expected, "moving bass: " + names(slash));
  require(recognise(std::vector<MidiNote>{note(0, 16, 45, 1)}, {bar}).empty(),
          "a bass alone is no chord");
  require(recognise({}, {bar}).empty(), "an empty clip has no chords");
  REQUIRE_EQ(steps_per_bar, 16, "a bar is sixteen steps");
}

// ---- rewrite ----
void rewrite_changes_only_the_edited_region() {
  const auto lane = lane_of({chord(A, minor_ninth), chord(D, minor_ninth), chord(F, major_ninth),
                             chord(E, dominant_seventh)});
  const auto length = span_end(lane);
  const auto part = comp_part(write(lane, VoicingOptions{}));
  const auto read = recognise(part, {length});
  REQUIRE_EQ(read, lane, "the comping part reads as its lane: " + names(read));
  // The same lane changes nothing, ids included.
  REQUIRE_EQ(rewrite(part, read, {length}, VoicingOptions{}), part,
             "the same lane keeps every note");
  // Shuffled input comes back sorted and otherwise unchanged.
  auto shuffled = part;
  std::reverse(shuffled.begin(), shuffled.end());
  REQUIRE_EQ(rewrite(shuffled, read, {length}, VoicingOptions{}), part, "unsorted input is sorted");

  for (const auto& replacement : {chord(b_flat, major_seventh), chord(G, dominant_ninth),
                                  chord(D, minor, F), chord(C, sixth)}) {
    auto edited = lane;
    edited[1].chord = replacement;
    const auto out = rewrite(part, edited, {length}, VoicingOptions{});
    const auto what = "Dm9 -> " + name(replacement, a_minor);
    check_notes(out, length, false, what);
    for (const std::size_t other : {0U, 2U, 3U}) {
      const auto from = lane[other].position.value, to = from + lane[other].length_ticks;
      REQUIRE_EQ(starting_in(out, from, to), starting_in(part, from, to),
                 what + ": region " + std::to_string(other) + " untouched");
    }
    const auto from = lane[1].position.value, to = from + lane[1].length_ticks;
    REQUIRE_EQ(rhythm(starting_in(out, from, to)), rhythm(starting_in(part, from, to)),
               what + ": the rhythm is kept");
    const auto again = recognise(out, {length});
    REQUIRE_EQ(again.size(), edited.size(), what + ": read as " + names(again));
    for (std::size_t i = 0; i < again.size(); ++i)
      REQUIRE_EQ(again[i], edited[i], what + ": read as " + names(again));
  }
}

void rewrite_keeps_notes_outside_the_lane() {
  const auto lane = lane_of({chord(A, minor_ninth), chord(D, minor_ninth), chord(F, major_ninth),
                             chord(E, dominant_seventh)});
  const auto length = span_end(lane);
  const auto part = comp_part(write(lane, VoicingOptions{}));
  // Only the first two bars are in the lane; the first changes.
  auto partial = std::vector<ChordRegion>(lane.begin(), lane.begin() + 2);
  partial[0].chord = chord(F, major_seventh);
  const auto out = rewrite(part, partial, {length}, VoicingOptions{});
  REQUIRE_EQ(starting_in(out, 2 * bar, length), starting_in(part, 2 * bar, length),
             "notes outside every region are untouched");
  REQUIRE_EQ(starting_in(out, bar, 2 * bar), starting_in(part, bar, 2 * bar),
             "the kept region is untouched");
  const auto again = recognise(out, {length});
  require(!again.empty() && again.front().chord == chord(F, major_seventh) &&
              again.front().length_ticks == bar,
          "the first bar reads as Fmaj7: " + names(again));
}

void rewrite_split_cuts_held_notes() {
  const auto lane = lane_of({chord(A, minor_ninth), chord(D, minor_ninth)});
  const auto length = span_end(lane);
  const auto held = with_ids(write(lane, VoicingOptions{}));
  const auto half = bar / 2;
  std::vector<ChordRegion> split{
      {{0}, half, chord(A, minor_ninth), std::nullopt, ChordDevice::none},
      {{half}, half, chord(G, dominant_seventh, B), std::nullopt, ChordDevice::none},
      lane[1],
  };
  const auto out = rewrite(held, split, {length}, VoicingOptions{});
  check_notes(out, length, false, "split");
  // The first half keeps its notes, cut at the boundary, ids and keys kept.
  const auto first = starting_in(out, 0, half);
  const auto before = starting_in(held, 0, bar);
  REQUIRE_EQ(first.size(), before.size(), "the first half keeps its notes");
  for (std::size_t i = 0; i < first.size(); ++i)
    require(first[i].id == before[i].id && first[i].key == before[i].key &&
                first[i].end.value == half,
            "held notes are cut at the split");
  for (const auto& n : starting_in(out, half, bar))
    require(n.start.value == half && n.end.value == bar && n.id == unassigned_note_id(),
            "the second half is the new chord, held");
  REQUIRE_EQ(starting_in(out, bar, length), starting_in(held, bar, length),
             "the next bar is untouched");
  const auto again = recognise(out, {length});
  REQUIRE_EQ(again, split, "the split reads back: " + names(again));
}

void rewrite_revoices_a_region_in_its_rhythm() {
  const auto lane = lane_of({chord(A, minor_ninth), chord(D, minor_ninth), chord(F, major_ninth),
                             chord(E, dominant_seventh)});
  const auto length = span_end(lane);
  const auto part = comp_part(write(lane, VoicingOptions{}));
  int changed = 0;
  constexpr int octave_up = 12;
  for (const auto& [style, offset] :
       {std::pair{VoicingStyle::close, 0}, std::pair{VoicingStyle::spread, 0},
        std::pair{VoicingStyle::rootless, 0}, std::pair{VoicingStyle::drop2, octave_up}}) {
    VoicingOptions o;
    o.style = style;
    o.register_offset = offset;
    const auto out = rewrite(part, lane, {length}, o, 1);
    const auto what = "revoice style " + std::to_string(static_cast<int>(style)) + " register " +
                      std::to_string(offset);
    check_notes(out, length, false, what);
    for (const std::size_t other : {0U, 2U, 3U}) {
      const auto from = lane[other].position.value, to = from + lane[other].length_ticks;
      REQUIRE_EQ(starting_in(out, from, to), starting_in(part, from, to),
                 what + ": others untouched");
    }
    const auto in = starting_in(out, bar, 2 * bar);
    REQUIRE_EQ(rhythm(in), rhythm(starting_in(part, bar, 2 * bar)), what + ": the rhythm is kept");
    changed += in != starting_in(part, bar, 2 * bar) ? 1 : 0;
    const auto again = recognise(out, {length});
    require(again.size() == lane.size() && sounds_alike(again[1], lane[1]) &&
                again[1].position == lane[1].position,
            what + ": still Dm9, read as " + names(again));
  }
  REQUIRE_GE(changed, 1, "the region is re-voiced");
  // Without revoice the same lane is kept as it is.
  REQUIRE_EQ(rewrite(part, lane, {length}, VoicingOptions{}, std::nullopt), part,
             "no revoice, no change");
}

void rewrite_writes_into_silence() {
  const auto lane = lane_of({chord(A, minor_ninth), chord(D, minor_ninth)});
  const auto length = span_end(lane);
  const auto first = with_ids(write(std::span(lane).first(1), VoicingOptions{}));
  // The clip's second bar is silent: the lane as read keeps it silent.
  const auto read = recognise(first, {length});
  require(read.size() == 1 && read.front().length_ticks == length,
          "silence inherits: " + names(read));
  REQUIRE_EQ(rewrite(first, read, {length}, VoicingOptions{}), first,
             "inherited silence stays silent");
  // A new chord there is written as held notes.
  const auto out = rewrite(first, lane, {length}, VoicingOptions{});
  REQUIRE_EQ(starting_in(out, 0, bar), first, "the first bar is kept");
  const auto second = starting_in(out, bar, length);
  require(!second.empty(), "the second bar has the chord");
  for (const auto& n : second)
    require(n.start.value == bar && n.end.value == length, "held for the whole region");
  REQUIRE_EQ(recognise(out, {length}), lane, "reads back as the lane");
}

// ---- write ----
void write_holds_each_chord_with_one_bass() {
  LabOptions o;
  o.devices = device_bit(Device::drone) | device_bit(Device::walk) | device_bit(Device::passing);
  for (const auto& style :
       {VoicingStyle::close, VoicingStyle::drop2, VoicingStyle::spread, VoicingStyle::rootless}) {
    VoicingOptions v;
    v.style = style;
    for (const auto& suggestion : suggest(o)) {
      const auto& lane = suggestion.chords;
      const auto notes = write(lane, v);
      check_notes(notes, span_end(lane), false, "write " + names(lane));
      for (const auto& r : lane) {
        const auto from = r.position.value, to = from + r.length_ticks;
        int basses = 0;
        for (const auto& n : starting_in(notes, from, to)) {
          REQUIRE_EQ(n.start.value, from, "held for the whole region");
          REQUIRE_EQ(n.end.value, to, "held for the whole region");
          REQUIRE_EQ(n.velocity, written_velocity, "velocity and id");
          REQUIRE_EQ(n.id, unassigned_note_id(), "velocity and id");
          if (n.key < bass_role_below) {
            ++basses;
            REQUIRE_GE(n.key, comping_bass_lowest, "bass from E2");
            REQUIRE_LT(n.key, comping_bass_lowest + 12, "bass from E2");
            REQUIRE_EQ(n.key % pitch_classes, r.chord.bass.value_or(r.chord.root),
                       "the chord's bass");
          } else {
            REQUIRE_GE(n.key, std::max(v.lowest, bass_role_below), "upper voices within the range");
            REQUIRE_LE(n.key, v.highest + drone_headroom, "upper voices within the range");
          }
        }
        REQUIRE_EQ(basses, 1, "one bass per region");
      }
    }
  }
}

// The bass switch: off, nothing sounds in the bass's register and each
// chord's own bass (its root, or an inversion's note) is its lowest note, so
// the chords read back unchanged in every style; rewriting turns the bass
// off and on again in the same rhythm.
void the_bass_switches_off_and_on() {
  const auto lane = lane_of({chord(A, minor_add9), chord(F, add9), chord(C, add9, E),
                             chord(G, major), chord(A, minor, C)});
  const timeline::TickPosition length{span_end(lane)};
  const auto chords_of = [](std::span<const ChordRegion> regions) {
    std::vector<ChordSymbol> out;
    for (const auto& r : regions)
      out.push_back(r.chord);
    return out;
  };
  for (auto v :
       {open_voicing(), VoicingOptions{.style = VoicingStyle::close},
        VoicingOptions{.style = VoicingStyle::drop2}, VoicingOptions{.style = VoicingStyle::spread},
        VoicingOptions{.style = VoicingStyle::rootless}}) {
    const auto split = std::max<int>(v.lowest, v.bass_lowest + pitch_classes);
    const auto with_bass = write(lane, v);
    v.bass = false;
    const auto without = write(lane, v);
    for (const auto& r : lane) {
      const auto here = starting_in(without, r.position.value, r.position.value + r.length_ticks);
      require(!here.empty(), "every chord still sounds");
      const auto lowest = std::ranges::min(here, {}, &MidiNote::key);
      REQUIRE_EQ(lowest.key % pitch_classes, r.chord.bass.value_or(r.chord.root),
                 name(r.chord, a_minor) + ": its bass is its lowest note");
    }
    require(std::ranges::none_of(without, [&](const MidiNote& n) { return n.key < split; }),
            "nothing in the bass's register");
    REQUIRE_EQ(chords_of(recognise(without, length)), chords_of(lane),
               "the chords read back without a bass");

    // The switch on a written clip: off drops the bass, on brings it back.
    const auto off = rewrite(with_bass, lane, length, v);
    require(std::ranges::none_of(off, [&](const MidiNote& n) { return n.key < split; }),
            "off drops the bass");
    REQUIRE_EQ(chords_of(recognise(off, length)), chords_of(lane), "and keeps the chords");
    v.bass = true;
    const auto on = rewrite(off, lane, length, v);
    for (const auto& r : lane) {
      const auto here = starting_in(on, r.position.value, r.position.value + r.length_ticks);
      REQUIRE_EQ(std::ranges::count_if(here, [&](const MidiNote& n) { return n.key < split; }), 1,
                 "on brings one bass back per chord");
    }
    REQUIRE_EQ(chords_of(recognise(on, length)), chords_of(lane), "and the chords read the same");
  }
}

// Without the bass a chord keeps the shape it has over one: its own bass sits
// just under the voicing, a third or more below the next voice, and is never
// folded up into a low cluster (Hope in C minor once wrote F9 as F G A C E♭
// and G7 as G B D F).
void the_bass_off_keeps_the_voicing_open() {
  constexpr std::uint8_t e_flat = 3;
  const auto lane = lane_of({chord(C, minor_ninth), chord(e_flat, major_ninth),
                             chord(G, dominant_seventh), chord(F, dominant_ninth)});
  auto v = open_voicing();
  const auto with_bass = write(lane, v);
  v.bass = false;
  const auto without = write(lane, v);
  const auto split = std::max<int>(v.lowest, v.bass_lowest + pitch_classes);
  for (const auto& r : lane) {
    const auto end = r.position.value + r.length_ticks;
    auto here = starting_in(without, r.position.value, end);
    std::ranges::sort(here, {}, &MidiNote::key);
    REQUIRE_GE(here.size(), 2, name(r.chord, c_minor) + ": sounds");
    REQUIRE_EQ(here[0].key % pitch_classes, r.chord.root,
               name(r.chord, c_minor) + ": its root is lowest");
    REQUIRE_GE(here[1].key - here[0].key, minor_third,
               name(r.chord, c_minor) + ": a third or more over its bass");
  }
  // Open without the bass is the voicing it has over the bass, its lowest
  // voice the bass's octave: above that, G7's voices are the same.
  const auto at = lane[2].position.value;
  const auto end = at + lane[2].length_ticks;
  std::vector<int> upper_with;
  std::vector<int> all_without;
  for (const auto& n : starting_in(with_bass, at, end))
    if (n.key >= split)
      upper_with.push_back(n.key);
  for (const auto& n : starting_in(without, at, end))
    all_without.push_back(n.key);
  std::ranges::sort(upper_with);
  std::ranges::sort(all_without);
  REQUIRE_EQ(all_without.front() % pitch_classes, G, "G7's lowest voice is its root");
  REQUIRE_EQ(std::vector<int>(upper_with.begin() + 1, upper_with.end()),
             std::vector<int>(all_without.begin() + 1, all_without.end()),
             "G7 keeps its voicing above it");
}

// A clip at the note limit reads and rewrites (budgeted by CTest).
void recognise_scales_to_full_clips() {
  const auto loop = lane_of({chord(A, minor_ninth), chord(D, minor_ninth), chord(F, major_ninth),
                             chord(E, dominant_seventh)},
                            beat);
  const auto per_loop = comp_part(write(loop, VoicingOptions{})).size();
  const auto loops = project::max_midi_notes_per_clip / std::max<std::size_t>(per_loop, 1);
  const auto lane = tile(loop, {0}, {static_cast<std::int64_t>(loops) * span_end(loop)});
  const auto length = span_end(lane);
  const auto part = comp_part(write(lane, VoicingOptions{}));
  REQUIRE_LE(part.size(), project::max_midi_notes_per_clip, "a full clip");
  REQUIRE_GT(part.size() * 2, project::max_midi_notes_per_clip, "a full clip");
  const auto read = recognise(part, {length});
  REQUIRE_TRUE(!read.empty(), "read to the end");
  REQUIRE_EQ(read.back().position.value + read.back().length_ticks, length, "read to the end");
  auto edited = read;
  for (std::size_t i = 0; i < edited.size(); i += 2)
    edited[i].chord = chord(C, major_seventh);
  const auto out = rewrite(part, edited, {length}, VoicingOptions{});
  check_notes(out, length, false, "full clip");
}

// ---- sitting like the records ----
// Measured over PML's Iconic Chord Progressions (52 progressions, 718 chords
// of three notes or more, as the producers voiced them): the bass's median
// is MIDI 41, the lowest chord voice 50.6, the top 64.4; 3.7 voices over the
// bass, the first 9.8 semitones up; a semitone between neighbours in 0.2 %
// of chords, a second under A3 in 1.4 %, three voices within a major third
// in 1.2 %; the top voice leaps more than a major third in 20 % of changes.
struct Sitting {
  double chords = 0, semitones = 0, low_seconds = 0, clusters = 0, voices = 0, lowest = 0, top = 0,
         bass = 0, first_gap = 0, changes = 0, top_leaps = 0;
};

Sitting sit(const std::vector<std::vector<ChordRegion>>& lanes, const VoicingOptions& voicing) {
  Sitting s;
  const int split = bass_split(voicing);
  for (const auto& lane : lanes) {
    const auto notes = write(lane, voicing);
    std::vector<int> before;
    for (const auto& r : lane) {
      std::vector<int> keys;
      for (const auto& n : starting_in(notes, r.position.value, r.position.value + r.length_ticks))
        keys.push_back(n.key);
      std::ranges::sort(keys);
      if (keys.size() < 2)
        continue;
      const int bass = keys.front() < split ? keys.front() : -1;
      std::vector<int> up(keys.begin() + (bass >= 0 ? 1 : 0), keys.end());
      s.chords += 1;
      bool semitones = false, low_seconds = false, clusters = false;
      for (std::size_t i = 1; i < up.size(); ++i) {
        semitones = semitones || up[i] - up[i - 1] == 1;
        low_seconds = low_seconds || (up[i] - up[i - 1] <= 2 && up[i - 1] < 57);
        clusters = clusters || (i >= 2 && up[i] - up[i - 2] <= 4);
      }
      s.semitones += semitones ? 1 : 0;
      s.low_seconds += low_seconds ? 1 : 0;
      s.clusters += clusters ? 1 : 0;
      s.voices += static_cast<double>(up.size());
      s.lowest += up.front();
      s.top += up.back();
      if (bass >= 0) {
        s.bass += bass;
        s.first_gap += up.front() - bass;
      }
      if (!before.empty()) {
        s.changes += 1;
        s.top_leaps += std::abs(up.back() - before.back()) > 4 ? 1 : 0;
      }
      before = up;
    }
  }
  return s;
}

std::vector<std::vector<ChordRegion>> lab_corpus(std::initializer_list<Colour> colours) {
  std::vector<std::vector<ChordRegion>> lanes;
  for (const double brightness : {0.1, 0.5, 0.9})
    for (const double restlessness : {0.1, 0.5, 0.9})
      for (std::size_t sh = 0; sh < shape_count; ++sh)
        for (const auto colour : colours)
          for (std::uint64_t seed = 0; seed < 2; ++seed) {
            LabOptions o;
            o.key_mode = KeyMode::minor;
            o.mood = {brightness, restlessness};
            o.shape = static_cast<Shape>(sh);
            o.colour = colour;
            o.devices = 0;
            o.seed = seed;
            for (const auto& suggestion : suggest(o))
              lanes.push_back(suggestion.chords);
          }
  return lanes;
}

void written_chords_sit_like_the_records() {
  const auto lanes = lab_corpus({Colour::natural, Colour::triads, Colour::add9, Colour::sus2,
                                 Colour::sevenths, Colour::lush});
  const auto s = sit(lanes, open_voicing());
  REQUIRE_GT(s.chords, 1000, "a corpus to measure");
  const auto share = [&](double count) { return count / s.chords; };
  const auto mean = [&](double sum) { return sum / s.chords; };
  REQUIRE_LE(share(s.semitones), 0.005, "a semitone between neighbours almost never");
  REQUIRE_LE(share(s.low_seconds), 0.02, "a second under A3 almost never");
  REQUIRE_LE(share(s.clusters), 0.02, "three voices in a major third almost never");
  REQUIRE_GE(mean(s.voices), 3.4, "three or four voices over the bass");
  REQUIRE_LE(mean(s.voices), 4.4, "three or four voices over the bass");
  REQUIRE_GE(mean(s.bass), 39.0, "the bass near F2");
  REQUIRE_LE(mean(s.bass), 44.0, "the bass near F2");
  REQUIRE_GE(mean(s.lowest), 49.0, "the lowest voice near D3");
  REQUIRE_LE(mean(s.lowest), 53.0, "the lowest voice near D3");
  REQUIRE_GE(mean(s.top), 62.0, "the top near E4");
  REQUIRE_LE(mean(s.top), 68.0, "the top near E4");
  REQUIRE_GE(mean(s.first_gap), 8.0, "the first voice an octave or fifth up");
  REQUIRE_LE(mean(s.first_gap), 12.0, "the first voice an octave or fifth up");
  REQUIRE_LE(s.top_leaps / s.changes, 0.25, "the top voice leaps no more than the records'");
  // Natural colour is the records' own: the same register, the top lower.
  const auto natural = sit(lab_corpus({Colour::natural}), open_voicing());
  REQUIRE_LE(natural.top / natural.chords, 66.5, "natural chords' tops sit lower");
}

// A drone rings at one pitch: once placed, the same key through every chord
// it can sit over cleanly.
void the_drone_rings_at_one_pitch() {
  int lanes = 0, held = 0;
  for (std::size_t sh = 0; sh < shape_count; ++sh)
    for (const auto colour : {Colour::natural, Colour::add9, Colour::sevenths}) {
      LabOptions o;
      o.key_mode = KeyMode::minor;
      o.shape = static_cast<Shape>(sh);
      o.colour = colour;
      o.devices = device_bit(Device::drone);
      const auto lane = suggest(o)[0].chords;
      if (!lane.front().top)
        continue;
      const auto notes = write(lane, open_voicing());
      std::optional<int> pitch;
      bool steady = true;
      for (const auto& r : lane) {
        std::optional<int> here;
        for (const auto& n :
             starting_in(notes, r.position.value, r.position.value + r.length_ticks))
          if (n.key % pitch_classes == *r.top)
            here = std::max<int>(here.value_or(0), n.key);
        REQUIRE_OK(here, names(lane) + ": the drone sounds over every chord");
        steady = steady && (!pitch || *pitch == *here);
        pitch = here;
      }
      ++lanes;
      held += steady ? 1 : 0;
    }
  REQUIRE_GT(lanes, 20, "drone lanes to measure");
  REQUIRE_GE(held * 10, lanes * 8,
             "the drone holds one pitch in most lanes: " + std::to_string(held) + " of " +
                 std::to_string(lanes));
}

} // namespace

// Layers follow the chords: the pad holds them an octave up without the
// bass and reads as them; the pluck sits high and close; the running tonic
// pulses through every chord, each beat accented; the bass plays each
// chord's bass on the off-beats or rolls the 16ths between the beats.
void layers_follow_the_chords() {
  const std::vector<ChordRegion> lane{{{0}, bar, chord(A, minor)},
                                      {{bar}, bar, chord(F, major)},
                                      {{2 * bar}, bar, chord(C, major, E)}};
  const timeline::TickPosition length{3 * bar};
  const auto pad = pad_layer(lane);
  const auto open = open_voicing();
  for (const auto& n : pad)
    REQUIRE_GE(n.key, open.lowest + pad_octave_up, "the pad sits an octave above the Open chord");
  const auto pad_reads = recognise(pad, length);
  REQUIRE_EQ(pad_reads.size(), lane.size(), "the pad reads as the three chords");
  for (std::size_t i = 0; i < lane.size(); ++i)
    REQUIRE_EQ(pad_reads[i].chord.root, lane[i].chord.root, "with their roots");
  for (const auto& n : pluck_layer(lane))
    require(n.key >= pluck_lowest && n.key <= pluck_highest, "the pluck sits high");

  const auto tonic = running_tonic(length, A, Pulse::eighths);
  REQUIRE_EQ(tonic.size(), 3 * 8, "the tonic runs in 8ths through all three bars");
  for (const auto& n : tonic) {
    REQUIRE_EQ(n.key % 12, A, "on the key's tonic");
    REQUIRE_GE(n.key, tonic_lowest, "on the key's tonic");
    REQUIRE_EQ(n.velocity, (n.start.value % beat == 0 ? accent_velocity : pulse_velocity),
               "each beat's first pulse accented");
    REQUIRE_EQ(n.end.value - n.start.value, step, "half the pulse long");
  }
  REQUIRE_GE(running_tonic(length, A, Pulse::dotted_eighths).front().end.value -
                 running_tonic(length, A, Pulse::dotted_eighths).front().start.value,
             step, "dotted 8ths pulse too");

  const auto offbeat = bass_line(lane, BassStyle::offbeat);
  REQUIRE_EQ(offbeat.size(), 3 * 4, "the off-beat bass plays four a bar");
  for (const auto& n : offbeat)
    require(n.start.value % beat == 2 * step && n.key < open_lowest, "on the off-beat 8ths, low");
  REQUIRE_EQ(offbeat.back().key % 12, E, "under a slash chord, its bass");
  const auto rolling = bass_line(lane, BassStyle::rolling);
  REQUIRE_EQ(rolling.size(), 3 * 12, "the rolling bass fills the 16ths between the beats");
  for (const auto& n : rolling)
    REQUIRE_NE(n.start.value % beat, 0, "leaving each beat to the kick");
  REQUIRE_EQ(bass_line(lane, BassStyle::held).size(), lane.size(), "held: one per chord");
}

// Held rhythm: anticipation moves each change early and the chords still
// read as themselves; push and land touches the next chord once, short and
// softer, before it lands, and never re-strikes a chord otherwise.
void held_chords_anticipate_and_land() {
  const std::vector<ChordRegion> lane{{{0}, 2 * bar, chord(A, minor)},
                                      {{2 * bar}, 2 * bar, chord(F, major)},
                                      {{4 * bar}, 2 * bar, chord(C, major)},
                                      {{6 * bar}, 2 * bar, chord(G, major)}};
  const timeline::TickPosition length{8 * bar};
  const auto eighth = anticipate(lane, Anticipation::eighth);
  REQUIRE_EQ(eighth.front().position.value, 0, "the first chord stays on the downbeat");
  for (std::size_t i = 1; i < eighth.size(); ++i)
    require(eighth[i].position.value == lane[i].position.value - 2 * step &&
                eighth[i - 1].position.value + eighth[i - 1].length_ticks ==
                    eighth[i].position.value,
            "each change arrives an 8th early, the chord before shortened");
  const auto varied = anticipate(lane, Anticipation::varied);
  for (std::size_t i = 1; i < varied.size(); ++i)
    REQUIRE_EQ(lane[i].position.value - varied[i].position.value,
               anticipation_sixteenths(Anticipation::varied, i) * step,
               "varied anticipation cycles its amounts");
  REQUIRE_EQ(anticipate(lane, Anticipation::none), std::vector(lane.begin(), lane.end()),
             "none moves nothing");
  const auto held = write(varied, open_voicing());
  const auto read = recognise(held, length);
  REQUIRE_EQ(read.size(), lane.size(), "the anticipated chords still read as four");
  for (std::size_t i = 0; i < lane.size(); ++i)
    REQUIRE_EQ(read[i].chord.root, lane[i].chord.root, "with their roots");

  const auto landed = push_and_land(lane, open_voicing(), Anticipation::none);
  std::size_t touches = 0;
  for (const auto& n : landed)
    touches += n.velocity == touch_velocity ? 1 : 0;
  REQUIRE_GT(touches, 0, "push and land touches the next chords");
  for (const auto& n : landed) {
    if (n.velocity != touch_velocity)
      continue;
    REQUIRE_EQ(n.end.value - n.start.value, touch_sixteenths * step, "a short touch");
    REQUIRE_EQ((n.start.value + 3 * step) % (2 * bar), 0, "a dotted 8th before the change");
  }
  std::set<std::int64_t> onsets;
  for (const auto& n : landed)
    if (n.velocity != touch_velocity)
      onsets.insert(n.start.value);
  REQUIRE_EQ(onsets.size(), lane.size(), "otherwise each chord is struck once and held");
}

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "harmony clip tests passed",
      {
          {"identify_reads_every_voiced_chord", identify_reads_every_voiced_chord},
          {"recognise_reads_written_lab_lanes", recognise_reads_written_lab_lanes},
          {"recognise_ignores_passing_notes_and_inherits",
           recognise_ignores_passing_notes_and_inherits},
          {"rewrite_changes_only_the_edited_region", rewrite_changes_only_the_edited_region},
          {"rewrite_keeps_notes_outside_the_lane", rewrite_keeps_notes_outside_the_lane},
          {"rewrite_split_cuts_held_notes", rewrite_split_cuts_held_notes},
          {"rewrite_revoices_a_region_in_its_rhythm", rewrite_revoices_a_region_in_its_rhythm},
          {"rewrite_writes_into_silence", rewrite_writes_into_silence},
          {"write_holds_each_chord_with_one_bass", write_holds_each_chord_with_one_bass},
          {"recognise_scales_to_full_clips", recognise_scales_to_full_clips},
          {"layers_follow_the_chords", layers_follow_the_chords},
          {"held_chords_anticipate_and_land", held_chords_anticipate_and_land},
          {"the_bass_switches_off_and_on", the_bass_switches_off_and_on},
          {"the_bass_off_keeps_the_voicing_open", the_bass_off_keeps_the_voicing_open},
          {"written_chords_sit_like_the_records", written_chords_sit_like_the_records},
          {"the_drone_rings_at_one_pitch", the_drone_rings_at_one_pitch},
      });
}
