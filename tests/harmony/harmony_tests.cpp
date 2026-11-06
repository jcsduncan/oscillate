#include "oscillate/harmony/harmony.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Harmony oracles (ADR-0083). Expected chords come from music theory (the
// modes stacked in thirds, the usual functions), not from the code's output.
namespace {

using namespace oscillate;
using namespace oscillate::harmony;
using enum harmony::ChordQuality;
using project::KeyMode;

constexpr std::int64_t beat = timeline::ticks_per_quarter;
constexpr std::int64_t bar = 4 * beat;
constexpr std::uint8_t A = 9, B = 11, C = 0, D = 2, E = 4, F = 5, G = 7;
constexpr std::uint8_t g_sharp = 8, b_flat = 10, d_sharp = 3, a_flat = 8, f_sharp = 6;
const MusicalKey a_minor{A, KeyMode::minor};
const MusicalKey c_major{C, KeyMode::major};

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

void check_valid(std::span<const ChordRegion> lane, const std::string& what) {
  for (std::size_t i = 0; i < lane.size(); ++i) {
    REQUIRE_GT(lane[i].length_ticks, 0, what + ": region " + std::to_string(i) + " has no length");
    REQUIRE_GE(lane[i].position.value, 0, what + ": region before zero");
    REQUIRE_LT(lane[i].chord.root, pitch_classes, what + ": root out of range");
    if (i > 0) {
      REQUIRE_LE(lane[i - 1].position.value + lane[i - 1].length_ticks, lane[i].position.value,
                 what + ": regions overlap or are out of order at " + std::to_string(i));
    }
  }
}

std::int64_t span_end(std::span<const ChordRegion> lane) {
  return lane.back().position.value + lane.back().length_ticks;
}

std::string names(std::span<const ChordRegion> lane, const MusicalKey& key) {
  std::string out;
  for (const auto& r : lane) {
    out += (out.empty() ? "" : " · ") + name(r.chord, key);
  }
  return out;
}

void intervals_match_the_table() {
  struct Row {
    ChordQuality q;
    std::vector<std::uint8_t> semitones;
  };
  const std::vector<Row> rows{
      {major, {0, 4, 7}},
      {minor, {0, 3, 7}},
      {diminished, {0, 3, 6}},
      {sus2, {0, 2, 7}},
      {add9, {0, 4, 7, 14}},
      {sixth, {0, 4, 7, 9}},
      {minor_sixth, {0, 3, 7, 9}},
      {major_seventh, {0, 4, 7, 11}},
      {minor_seventh, {0, 3, 7, 10}},
      {dominant_seventh, {0, 4, 7, 10}},
      {half_diminished, {0, 3, 6, 10}},
      {diminished_seventh, {0, 3, 6, 9}},
      {seventh_sus4, {0, 5, 7, 10}},
      {major_ninth, {0, 4, 7, 11, 14}},
      {minor_ninth, {0, 3, 7, 10, 14}},
      {dominant_ninth, {0, 4, 7, 10, 14}},
      {dominant_seventh_flat_ninth, {0, 4, 7, 10, 13}},
      {minor_eleventh, {0, 3, 7, 10, 14, 17}},
      {dominant_thirteenth, {0, 4, 10, 14, 21}},
      {minor_add9, {0, 3, 7, 14}},
      {sus4, {0, 5, 7}},
      {power, {0, 7}},
  };
  REQUIRE_EQ(rows.size(), harmony::last_chord_quality, "every quality is tested");
  for (const auto& r : rows) {
    const auto iv = intervals(r.q);
    REQUIRE_EQ(iv.count, r.semitones.size(), "count for " + std::string(suffix(r.q)));
    for (std::size_t i = 0; i < r.semitones.size(); ++i) {
      REQUIRE_EQ(iv.semitones[i], r.semitones[i], "interval for " + std::string(suffix(r.q)));
    }
  }
  REQUIRE_EQ(pitch_class_set(chord(A, minor_seventh, G)),
             ((1U << A) | (1U << C) | (1U << E) | (1U << G)), "Am7/G sounds A C E G");
  REQUIRE_TRUE(is_dominant(seventh_sus4), "dominant family");
  REQUIRE_TRUE(!is_dominant(major_seventh), "dominant family");
  REQUIRE_TRUE(is_minor(half_diminished), "minor family");
  REQUIRE_TRUE(!is_minor(sixth), "minor family");
}

void names_and_roman_numerals() {
  const auto expect = [](const ChordSymbol& c, const MusicalKey& k, const std::string& n,
                         const std::string& r) {
    REQUIRE_EQ(name(c, k), n, "name " + name(c, k) + " != " + n);
    REQUIRE_EQ(roman(c, k), r, "roman " + roman(c, k) + " != " + r);
  };
  expect(chord(A, minor_ninth), a_minor, "Am9", "i");
  expect(chord(D, minor_ninth), a_minor, "Dm9", "iv");
  expect(chord(F, major_ninth), a_minor, "Fmaj9", "VI");
  expect(chord(E, dominant_seventh), a_minor, "E7", "V⁷");
  expect(chord(g_sharp, diminished_seventh), a_minor, "G♯°7", "♮vii°");
  expect(chord(b_flat, major_seventh), a_minor, "B♭maj7", "♭II");
  expect(chord(B, half_diminished), a_minor, "Bm7♭5", "iiø");
  expect(chord(A, minor_ninth, G), a_minor, "Am9/G", "i");
  expect(chord(G, dominant_seventh), c_major, "G7", "V⁷");
  expect(chord(D, minor_seventh), c_major, "Dm7", "ii");
  expect(chord(b_flat, major_seventh), c_major, "B♭maj7", "♭VII");
  // Flat keys spell with flats, sharp keys with sharps.
  expect(chord(b_flat, major), {F, KeyMode::major}, "B♭", "IV");
  expect(chord(a_flat, major_seventh), {3, KeyMode::major}, "A♭maj7", "IV");
  expect(chord(d_sharp, half_diminished), {E, KeyMode::major}, "D♯m7♭5", "viiø");
  expect(chord(f_sharp, minor), {D, KeyMode::major}, "F♯m", "iii");
  REQUIRE_EQ(role(chord(A, minor_ninth), a_minor), Role::home, "i is home");
  REQUIRE_EQ(role(chord(D, minor_ninth), a_minor), Role::lift, "iv lifts");
  REQUIRE_EQ(role(chord(E, dominant_seventh), a_minor), Role::pull, "V7 pulls in minor");
  REQUIRE_EQ(role(chord(b_flat, major_seventh), a_minor), Role::colour, "♭II colours");
}

void tension_orders_by_pull() {
  const int i = tension(chord(A, minor), a_minor);
  const int iv = tension(chord(D, minor), a_minor);
  const int v7 = tension(chord(E, dominant_seventh), a_minor);
  const int v7b9 = tension(chord(E, dominant_seventh_flat_ninth), a_minor);
  REQUIRE_LT(i, iv, "i < iv < V7 < V7♭9");
  REQUIRE_LT(iv, v7, "i < iv < V7 < V7♭9");
  REQUIRE_LT(v7, v7b9, "i < iv < V7 < V7♭9");
  REQUIRE_GT(tension(chord(D, minor, A), a_minor), iv, "a pedal under iv adds tension");
  REQUIRE_LT(tension(chord(E, seventh_sus4), a_minor), v7, "a sus eases the dominant");
  REQUIRE_LT(tension(chord(C, major), c_major), tension(chord(G, dominant_seventh), c_major),
             "I < V7 in major");
  for (int q = 1; q <= harmony::last_chord_quality; ++q) {
    for (std::uint8_t r = 0; r < pitch_classes; ++r) {
      const int t = tension(chord(r, static_cast<ChordQuality>(q)), a_minor);
      REQUIRE_GE(t, 0, "tension within 0..100");
      REQUIRE_LE(t, 100, "tension within 0..100");
    }
  }
}

void voicing_moves_least_and_stays_in_range() {
  VoicingOptions close;
  close.style = VoicingStyle::close;
  const auto am7 = voice(chord(A, minor_seventh), std::nullopt, close, nullptr);
  const auto cmaj7 = voice(chord(C, major_seventh), std::nullopt, close, &am7);
  // Am7 -> Cmaj7 shares C, E and G: only A moves, a whole tone up to B.
  REQUIRE_EQ(movement(am7, cmaj7), 2,
             "Am7 to Cmaj7 moves one voice a whole tone, moved " +
                 std::to_string(movement(am7, cmaj7)));
  for (const auto style :
       {VoicingStyle::close, VoicingStyle::drop2, VoicingStyle::spread, VoicingStyle::rootless}) {
    for (int reg : {-7, 0, 7}) {
      VoicingOptions o;
      o.style = style;
      o.register_offset = reg;
      const auto lane =
          lane_of({chord(A, minor_ninth), chord(D, minor_ninth), chord(F, major_ninth),
                   chord(E, dominant_seventh_flat_ninth), chord(g_sharp, diminished_seventh)});
      const auto v = voice_lane(lane, o);
      for (std::size_t i = 0; i < v.size(); ++i) {
        REQUIRE_GE(v[i].count, 3, "a voicing has at least three notes");
        REQUIRE_GE(v[i].notes[0], o.lowest, "voicing within range");
        REQUIRE_LE(v[i].notes[v[i].count - 1], o.highest, "voicing within range");
        require(std::is_sorted(v[i].notes.begin(), v[i].notes.begin() + v[i].count), "ascending");
        // Every note is a chord tone.
        const auto set = pitch_class_set(lane[i].chord);
        for (std::uint8_t k = 0; k < v[i].count; ++k) {
          REQUIRE_NE((set & (1U << (v[i].notes[k] % pitch_classes))), 0, "only chord tones");
        }
      }
    }
  }
  VoicingOptions rootless;
  rootless.style = VoicingStyle::rootless;
  const auto r = voice(chord(D, minor_ninth), std::nullopt, rootless, nullptr);
  for (std::uint8_t k = 0; k < r.count; ++k) {
    REQUIRE_NE(r.notes[k] % pitch_classes, D, "rootless leaves the root out");
  }
  // The top line holds.
  VoicingOptions hold;
  hold.top_line = TopLine::hold;
  const auto lane = lane_of({chord(A, minor_ninth), chord(D, minor_ninth), chord(F, major_ninth)});
  const auto held = voice_lane(lane, hold);
  for (std::size_t i = 1; i < held.size(); ++i) {
    const int was = held[i - 1].notes[held[i - 1].count - 1];
    const int now = held[i].notes[held[i].count - 1];
    REQUIRE_NEAR(now, was, 2, "a held top line moves at most a step");
  }
}

void drop2_is_close_with_second_from_top_dropped() {
  VoicingOptions drop2;
  drop2.style = VoicingStyle::drop2;
  for (const auto q : {minor_seventh, major_seventh, dominant_seventh, half_diminished}) {
    for (std::uint8_t root = 0; root < pitch_classes; ++root) {
      const auto v = voice(chord(root, q), std::nullopt, drop2, nullptr);
      REQUIRE_EQ(v.count, 4, "four voices");
      // Raising the lowest voice an octave restores a close voicing, with
      // the raised voice second from the top.
      std::vector<int> close(v.notes.begin(), v.notes.begin() + v.count);
      const int raised = close.front() + 12;
      close.front() = raised;
      std::ranges::sort(close);
      REQUIRE_LT(close.back() - close.front(), 12, "the restored voicing is close");
      REQUIRE_EQ(close[close.size() - 2], raised, "the dropped voice was second from the top");
    }
  }
}

void drone_sits_on_top() {
  const VoicingOptions o;
  const auto v = voice(chord(A, minor_seventh), E, o, nullptr);
  REQUIRE_EQ(v.notes[v.count - 1] % pitch_classes, E, "E on top");
  for (std::uint8_t k = 0; k + 1 < v.count; ++k) {
    REQUIRE_LT(v.notes[k], v.notes[v.count - 1], "the drone is above every chord tone");
  }
  const auto plain = voice(chord(F, major_seventh), std::nullopt, o, nullptr);
  const auto droned = voice(chord(F, major_seventh), E, o, nullptr);
  REQUIRE_EQ(droned.notes[droned.count - 1] % pitch_classes, E, "E on top of Fmaj7");
  require(droned.count == plain.count || droned.count == plain.count + 1,
          "at most one added voice");
  REQUIRE_EQ(bass_note(chord(A, minor_ninth, G), 33), 43, "the bass of Am9/G from A1 is G2");
  REQUIRE_EQ(bass_note(chord(A, minor_ninth), 33), 33, "the bass of Am9 from A1 is A1");
}

void voicing_is_deterministic_and_callback_safe() {
  const VoicingOptions o;
  static_assert(noexcept(voice(std::declval<const ChordSymbol&>(), std::nullopt,
                               std::declval<const VoicingOptions&>(), nullptr)));
  const auto a = voice(chord(D, minor_eleventh), G, o, nullptr);
  const auto b = voice(chord(D, minor_eleventh), G, o, nullptr);
  REQUIRE_EQ(a.count, b.count, "same chord, same voicing");
  REQUIRE_EQ(a.notes, b.notes, "same chord, same voicing");
  REQUIRE_LE(a.count, max_voices, "within the voice limit");
}

LabOptions lab(double brightness, Shape shape, Colour colour, double restlessness = 0.4) {
  LabOptions o;
  o.tonic = A;
  o.mood = {brightness, restlessness};
  o.shape = shape;
  o.colour = colour;
  o.chord_ticks = bar;
  o.devices = 0;
  return o;
}

void check_chords(std::span<const ChordRegion> lane, std::initializer_list<ChordSymbol> expected,
                  const std::string& what) {
  REQUIRE_EQ(lane.size(), expected.size(), what + ": " + names(lane, a_minor));
  std::size_t i = 0;
  for (const auto& e : expected) {
    REQUIRE_EQ(lane[i].chord, e, what + ": got " + names(lane, a_minor));
    ++i;
  }
}

void lab_loop_in_aeolian_is_the_minor_four() {
  const auto s = suggest(lab(0.3, Shape::loop, Colour::lush));
  // A Aeolian stacked in fifths of thirds: i9, VImaj9, IIImaj9, VII9.
  check_chords(s[0].chords,
               {chord(A, minor_ninth), chord(F, major_ninth), chord(C, major_ninth),
                chord(G, dominant_ninth)},
               "loop");
  check_valid(s[0].chords, "loop");
  REQUIRE_EQ(s[0].chords.front().position.value, 0, "four bars");
  REQUIRE_EQ(span_end(s[0].chords), 4 * bar, "four bars");
  require(s[0].label.starts_with("Aeolian"), "labelled with the mode");
  const auto plain = suggest(lab(0.3, Shape::loop, Colour::triads));
  check_chords(plain[0].chords,
               {chord(A, minor), chord(F, major), chord(C, major), chord(G, major)}, "plain loop");
  // The default colour: melodic house triads with their ninths.
  const auto add9s = suggest(lab(0.3, Shape::loop, Colour::add9));
  check_chords(add9s[0].chords,
               {chord(A, minor_add9), chord(F, add9), chord(C, add9), chord(G, add9)}, "add9 loop");
  const auto sus = suggest(lab(0.3, Shape::loop, Colour::sus2));
  check_chords(sus[0].chords, {chord(A, sus2), chord(F, sus2), chord(C, sus2), chord(G, sus2)},
               "sus2 loop");
}

void lab_modes_follow_brightness() {
  const auto vamp = [](double b) {
    return suggest(lab(b, Shape::vamp, Colour::sevenths))[0].chords;
  };
  REQUIRE_EQ(mode_at(0.0), Mode::phrygian, "the pad's ends");
  REQUIRE_EQ(mode_at(1.0), Mode::lydian, "the pad's ends");
  // With a song key the pad crosses only its family, from the key's own mode.
  using project::KeyMode;
  REQUIRE_EQ(mode_at(0.0, KeyMode::minor), Mode::phrygian,
             "a minor key's pad runs Phrygian to Dorian");
  REQUIRE_EQ(mode_at(1.0, KeyMode::minor), Mode::dorian,
             "a minor key's pad runs Phrygian to Dorian");
  REQUIRE_EQ(mode_at(0.0, KeyMode::major), Mode::mixolydian,
             "a major key's pad runs Mixolydian to Lydian");
  REQUIRE_EQ(mode_at(1.0, KeyMode::major), Mode::lydian,
             "a major key's pad runs Mixolydian to Lydian");
  REQUIRE_EQ(mode_at(home_brightness(KeyMode::minor), KeyMode::minor), Mode::aeolian,
             "the pad starts on the key's own mode");
  REQUIRE_EQ(mode_at(home_brightness(KeyMode::major), KeyMode::major), Mode::ionian,
             "the pad starts on the key's own mode");
  for (const double b : {0.0, 0.4, 0.99}) {
    auto options = lab(b, Shape::loop, Colour::sevenths);
    options.key_mode = KeyMode::minor;
    require(is_minor(suggest(options)[0].chords.front().chord.quality),
            "anywhere on a minor key's pad the tonic chord is minor");
  }
  // The vamp is i–VI.
  check_chords(vamp(0.05), {chord(A, minor_seventh), chord(F, major_seventh)}, "Phrygian i VI");
  check_chords(vamp(0.2), {chord(A, minor_seventh), chord(F, major_seventh)}, "Aeolian i VI");
  check_chords(vamp(0.45), {chord(A, minor_seventh), chord(f_sharp, half_diminished)},
               "Dorian i viø");
  check_chords(vamp(0.7), {chord(A, major_seventh), chord(f_sharp, minor_seventh)}, "Ionian I vi");
  // With triad colours no shape in any mode writes a diminished chord: the
  // nearest mode lends its plain chord (A Dorian's vamp is Am–F).
  check_chords(suggest(lab(0.45, Shape::vamp, Colour::triads))[0].chords,
               {chord(A, minor), chord(F, major)}, "Dorian vamp borrows F");
  for (const double b : {0.05, 0.2, 0.45, 0.55, 0.7, 0.95})
    for (std::size_t sh = 0; sh < shape_count; ++sh)
      for (const auto colour : {Colour::triads, Colour::add9, Colour::sus2}) {
        // Held by name: a range-for over a member of the returned array would dangle.
        const auto suggestions = suggest(lab(b, static_cast<Shape>(sh), colour));
        for (const auto& c : suggestions[0].chords)
          require(c.chord.quality != diminished && c.chord.quality != diminished_seventh &&
                      c.chord.quality != half_diminished,
                  "no diminished chord in " + std::string(shape_name(static_cast<Shape>(sh))));
      }
  REQUIRE_EQ(feel_at({0.0, 0.0}), "Hypnotic", "the corners");
  REQUIRE_EQ(feel_at({1.0, 1.0}), "Uplifting", "the corners");
}

void lab_restlessness_shapes_the_dominant() {
  const auto v_of = [](double restless) {
    return suggest(lab(0.3, Shape::fall, Colour::sevenths, restless))[0].chords[3].chord;
  };
  REQUIRE_EQ(v_of(0.4), chord(E, minor_seventh), "middling: the modal v");
  REQUIRE_EQ(v_of(0.1), chord(E, seventh_sus4), "still: v7sus4");
  REQUIRE_EQ(v_of(0.65), chord(E, dominant_seventh), "restless: V7");
  REQUIRE_EQ(v_of(0.9), chord(E, dominant_seventh), "even most restless stays a plain V7, no ♭9");
  // In Add9 the colour stays in the mode: A Aeolian has F, not the F♯ that
  // would be Em's ninth, so the v keeps its seventh instead. The lift is a
  // major V triad, the trance way (its ninth would be F♯ too).
  const auto v_add9 = [](double restless) {
    return suggest(lab(0.3, Shape::fall, Colour::add9, restless))[0].chords[3].chord;
  };
  REQUIRE_EQ(v_add9(0.4), chord(E, minor_seventh), "middling: the modal v, its colour in the mode");
  REQUIRE_EQ(v_add9(0.1), chord(E, seventh_sus4), "still: v7sus4");
  REQUIRE_EQ(v_add9(0.9), chord(E, major), "restless: a major V");
}

// A minor's notes, and whether one root is the scale step below another.
bool in_scale(std::uint8_t root) {
  constexpr std::array<std::uint8_t, 7> a_aeolian{A, B, C, D, E, F, G};
  return std::ranges::find(a_aeolian, root) != a_aeolian.end();
}
bool step_below(std::uint8_t lower, std::uint8_t upper) {
  constexpr int largest_step = 2;
  const int up = (upper - lower + 12) % 12;
  return up >= 1 && up <= largest_step;
}

void lab_devices_do_what_they_say() {
  const auto base = lab(0.3, Shape::loop, Colour::lush);
  const auto plain = suggest(base)[0].chords;
  const auto with = [&](Device d) {
    auto o = base;
    o.devices = device_bit(d);
    const auto lane = suggest(o)[0].chords;
    check_valid(lane, std::string(device_name(d)));
    require(lane.front().position.value == 0 && span_end(lane) == span_end(plain),
            std::string(device_name(d)) + " keeps the length");
    require(!device_hint(d).empty(), "every device has a hint");
    return lane;
  };
  for (const auto& r : with(Device::pedal)) {
    require(r.chord.root == A || r.chord.bass == A, "pedal: A under every chord");
  }
  {
    const auto lane = with(Device::drone);
    const auto top = lane.front().top;
    REQUIRE_OK(top, "drone: a top note");
    int holding = 0;
    for (const auto& r : lane) {
      REQUIRE_EQ(r.top, top, "drone: the same note throughout");
      holding += (pitch_class_set(r.chord) & (1U << *top)) != 0 ? 1 : 0;
    }
    // G is the only note in all four (Am9, Fmaj9, Cmaj9, G9).
    require(*top == G && holding == 4, "drone: the note every chord shares");
  }
  {
    const auto lane = with(Device::walk);
    bool any = false;
    for (std::size_t i = 1; i < lane.size(); ++i) {
      if (lane[i].chord.bass) {
        any = true;
        const auto prev = lane[i - 1].chord.bass.value_or(lane[i - 1].chord.root);
        const int down = (prev - *lane[i].chord.bass + 12) % 12;
        REQUIRE_GE(down, 1, "walk: the bass steps down");
        REQUIRE_LE(down, 4, "walk: the bass steps down");
      }
    }
    require(any, "walk: at least one inversion");
  }
  {
    const auto lane = with(Device::suspension);
    REQUIRE_EQ(lane.size(), plain.size() + 1, "suspension: one more region");
    const auto& sus = lane[lane.size() - 2];
    REQUIRE_EQ(sus.device, ChordDevice::suspension,
               "suspension: suspended, then resolved on the same root");
    REQUIRE_TRUE((sus.chord.quality == seventh_sus4 || sus.chord.quality == sus2),
                 "suspension: suspended, then resolved on the same root");
    REQUIRE_EQ(sus.chord.root, lane.back().chord.root,
               "suspension: suspended, then resolved on the same root");
  }
  {
    const auto lane = with(Device::passing);
    bool found = false;
    for (std::size_t i = 0; i + 1 < lane.size(); ++i) {
      if (lane[i].device == ChordDevice::passing) {
        found = true;
        REQUIRE_EQ(lane[i].length_ticks, beat,
                   "passing: the scale's chord a step below the next chord, for a beat");
        REQUIRE_NE(lane[i].chord.quality, diminished_seventh,
                   "passing: the scale's chord a step below the next chord, for a beat");
        REQUIRE_TRUE(in_scale(lane[i].chord.root),
                     "passing: the scale's chord a step below the next chord, for a beat");
        REQUIRE_TRUE(step_below(lane[i].chord.root, lane[i + 1].chord.root),
                     "passing: the scale's chord a step below the next chord, for a beat");
      }
    }
    require(found, "passing: placed before the biggest jump");
  }
  {
    const auto lane = with(Device::borrowed);
    REQUIRE_EQ(lane[2].chord.root, plain[2].chord.root, "borrowed: chord 3 from the other mode");
    REQUIRE_NE(lane[2].chord.quality, plain[2].chord.quality,
               "borrowed: chord 3 from the other mode");
    REQUIRE_EQ(lane[2].device, ChordDevice::borrowed, "borrowed: chord 3 from the other mode");
  }
  {
    const auto lane = with(Device::pull_home);
    REQUIRE_EQ(lane.back().chord, chord(E, dominant_seventh), "pull home: E7 on the last beat");
    REQUIRE_EQ(lane.back().length_ticks, beat, "pull home: E7 on the last beat");
  }
  {
    const auto lane = with(Device::slide);
    REQUIRE_EQ(lane.back().chord, chord(b_flat, minor_ninth),
               "slide: B♭m9 a semitone above Am9, for the last beat");
    REQUIRE_EQ(lane.back().length_ticks, beat,
               "slide: B♭m9 a semitone above Am9, for the last beat");
  }
  auto both = base;
  both.devices = device_bit(Device::pull_home) | device_bit(Device::slide);
  REQUIRE_EQ(suggest(both)[0].chords.back().device, ChordDevice::pull_home,
             "pull home wins over slide");
  auto all = base;
  all.devices = 0xFF;
  for (const auto& s : suggest(all)) {
    check_valid(s.chords, "every device at once");
    REQUIRE_EQ(span_end(s.chords), span_end(plain), "every device keeps the length");
  }
}

void lab_is_deterministic_and_variants_differ() {
  auto o = lab(0.3, Shape::circle, Colour::lush);
  o.devices = device_bit(Device::drone) | device_bit(Device::passing);
  const auto a = suggest(o);
  const auto b = suggest(o);
  for (std::size_t i = 0; i < variant_count; ++i) {
    REQUIRE_EQ(a[i].chords, b[i].chords, "same options, same suggestion");
    REQUIRE_EQ(a[i].label, b[i].label, "same options, same suggestion");
  }
  REQUIRE_NE(a[3].label.find("borrows from Dorian"), std::string::npos,
             "Aeolian borrows from Dorian");
  REQUIRE_NE(a[2].chords.front().chord, a[0].chords.front().chord, "rotate starts elsewhere");
  std::set<std::string> swaps;
  for (std::uint64_t seed = 0; seed < 20; ++seed) {
    o.seed = seed;
    swaps.insert(names(suggest(o)[1].chords, a_minor));
  }
  REQUIRE_GE(swaps.size(), 2, "More gives different swaps");
}

void next_after_the_dominant_is_home() {
  const auto after_v = next_candidates(chord(E, dominant_seventh), a_minor);
  REQUIRE_TRUE(!after_v.empty(), "at most eight");
  REQUIRE_LE(after_v.size(), next_candidate_limit, "at most eight");
  require(after_v.front().chord.root == A && is_minor(after_v.front().chord.quality),
          "V7 resolves to i, got " + name(after_v.front().chord, a_minor));
  REQUIRE_EQ(after_v.front().likelihood, 1.0, "the best is 1");
  for (std::size_t i = 1; i < after_v.size(); ++i) {
    REQUIRE_LE(after_v[i].likelihood, after_v[i - 1].likelihood, "sorted by likelihood");
  }
  const auto after_dim = next_candidates(chord(g_sharp, diminished_seventh), a_minor);
  REQUIRE_EQ(after_dim.front().chord.root, A, "a °7 rises a semitone");
  const auto after_i = next_candidates(chord(A, minor_ninth), a_minor);
  for (const auto& c : after_i) {
    require(!(c.chord == chord(A, minor_ninth)), "never the same chord");
  }
  REQUIRE_EQ(after_i.front().chord.root, D, "i goes to iv (down a fifth) first");
  const auto after_g7 = next_candidates(chord(G, dominant_seventh), c_major);
  REQUIRE_EQ(after_g7.front().chord.root, C, "G7 to C in C major");
}

void substitutions_and_hints_make_valid_lanes() {
  const auto lane = lane_of({chord(A, minor_ninth), chord(D, minor_ninth), chord(F, major_ninth),
                             chord(E, dominant_seventh)});
  for (std::size_t i = 0; i < lane.size(); ++i) {
    for (const auto& list : {substitutions(lane, i, a_minor), hints(lane, i, a_minor)}) {
      for (const auto& e : list) {
        check_valid(e.lane, e.title);
        require(e.lane.front().position == lane.front().position &&
                    span_end(e.lane) == span_end(lane),
                e.title + " keeps the lane's span");
        REQUIRE_LT(e.selected, e.lane.size(), e.title + " selects a region");
        require(!e.preview.empty(), e.title + " has a preview");
      }
    }
    REQUIRE_LE(hints(lane, i, a_minor).size(), hint_limit, "at most four hints");
  }
  const auto has = [](const std::vector<LaneEdit>& list, EditKind k) {
    return std::ranges::any_of(list, [&](const LaneEdit& e) { return e.kind == k; });
  };
  const auto at_f = hints(lane, 2, a_minor);
  require(has(at_f, EditKind::passing_chord), "Dm9 -> Fmaj9 jumps a third: a passing chord");
  require(!has(at_f, EditKind::borrowed), "VI is home: nothing borrowed");
  for (const auto& h : at_f) {
    require(!h.why.empty(), h.title + " says why");
  }
  const auto at_e = hints(lane, 3, a_minor);
  require(has(at_e, EditKind::suspension), "the V can be suspended");
  require(!has(at_e, EditKind::pull_home), "E7 already pulls home");
  const auto at_d = hints(lane, 1, a_minor);
  require(has(at_d, EditKind::pedal_bass), "a pedal under iv");
  require(has(at_d, EditKind::borrowed), "iv can be borrowed (D9)");
  const auto subs_e = substitutions(lane, 3, a_minor);
  require(has(subs_e, EditKind::tritone), "a dominant has a tritone sub");
  const auto tritone = std::ranges::find(subs_e, EditKind::tritone, &LaneEdit::kind);
  REQUIRE_EQ(tritone->lane[3].chord, chord(b_flat, dominant_seventh), "E7's tritone sub is B♭7");
  const auto lead_in = std::ranges::find(subs_e, EditKind::lead_in_dominant, &LaneEdit::kind);
  REQUIRE_NE(lead_in, subs_e.end(), "a lead-in dominant goes before the chord");
  REQUIRE_EQ(lead_in->lane[3].chord, chord(B, dominant_seventh),
             "a lead-in dominant goes before the chord");
  REQUIRE_EQ(lead_in->lane[4].chord, lane[3].chord, "a lead-in dominant goes before the chord");
  REQUIRE_GE(substitutions(lane, 0, a_minor).size(), 3, "the first chord has substitutions");
}

void tile_and_replace_cut_at_the_edges() {
  const auto prog = lane_of({chord(A, minor_ninth), chord(F, major_ninth)});
  const auto tiled = tile(prog, {bar}, {bar + 3 * bar + 2 * beat});
  REQUIRE_EQ(tiled.size(), 4, "two repeats of two chords");
  REQUIRE_EQ(tiled.front().position.value, bar, "starts at the start");
  REQUIRE_EQ(tiled.back().length_ticks, 2 * beat, "the last is cut at the end");
  check_valid(tiled, "tile");
  require(tile(prog, {bar}, {bar}).empty(), "an empty range");

  const auto lane = lane_of({chord(A, minor), chord(D, minor), chord(E, minor)});
  const auto fill = lane_of({chord(C, major)}, 2 * bar);
  // Replace from the middle of the first chord to the middle of the third.
  auto placed = fill;
  placed[0].position = {bar / 2};
  const auto out = replace_range(lane, {bar / 2}, {2 * bar + bar / 2}, placed);
  check_valid(out, "replace");
  REQUIRE_EQ(out.size(), 3, "head, the new chord, tail");
  REQUIRE_EQ(out[0].chord, chord(A, minor), "the head is kept");
  REQUIRE_EQ(out[0].length_ticks, bar / 2, "the head is kept");
  REQUIRE_EQ(out[1].chord, chord(C, major), "the new chord fills the range");
  REQUIRE_EQ(out[1].position.value, bar / 2, "the new chord fills the range");
  REQUIRE_EQ(out[1].length_ticks, 2 * bar, "the new chord fills the range");
  REQUIRE_EQ(out[2].chord, chord(E, minor), "the tail is kept");
  REQUIRE_EQ(out[2].position.value, 2 * bar + bar / 2, "the tail is kept");
  REQUIRE_EQ(out[2].length_ticks, bar / 2, "the tail is kept");
  // A region spanning the whole range splits in two.
  const auto long_lane = lane_of({chord(A, minor)}, 4 * bar);
  const auto split = replace_range(long_lane, {bar}, {2 * bar}, lane_of({chord(D, minor)}));
  REQUIRE_EQ(split.size(), 2,
             "a spanning region keeps both ends (the new chord lies outside the range)");
  REQUIRE_EQ(split[0].length_ticks, bar,
             "a spanning region keeps both ends (the new chord lies outside the range)");
  REQUIRE_EQ(split[1].position.value, 2 * bar,
             "a spanning region keeps both ends (the new chord lies outside the range)");
}

// The modes from theory, from the tonic: Phrygian to Lydian, as the pad
// runs dark to bright.
constexpr std::array<std::array<int, 7>, 6> theory_modes{{{0, 1, 3, 5, 7, 8, 10},
                                                          {0, 2, 3, 5, 7, 8, 10},
                                                          {0, 2, 3, 5, 7, 9, 10},
                                                          {0, 2, 4, 5, 7, 9, 10},
                                                          {0, 2, 4, 5, 7, 9, 11},
                                                          {0, 2, 4, 6, 7, 9, 11}}};

// Colour stays in the mode: beyond the triad (which a major V or a borrowed
// chord may raise), every tone a Lab chord adds is the mode's own, so
// Phrygian's i is never Am(add9) with a B♮ and Aeolian's v never Em(add9)
// with an F♯.
void lab_colour_stays_in_the_mode() {
  int chords = 0;
  for (const double brightness : {0.05, 0.25, 0.45, 0.65, 0.85, 0.95})
    for (const double restlessness : {0.1, 0.4, 0.9})
      for (std::size_t sh = 0; sh < shape_count; ++sh)
        for (const auto colour : {Colour::natural, Colour::triads, Colour::add9, Colour::sus2,
                                  Colour::sevenths, Colour::lush})
          for (std::uint64_t seed = 0; seed < 3; ++seed) {
            auto o = lab(brightness, static_cast<Shape>(sh), colour, restlessness);
            o.seed = seed;
            const auto mode = static_cast<std::size_t>(mode_at(brightness, std::nullopt));
            const auto all = suggest(o);
            for (const auto& suggestion : all) {
              if (suggestion.variant == Variant::borrow)
                continue;
              for (const auto& r : suggestion.chords) {
                const auto iv = intervals(r.chord.quality);
                for (std::uint8_t i = 3; i < iv.count; ++i) {
                  const int from_tonic = (r.chord.root + iv.semitones[i] - A + 120) % 12;
                  REQUIRE_NE(std::ranges::find(theory_modes[mode], from_tonic),
                             theory_modes[mode].end(),
                             names(suggestion.chords, a_minor) + " in " +
                                 std::string(mode_name(static_cast<Mode>(mode))) + ": " +
                                 name(r.chord, a_minor) + " adds a tone outside the mode");
                }
                ++chords;
              }
            }
          }
  REQUIRE_GT(chords, 1000, "chords checked");
}

// Natural colours as the records do (PML's Iconic Chord Progressions: 63 %
// plain triads, the colour by degree): most chords plain, one or two a loop
// coloured, never two neighbours alike in colour, each colour one the
// records use on that degree, the same seed the same colours and another
// seed sometimes others.
void natural_colour_is_mostly_plain() {
  int chords = 0, plain = 0, differ = 0;
  for (const double brightness : {0.1, 0.3, 0.5, 0.7, 0.9})
    for (std::size_t sh = 1; sh < shape_count; ++sh)
      for (std::uint64_t seed = 0; seed < 6; ++seed) {
        auto o = lab(brightness, static_cast<Shape>(sh), Colour::natural);
        o.seed = seed;
        const auto lane = suggest(o)[0].chords;
        REQUIRE_EQ(lane, suggest(o)[0].chords, "the same seed, the same colours");
        auto next = o;
        next.seed = seed + 1;
        differ += lane == suggest(next)[0].chords ? 0 : 1;
        int coloured = 0;
        for (std::size_t i = 0; i < lane.size(); ++i) {
          const auto q = lane[i].chord.quality;
          const bool is_plain = q == major || q == minor;
          coloured += is_plain ? 0 : 1;
          plain += is_plain ? 1 : 0;
          ++chords;
          require(is_plain || q == sus2 || q == sus4 || q == minor_seventh || q == major_seventh ||
                      q == sixth || q == add9 || q == seventh_sus4 || q == dominant_seventh,
                  names(lane, a_minor) + ": a colour the records use");
          if (i > 0 && !is_plain)
            REQUIRE_NE(q, lane[i - 1].chord.quality,
                       names(lane, a_minor) + ": neighbours differ in colour");
        }
        if (lane.size() >= 2)
          REQUIRE_GE(coloured, 1, names(lane, a_minor) + ": a coloured chord in the loop");
        // A restless major V may add one beyond the half.
        REQUIRE_LE(coloured, static_cast<int>((lane.size() + 1) / 2) + 1,
                   names(lane, a_minor) + ": no more than half coloured");
      }
  REQUIRE_GE(plain * 2, chords,
             "most chords plain: " + std::to_string(plain) + " of " + std::to_string(chords));
  REQUIRE_GT(differ, 0, "another seed gives other colours");
}

} // namespace

// Every voicing sounds clean, whatever the chord, root, style, register or
// drone: no neighbouring semitones, no minor ninths (but in a 7♭9), wide
// intervals low down, inside the range, and the chord's third and seventh
// (or the tones standing for them) always present.
void every_voicing_is_clean() {
  constexpr int semitone = 1;
  constexpr int minor_ninth_span = 13;
  constexpr int fifth_floor = 48;
  constexpr int third_floor = 53;
  constexpr int drone_headroom = 3;
  for (std::uint8_t q = 1; q <= harmony::last_chord_quality; ++q) {
    const auto quality = static_cast<harmony::ChordQuality>(q);
    const auto iv = intervals(quality);
    for (std::uint8_t root = 0; root < pitch_classes; ++root) {
      for (const auto style : {VoicingStyle::close, VoicingStyle::drop2, VoicingStyle::spread,
                               VoicingStyle::rootless}) {
        for (const int reg : {-7, 0, 7}) {
          for (const std::optional<std::uint8_t> drone :
               {std::optional<std::uint8_t>{}, std::optional<std::uint8_t>{7}}) {
            VoicingOptions o;
            o.style = style;
            o.register_offset = reg;
            const auto c = chord(root, quality);
            const auto v = voice(c, drone, o, nullptr);
            const auto what = std::string(name(c, {0, KeyMode::major})) + " style " +
                              std::to_string(static_cast<int>(style)) + " reg " +
                              std::to_string(reg) + (drone ? " drone" : "");
            REQUIRE_GE(v.count, 3, what + ": at least three voices");
            require(v.notes[0] >= o.lowest && v.notes[v.count - 1] <= o.highest + drone_headroom,
                    what + ": inside the range");
            for (std::uint8_t i = 1; i < v.count; ++i) {
              const int low = v.notes[i - 1], gap = v.notes[i] - low;
              REQUIRE_NE(gap, semitone, what + ": no neighbouring semitone");
              require(low >= fifth_floor || gap >= 7, what + ": a fifth apart below C3");
              require(low >= third_floor || gap >= 3, what + ": a third apart below F3");
            }
            if (quality != dominant_seventh_flat_ninth)
              for (std::uint8_t i = 0; i < v.count; ++i)
                for (std::uint8_t j = i + 1; j < v.count; ++j)
                  require(!((v.notes[j] - v.notes[i]) % 12 == semitone &&
                            v.notes[j] - v.notes[i] >= minor_ninth_span),
                          what + ": no minor ninth");
            std::set<int> pcs;
            for (std::uint8_t i = 0; i < v.count; ++i)
              pcs.insert(v.notes[i] % 12);
            // The quality's second tone (its third, the sus tone, or a power
            // chord's fifth) and, for four-note chords, its third tone above
            // the fifth (the seventh, sixth or added tone) must sound.
            require(pcs.contains((root + iv.semitones[1]) % 12), what + ": its third sounds");
            if (iv.count >= 4 && quality != minor_eleventh && quality != dominant_thirteenth)
              require(pcs.contains((root + iv.semitones[3]) % 12), what + ": its seventh sounds");
          }
        }
      }
    }
  }
  // The Lab's default loop, voiced for Comp: Am9 is G3 C4 E4 B4 over the
  // A bass, not a cluster.
  VoicingOptions o;
  const auto am9 = voice(chord(A, minor_ninth), std::nullopt, o, nullptr);
  REQUIRE_EQ(bass_note(chord(A, minor_ninth), comping_bass_lowest), 45, "the bass is A2");
  REQUIRE_EQ(am9.count, 4, "Am9 voices four notes over the bass");
}

// Open sits as the records do (PML's Iconic Chord Progressions, measured;
// ADR-0084): over the bass, octaves and fifths low down, the first voice the
// bass's octave or fifth; thirds and colour a ninth or more over the bass and
// never under E3; seconds only from C4 up and a semitone never; three to five
// voices, every chord tone sounding where the range allows.
void open_voicing_sits_like_the_records() {
  const auto o = open_voicing();
  for (std::uint8_t q = 1; q <= harmony::last_chord_quality; ++q) {
    const auto quality = static_cast<harmony::ChordQuality>(q);
    for (std::uint8_t root = 0; root < pitch_classes; ++root) {
      const auto c = chord(root, quality);
      const auto v = voice(c, std::nullopt, o, nullptr);
      const int bass = bass_note(c, o.bass_lowest);
      const auto what = std::string(name(c, {0, KeyMode::major})) + " open";
      require(v.count >= 3 && v.count <= 5, what + ": three to five voices");
      constexpr int colour_headroom = 3;
      require(v.notes[0] >= o.lowest && v.notes[v.count - 1] <= o.highest + colour_headroom,
              what + ": in melodic house's register");
      const int first = v.notes[0] - bass;
      require(first >= 7 && first <= 19,
              what + ": the first voice a fifth to a twelfth over the bass");
      for (std::uint8_t i = 0; i < v.count; ++i) {
        const int over_root = (v.notes[i] - root + 120) % 12;
        const bool root_or_fifth = over_root == 0 || (over_root >= 6 && over_root <= 8);
        const bool seventh = over_root == 10 || over_root == 11;
        require(root_or_fifth || (v.notes[i] >= 52 && v.notes[i] - bass >= (seventh ? 10 : 14)),
                what + ": thirds and colour high, clear of the bass");
        if (i == 0)
          continue;
        const int gap = v.notes[i] - v.notes[i - 1];
        require(gap != 1 || quality == dominant_seventh_flat_ninth ||
                    quality == dominant_thirteenth,
                what + ": no semitone between neighbours");
        require(gap != 2 || v.notes[i - 1] >= 60, what + ": seconds only from C4");
        require(v.notes[i - 1] >= 52 || gap >= 5, what + ": a fourth or more apart low down");
      }
    }
  }
  // Am: its fifth or doubled root over the bass, its third high.
  const auto am = voice(chord(A, minor), std::nullopt, o, nullptr);
  require(am.notes[0] % 12 == E || am.notes[0] % 12 == A,
          "Am open starts on its fifth or a doubled root");
  bool third = false;
  for (std::uint8_t i = 0; i < am.count; ++i)
    third = third || (am.notes[i] % 12 == C && am.notes[i] >= 57);
  require(third, "with its third high");
  // Cm9: every tone, its ninth clear of its third (D over E♭, not under it).
  const auto cm9 = voice(chord(C, minor_ninth), std::nullopt, o, nullptr);
  std::uint16_t sounding = 0;
  for (std::uint8_t i = 0; i < cm9.count; ++i)
    sounding = static_cast<std::uint16_t>(sounding | (1U << (cm9.notes[i] % 12)));
  REQUIRE_EQ((sounding & ((1U << 3) | (1U << 10) | (1U << 2))),
             ((1U << 3) | (1U << 10) | (1U << 2)), "Cm9 sounds its third, seventh and ninth");
  REQUIRE_LT(bass_note(chord(A, minor), o.bass_lowest), o.lowest,
             "the bass sounds under the chord");
}

// The seven shapes added on 2026-11-02, in A Aeolian with plain triads, and
// Sway and Hope in Dorian, whose iv turns major.
void the_added_shapes_write_their_degrees() {
  const auto chords_of = [](Shape shape, double brightness = 0.3) {
    return suggest(lab(brightness, shape, Colour::triads))[0].chords;
  };
  check_chords(chords_of(Shape::pendulum), {chord(F, major), chord(G, major)}, "Pendulum VI VII");
  check_chords(chords_of(Shape::shuffle),
               {chord(A, minor), chord(G, major), chord(F, major), chord(G, major)},
               "Shuffle i VII VI VII");
  check_chords(chords_of(Shape::sway), {chord(A, minor), chord(D, minor)}, "Sway i iv");
  check_chords(chords_of(Shape::sway, 0.45), {chord(A, minor), chord(D, major)},
               "Dorian Sway i IV");
  check_chords(chords_of(Shape::hope),
               {chord(A, minor), chord(C, major), chord(E, minor), chord(D, minor)},
               "Hope i III v iv");
  check_chords(chords_of(Shape::hope, 0.45),
               {chord(A, minor), chord(C, major), chord(E, minor), chord(D, major)},
               "Dorian Hope i III v IV");
  check_chords(chords_of(Shape::odd_loop), {chord(A, minor), chord(F, major), chord(G, major)},
               "Odd loop i VI VII");
  REQUIRE_EQ(span_end(chords_of(Shape::odd_loop)), 3 * bar, "Odd loop is three bars");
  check_chords(chords_of(Shape::long_phrase),
               {chord(A, minor), chord(F, major), chord(C, major), chord(G, major), chord(A, minor),
                chord(F, major), chord(D, minor), chord(E, minor)},
               "Long phrase");
  check_chords(chords_of(Shape::rise_and_fall),
               {chord(D, minor), chord(F, major), chord(G, major), chord(A, minor), chord(F, major),
                chord(E, minor), chord(D, minor), chord(A, minor)},
               "Rise and fall");
  REQUIRE_EQ(shape_degrees(Shape::long_phrase).size(), max_shape_chords,
             "the longest shapes fill the table");
}

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "harmony passed",
      {
          {"intervals_match_the_table", intervals_match_the_table},
          {"names_and_roman_numerals", names_and_roman_numerals},
          {"tension_orders_by_pull", tension_orders_by_pull},
          {"voicing_moves_least_and_stays_in_range", voicing_moves_least_and_stays_in_range},
          {"drop2_is_close_with_second_from_top_dropped",
           drop2_is_close_with_second_from_top_dropped},
          {"drone_sits_on_top", drone_sits_on_top},
          {"voicing_is_deterministic_and_callback_safe",
           voicing_is_deterministic_and_callback_safe},
          {"every_voicing_is_clean", every_voicing_is_clean},
          {"lab_loop_in_aeolian_is_the_minor_four", lab_loop_in_aeolian_is_the_minor_four},
          {"lab_modes_follow_brightness", lab_modes_follow_brightness},
          {"lab_restlessness_shapes_the_dominant", lab_restlessness_shapes_the_dominant},
          {"lab_devices_do_what_they_say", lab_devices_do_what_they_say},
          {"lab_is_deterministic_and_variants_differ", lab_is_deterministic_and_variants_differ},
          {"next_after_the_dominant_is_home", next_after_the_dominant_is_home},
          {"substitutions_and_hints_make_valid_lanes", substitutions_and_hints_make_valid_lanes},
          {"tile_and_replace_cut_at_the_edges", tile_and_replace_cut_at_the_edges},
          {"open_voicing_sits_like_the_records", open_voicing_sits_like_the_records},
          {"the_added_shapes_write_their_degrees", the_added_shapes_write_their_degrees},
          {"lab_colour_stays_in_the_mode", lab_colour_stays_in_the_mode},
          {"natural_colour_is_mostly_plain", natural_colour_is_mostly_plain},
      });
}
