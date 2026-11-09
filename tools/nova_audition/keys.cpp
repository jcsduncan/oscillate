#include "keys.hpp"

#include "audition.hpp"
#include "midi_out.hpp"
#include "parts.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/export/wav_writer.hpp"
#include "oscillate/project/clip_types.hpp"
#include "oscillate/timeline/timing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oscillate::tools::keys {
namespace {

namespace fs = std::filesystem;
using audition::done;
using audition::fail;
using audition::take;
using parts::beats_per_bar;
using parts::key;
using parts::Note;
using parts::ticks;
using P = parts::Pitch;
using Session = application::ProjectSession;

constexpr std::uint32_t render_rate = 48'000;
constexpr double tempo_bpm = 120.0;
// The pads' parts last twelve bars; the breakdown loops them to sixteen.
constexpr int bars = 12;
constexpr double part_beats = bars * beats_per_bar;
constexpr int breakdown_bars = 16;
constexpr double breakdown_beats = breakdown_bars * beats_per_bar;
constexpr std::string_view concert = "concert";
constexpr std::string_view expanse = parts::space_engine;

// ---- The pads' harmony -------------------------------------------------------------------
// Where the pads' chords change (parts::held_part, parts::low_part): C minor,
// A♭, the open G of the Hijaz, C minor with a suspended F, low C, and C
// minor again where the loop comes round.
enum class Region : std::uint8_t { c_minor, a_flat, g, c_minor_sus, low_c, c_minor_again };
constexpr std::array<double, 7> region_starts{0.0,   17.5,       25.25,          31.0,
                                              40.75, part_beats, breakdown_beats};

Region region_at(double beat) {
  for (std::size_t i = 1; i < region_starts.size(); ++i)
    if (beat < region_starts[i])
      return static_cast<Region>(i - 1);
  return Region::c_minor_again;
}

// ---- Pedals ------------------------------------------------------------------------------

struct PedalPoint {
  double beat;
  double value; // 0 lifts, 1 is full.
};

// Legato pedalling with the pads' chords: down just after each change, up
// just before the next, so one chord's ring never smears into the next.
constexpr double pedal_down_after = 0.25;
constexpr double pedal_up_before = 0.1;
std::vector<PedalPoint> sustain_by_region(double depth, double beats) {
  std::vector<PedalPoint> points;
  for (std::size_t i = 0; i + 1 < region_starts.size() && region_starts[i] < beats; ++i) {
    points.push_back({region_starts[i] + pedal_down_after, depth});
    points.push_back({std::min(region_starts[i + 1], beats) - pedal_up_before, 0.0});
  }
  return points;
}

// Una corda for the whole part.
std::vector<PedalPoint> soft_throughout() { return {{0.0, 1.0}}; }

// ---- The parts (beats from bar 1) ---------------------------------------------------------
// Everything sits above the Two Flutes' top E♭5, except the low octaves of
// part 4, and every velocity is soft.

// Single notes with long gaps, each a colour of the chord under it: the
// fifth, the third, the ninth; B natural over the Hijaz G.
std::vector<Note> sparse_singles() {
  return {{3.25, 1.5, key(P::g, 5), 46},   {6.75, 2.0, key(P::eb, 6), 40},
          {11.5, 1.0, key(P::d, 6), 52},   {13.25, 3.0, key(P::c, 6), 44},
          {19.75, 2.0, key(P::eb, 6), 50}, {22.5, 2.5, key(P::c, 6), 38},
          {27.0, 1.5, key(P::b, 5), 48},   {28.75, 2.0, key(P::d, 6), 42},
          {33.5, 1.5, key(P::g, 5), 54},   {35.25, 2.0, key(P::f, 5), 40},
          {38.0, 2.0, key(P::eb, 6), 46},  {43.5, 3.0, key(P::c, 6), 42},
          {46.0, 2.0, key(P::g, 5), 34}};
}

// Two-note intervals every five and a half beats, starting off the beat,
// so each lands somewhere new in the bar; the upper note sometimes a
// breath late, and the one before the close left out.
std::vector<Note> offbeat_intervals() {
  struct Dyad {
    std::uint8_t low;
    std::uint8_t high;
  };
  const auto dyad = [](Region region, bool second) -> Dyad {
    switch (region) {
    case Region::a_flat:
      return second ? Dyad{key(P::c, 6), key(P::eb, 6)} : Dyad{key(P::ab, 5), key(P::c, 6)};
    case Region::g:
      return second ? Dyad{key(P::g, 5), key(P::d, 6)} : Dyad{key(P::b, 5), key(P::d, 6)};
    case Region::low_c:
      return {key(P::g, 5), key(P::c, 6)};
    case Region::c_minor:
    case Region::c_minor_sus:
    case Region::c_minor_again:
      break;
    }
    return second ? Dyad{key(P::c, 6), key(P::eb, 6)} : Dyad{key(P::g, 5), key(P::eb, 6)};
  };
  constexpr double first = 1.5;
  constexpr double every = 5.5;
  constexpr double length = 1.25;
  constexpr double late = 0.06;
  constexpr double skipped = 40.0;
  constexpr std::array<std::uint8_t, 5> velocities{48, 40, 56, 44, 36};
  std::vector<Note> notes;
  std::size_t index = 0;
  for (double at = first; at < part_beats; at += every, ++index) {
    if (at == skipped)
      continue;
    const auto pair = dyad(region_at(at), index % 2 == 1);
    const auto velocity = velocities[index % velocities.size()];
    const double upper = index % 3 == 2 ? at + late : at;
    notes.push_back({at, length, pair.low, velocity});
    notes.push_back({upper, length, pair.high, static_cast<std::uint8_t>(velocity - 6)});
  }
  return notes;
}

// A five-note phrase seven beats long, against the pads' four-beat bars and
// longer phrases, so it falls differently each time; it bends to the
// chord under it (B and D over the Hijaz G), fades, and drops its last
// note every third time.
std::vector<Note> seven_beat_phrase() {
  struct Step {
    double at;
    double length;
    std::uint8_t key;
  };
  const std::array<Step, 5> phrase{{{0.0, 1.25, key(P::g, 5)},
                                    {1.5, 0.75, key(P::eb, 6)},
                                    {2.25, 1.0, key(P::d, 6)},
                                    {3.5, 1.5, key(P::c, 6)},
                                    {5.0, 1.75, key(P::g, 5)}}};
  const auto fit = [](std::uint8_t k, Region region) -> std::uint8_t {
    if (region == Region::a_flat && k == key(P::d, 6))
      return key(P::eb, 6);
    if (region == Region::g && k == key(P::eb, 6))
      return key(P::d, 6);
    if (region == Region::g && k == key(P::c, 6))
      return key(P::b, 5);
    return k;
  };
  constexpr double first = 2.0;
  constexpr double every = 7.0;
  constexpr std::uint8_t loudest = 58;
  constexpr std::uint8_t fade_per_repeat = 3;
  constexpr std::array<std::uint8_t, 5> shape{0, 8, 4, 2, 12};
  std::vector<Note> notes;
  int repeat = 0;
  for (double start = first; start < part_beats; start += every, ++repeat) {
    for (std::size_t i = 0; i < phrase.size(); ++i) {
      if (repeat % 3 == 2 && i + 1 == phrase.size())
        continue;
      const double at = start + phrase[i].at;
      if (at >= part_beats)
        break;
      const double length = std::min(phrase[i].length, part_beats - at);
      const auto velocity =
          static_cast<std::uint8_t>(loudest - repeat * fade_per_repeat - shape[i]);
      notes.push_back({at, length, fit(phrase[i].key, region_at(at)), velocity});
    }
  }
  return notes;
}

// Soft low octaves on the roots, a little after each chord change (a fifth
// where the octave would fall off the keyboard), under a high line that
// touches a note now and then.
std::vector<Note> low_octaves_high_line() {
  constexpr double held = 6.0;
  return {
      {0.75, held, key(P::c, 1), 34},   {0.75, held, key(P::c, 2), 30},
      {18.25, held, key(P::ab, 1), 32}, {18.25, held, key(P::eb, 2), 26},
      {26.0, 4.5, key(P::g, 1), 30},    {26.0, 4.5, key(P::d, 2), 24},
      {31.75, held, key(P::c, 1), 34},  {31.75, held, key(P::c, 2), 30},
      {41.5, held, key(P::c, 1), 30},   {41.5, held, key(P::c, 2), 26},
      {5.5, 1.5, key(P::g, 6), 42},     {9.0, 1.0, key(P::f, 6), 36},
      {10.5, 2.5, key(P::eb, 6), 40},   {14.75, 1.5, key(P::d, 6), 34},
      {21.0, 1.5, key(P::c, 7), 38},    {22.25, 2.0, key(P::g, 6), 44},
      {28.5, 1.0, key(P::b, 6), 40},    {29.75, 1.5, key(P::g, 6), 34},
      {36.5, 2.0, key(P::eb, 6), 42},   {38.25, 1.0, key(P::d, 6), 32},
      {45.0, 2.5, key(P::c, 7), 36},
  };
}

// A short falling answer a few beats after each pad phrase enters, never
// the same rhythm twice.
std::vector<Note> felt_answers() {
  return {{4.5, 0.75, key(P::eb, 6), 50}, {5.25, 1.25, key(P::d, 6), 42},
          {6.5, 2.0, key(P::c, 6), 38},   {13.0, 0.5, key(P::g, 5), 44},
          {13.5, 2.5, key(P::c, 6), 52},  {21.25, 1.25, key(P::c, 6), 46},
          {22.5, 2.0, key(P::g, 5), 36},  {28.5, 0.5, key(P::d, 6), 48},
          {29.0, 1.25, key(P::b, 5), 42}, {30.25, 1.5, key(P::ab, 5), 36},
          {35.0, 0.75, key(P::f, 6), 46}, {35.75, 2.5, key(P::eb, 6), 40},
          {44.0, 1.5, key(P::g, 5), 38},  {45.5, 2.5, key(P::c, 6), 44}};
}

// A two-note cell every three beats from the off-beat, drifting against
// the bar, silent in bars 4, 8 and 12; from bar 7 it falls instead of
// rising and sits a sixteenth later.
std::vector<Note> three_beat_cell() {
  constexpr double first = 0.5;
  constexpr double every = 3.0;
  constexpr double gap = 0.75;
  constexpr double length = 1.0;
  constexpr double turn = 24.0;
  constexpr double later = 0.25;
  constexpr int quiet_bar = 4;
  constexpr std::array<std::uint8_t, 4> velocities{44, 36, 50, 40};
  std::vector<Note> notes;
  std::size_t index = 0;
  for (double at = first; at < part_beats; at += every, ++index) {
    const double start = at >= turn ? at + later : at;
    const int bar = static_cast<int>(start / beats_per_bar) + 1;
    if (bar % quiet_bar == 0)
      continue;
    const auto region = region_at(start);
    const auto high = region == Region::a_flat ? key(P::c, 6) : key(P::d, 6);
    const auto low = key(P::g, 5);
    const auto velocity = velocities[index % velocities.size()];
    const auto first_key = start >= turn ? high : low;
    const auto second_key = start >= turn ? low : high;
    notes.push_back({start, length, first_key, velocity});
    notes.push_back({start + gap, length, second_key, static_cast<std::uint8_t>(velocity - 8)});
  }
  return notes;
}

// ---- Clips -------------------------------------------------------------------------------

struct Clip {
  std::string_view name;   // "<NN idea>"
  std::string_view preset; // Concert.
  std::string_view space;  // Expanse.
  double space_mix;
  // Make-up gain in dB, so the soft, una corda playing sits a few dB over
  // the pads: Trim before the reverb, then the track level.
  double gain_db;
  std::vector<Note> (*part)();
  double sustain; // Pedal depth by chord; 0 leaves the pedal up.
  bool una_corda;
  double beats = part_beats;
};

const std::vector<Clip>& clips() {
  static const std::vector<Clip> list{
      {"01 sparse single notes", "Felt", "Endless Cloud", 0.45, 25.0, sparse_singles, 1.0, true},
      {"02 off-beat intervals", "Soft", "Ambient Wash", 0.5, 29.0, offbeat_intervals, 0.6, true},
      {"03 seven-beat phrase", "Concert Grand", "Cathedral", 0.4, 19.5, seven_beat_phrase, 0.7,
       false},
      {"04 low octaves, high line", "Close", "Concert Hall", 0.4, 20.5, low_octaves_high_line, 1.0,
       true},
      {"05 felt answers", "Felt", "Dark Hall", 0.5, 25.5, felt_answers, 0.8, true},
      {"06 three-beat cell", "Dry", "Endless Cloud", 0.55, 20.0, three_beat_cell, 0.5, false},
  };
  return list;
}

// ---- The breakdown round (sixteen bars) ---------------------------------------------------
// Each part opens sparse, finds its peak around bars 10 to 12 and thins out
// in the last bars so it can dissolve into the reverb fade. Chords are two
// or three quiet notes, spread from the bottom up, placed off the melody so
// they answer it, holding common tones across the changes.

// Each note of a voicing a little after the one under it, a little softer.
constexpr double spread_step = 0.08;
constexpr std::uint8_t spread_fade = 3;
void spread(std::vector<Note>& notes, double at, std::initializer_list<std::uint8_t> keys,
            std::uint8_t velocity, double length) {
  double offset = 0.0;
  auto v = velocity;
  for (const auto k : keys) {
    notes.push_back({at + offset, length - offset, k, v});
    offset += spread_step;
    v = static_cast<std::uint8_t>(v - spread_fade);
  }
}

// 1. A line that climbs by leaps over two octaves to C7 in bar 10, then
// steps down; rolled chords between its phrases.
std::vector<Note> rising_line() {
  std::vector<Note> notes{
      {2.5, 2.0, key(P::g, 5), 42},    {7.0, 1.5, key(P::c, 6), 40},
      {12.75, 2.5, key(P::d, 6), 46},  {18.5, 1.5, key(P::eb, 6), 48},
      {20.25, 2.5, key(P::g, 6), 46},  {23.5, 1.0, key(P::f, 6), 40},
      {26.0, 1.0, key(P::d, 6), 48},   {27.25, 2.25, key(P::b, 5), 46},
      {29.5, 1.0, key(P::d, 6), 42},   {32.5, 1.0, key(P::g, 6), 52},
      {33.75, 1.5, key(P::f, 6), 48},  {36.5, 2.5, key(P::c, 7), 60},
      {39.25, 1.25, key(P::g, 6), 52}, {41.5, 1.5, key(P::eb, 6), 54},
      {43.25, 2.0, key(P::d, 6), 48},  {45.5, 2.0, key(P::c, 6), 44},
      {49.0, 2.5, key(P::g, 5), 40},   {53.5, 1.5, key(P::eb, 6), 36},
      {58.0, 4.0, key(P::d, 6), 32},
  };
  spread(notes, 9.25, {key(P::c, 4), key(P::g, 4), key(P::d, 5)}, 36, 6.0);
  spread(notes, 19.25, {key(P::ab, 3), key(P::g, 4), key(P::c, 5)}, 38, 5.5);
  spread(notes, 28.25, {key(P::g, 3), key(P::d, 4), key(P::b, 4)}, 36, 2.5);
  spread(notes, 34.75, {key(P::c, 4), key(P::f, 4), key(P::g, 4)}, 40, 5.5);
  spread(notes, 42.25, {key(P::c, 4), key(P::g, 4)}, 38, 5.0);
  spread(notes, 50.5, {key(P::eb, 4), key(P::d, 5)}, 30, 8.0);
  return notes;
}

// 2. A short high call, then a low tenth answering a beat or two later:
// the root and the third (or the Hijaz B) an octave and more above it.
std::vector<Note> call_and_response() {
  std::vector<Note> notes{
      {1.5, 0.75, key(P::g, 5), 42},   {2.25, 2.0, key(P::eb, 6), 46},
      {9.0, 0.75, key(P::d, 6), 44},   {9.75, 0.5, key(P::eb, 6), 40},
      {10.25, 2.5, key(P::g, 6), 48},  {18.0, 1.0, key(P::c, 6), 46},
      {19.0, 2.25, key(P::eb, 6), 50}, {23.0, 0.75, key(P::g, 6), 48},
      {23.75, 1.25, key(P::f, 6), 42}, {27.5, 0.5, key(P::d, 6), 46},
      {28.0, 0.5, key(P::eb, 6), 42},  {28.5, 2.0, key(P::b, 5), 48},
      {33.0, 0.75, key(P::g, 6), 52},  {33.75, 0.75, key(P::ab, 6), 54},
      {34.5, 3.0, key(P::c, 7), 62},   {39.5, 1.0, key(P::ab, 6), 52},
      {40.5, 2.0, key(P::g, 6), 48},   {45.0, 1.0, key(P::eb, 6), 44},
      {46.0, 2.5, key(P::d, 6), 40},   {53.0, 3.0, key(P::g, 5), 34},
  };
  spread(notes, 5.0, {key(P::c, 3), key(P::eb, 4)}, 38, 3.5);
  spread(notes, 13.5, {key(P::c, 3), key(P::eb, 4)}, 32, 3.5);
  spread(notes, 21.75, {key(P::ab, 2), key(P::c, 4)}, 38, 3.0);
  spread(notes, 25.75, {key(P::g, 2), key(P::b, 3)}, 40, 4.5);
  spread(notes, 31.5, {key(P::c, 3), key(P::f, 4)}, 40, 2.5);
  spread(notes, 38.0, {key(P::c, 3), key(P::f, 4)}, 44, 2.5);
  spread(notes, 43.25, {key(P::c, 3), key(P::g, 4)}, 38, 4.5);
  spread(notes, 49.5, {key(P::c, 3), key(P::eb, 4)}, 34, 3.5);
  spread(notes, 57.5, {key(P::c, 3), key(P::eb, 4)}, 26, 6.0);
  return notes;
}

// 3. The sparse single notes grown into a melody: the same opening, then
// longer phrases with a third under a note here and there, the peak on C7,
// and two notes to close.
std::vector<Note> singles_grown() {
  return {
      {3.25, 1.5, key(P::g, 5), 44},  {6.75, 2.0, key(P::eb, 6), 40},
      {11.5, 1.0, key(P::d, 6), 50},  {13.25, 3.0, key(P::c, 6), 42},
      {18.25, 1.0, key(P::c, 6), 46}, {19.25, 1.0, key(P::eb, 6), 48},
      {20.5, 2.5, key(P::g, 6), 50},  {20.58, 2.4, key(P::eb, 6), 42},
      {23.5, 1.5, key(P::f, 6), 42},  {26.25, 0.75, key(P::d, 6), 48},
      {27.0, 2.0, key(P::b, 5), 44},  {27.08, 1.9, key(P::d, 6), 46},
      {29.75, 1.0, key(P::g, 5), 40}, {32.0, 1.0, key(P::c, 6), 50},
      {33.0, 1.0, key(P::d, 6), 52},  {34.0, 1.0, key(P::eb, 6), 54},
      {35.25, 2.5, key(P::g, 6), 58}, {35.33, 2.4, key(P::eb, 6), 48},
      {38.0, 1.5, key(P::c, 7), 62},  {39.75, 1.0, key(P::g, 6), 52},
      {41.5, 2.0, key(P::eb, 6), 50}, {41.58, 1.9, key(P::c, 6), 44},
      {44.0, 1.5, key(P::d, 6), 44},  {46.0, 2.0, key(P::c, 6), 40},
      {50.0, 2.5, key(P::g, 5), 36},  {55.5, 3.0, key(P::eb, 6), 30},
      {55.58, 2.9, key(P::c, 6), 26},
  };
}

// 4. The three-beat cell grown: from bar 5 it takes a third note that climbs
// to C7 by bar 11; from bar 7 it sits a sixteenth late; from bar 13 it
// falls and plays only every other time. Quiet voicings under it on each
// change, off the cell. Bars 4, 8, 12 and 16 stay silent.
std::vector<Note> cell_with_harmony() {
  constexpr double first = 0.5;
  constexpr double every = 3.0;
  constexpr double gap = 0.75;
  constexpr double length = 1.0;
  constexpr double third_from = 16.0;
  constexpr double late_from = 24.0;
  constexpr double fall_from = 48.0;
  constexpr double later = 0.25;
  constexpr int quiet_bar = 4;
  constexpr std::array<std::uint8_t, 4> velocities{44, 36, 50, 40};
  std::vector<Note> notes;
  std::size_t index = 0;
  for (double at = first; at < breakdown_beats; at += every, ++index) {
    const double start = at >= late_from ? at + later : at;
    const int bar = static_cast<int>(start / beats_per_bar) + 1;
    if (bar % quiet_bar == 0 || (start >= fall_from && index % 2 == 1))
      continue;
    const auto region = region_at(start);
    std::uint8_t low = key(P::g, 5);
    std::uint8_t high = key(P::d, 6);
    std::uint8_t top = key(P::g, 6);
    switch (region) {
    case Region::a_flat:
      high = key(P::c, 6);
      top = key(P::eb, 6);
      break;
    case Region::g:
      low = key(P::b, 5);
      top = key(P::g, 6);
      break;
    case Region::c_minor_sus:
      top = start >= 36.0 ? key(P::c, 7) : key(P::f, 6);
      break;
    case Region::low_c:
      high = key(P::c, 6);
      top = key(P::eb, 6);
      break;
    case Region::c_minor:
    case Region::c_minor_again:
      break;
    }
    const bool peak = region == Region::c_minor_sus;
    const auto velocity =
        static_cast<std::uint8_t>(velocities[index % velocities.size()] + (peak ? 8 : 0));
    const bool falling = start >= fall_from;
    notes.push_back({start, length, falling ? high : low, velocity});
    notes.push_back(
        {start + gap, length, falling ? low : high, static_cast<std::uint8_t>(velocity - 8)});
    if (start >= third_from && start < fall_from)
      notes.push_back({start + 2 * gap, length, top, static_cast<std::uint8_t>(velocity - 4)});
  }
  spread(notes, 2.25, {key(P::c, 4), key(P::g, 4)}, 32, 6.0);
  spread(notes, 18.75, {key(P::ab, 3), key(P::eb, 4), key(P::g, 4)}, 34, 6.0);
  spread(notes, 26.75, {key(P::g, 3), key(P::b, 3), key(P::d, 4)}, 34, 4.0);
  spread(notes, 32.75, {key(P::c, 4), key(P::f, 4), key(P::g, 4)}, 38, 7.5);
  spread(notes, 42.25, {key(P::c, 4), key(P::g, 4)}, 34, 5.5);
  spread(notes, 50.75, {key(P::c, 4), key(P::d, 5)}, 28, 9.0);
  return notes;
}

// 5. Three voices moving slowly, the top rising while the lowest falls to
// the peak in bar 10, then settling; each voice moves at its own moment
// rather than together, and the lowest drops out for the close.
std::vector<Note> chorale() {
  struct Chord {
    double at;
    std::uint8_t top;
    std::uint8_t middle;
    std::optional<std::uint8_t> low;
    std::uint8_t velocity;
  };
  const std::array<Chord, 11> chords{{
      {2.0, key(P::g, 5), key(P::eb, 5), key(P::c, 5), 40},
      {9.5, key(P::d, 6), key(P::eb, 5), key(P::b, 4), 42},
      {18.0, key(P::eb, 6), key(P::g, 5), key(P::ab, 4), 44},
      {22.5, key(P::f, 6), key(P::c, 6), key(P::ab, 4), 42},
      {26.0, key(P::g, 6), key(P::b, 5), key(P::g, 4), 46},
      {32.0, key(P::ab, 6), key(P::c, 6), key(P::f, 4), 50},
      {36.5, key(P::c, 7), key(P::eb, 6), key(P::eb, 4), 58},
      {41.0, key(P::g, 6), key(P::d, 6), key(P::c, 4), 48},
      {45.0, key(P::eb, 6), key(P::g, 5), key(P::c, 4), 42},
      {50.0, key(P::d, 6), key(P::g, 5), key(P::eb, 4), 36},
      {56.0, key(P::c, 6), key(P::g, 5), std::nullopt, 30},
  }};
  // The voices' entries after the chord's moment, and the breath before
  // each voice's next one.
  constexpr double top_after = 0.0;
  constexpr double middle_after = 0.5;
  constexpr double low_after = 1.25;
  constexpr double breath = 0.15;
  constexpr double last_length = 6.0;
  std::vector<Note> notes;
  // A voice that keeps its note across a change holds: it sounds from the
  // chord that strikes it until the next chord that changes it.
  const auto voice = [&](auto pick, double after, int velocity_offset) {
    for (std::size_t i = 0; i < chords.size(); ++i) {
      const std::optional<std::uint8_t> k = pick(chords[i]);
      if (!k || (i > 0 && pick(chords[i - 1]) == k))
        continue;
      std::size_t next = i + 1;
      while (next < chords.size() && pick(chords[next]) == k)
        ++next;
      const double length = next == chords.size()
                                ? chords.back().at + last_length - chords[i].at - after
                                : chords[next].at - chords[i].at - breath;
      notes.push_back({chords[i].at + after, length, *k,
                       static_cast<std::uint8_t>(chords[i].velocity + velocity_offset)});
    }
  };
  constexpr int middle_softer = -8;
  constexpr int low_softer = -6;
  voice([](const Chord& c) { return std::optional<std::uint8_t>{c.top}; }, top_after, 0);
  voice([](const Chord& c) { return std::optional<std::uint8_t>{c.middle}; }, middle_after,
        middle_softer);
  voice([](const Chord& c) { return c.low; }, low_after, low_softer);
  return notes;
}

const std::vector<Clip>& breakdown_clips() {
  static const std::vector<Clip> list{
      {"01 rising line, rolled chords", "Felt", "Endless Cloud", 0.5, 14.8, rising_line, 1.0, true,
       breakdown_beats},
      {"02 call and response", "Concert Grand", "Ambient Wash", 0.55, 18.9, call_and_response, 0.8,
       false, breakdown_beats},
      {"03 single notes grown", "Felt", "Endless Cloud", 0.45, 25.3, singles_grown, 1.0, true,
       breakdown_beats},
      {"04 three-beat cell grown", "Dry", "Endless Cloud", 0.55, 17.9, cell_with_harmony, 0.6,
       false, breakdown_beats},
      {"05 three-voice chorale", "Soft", "Endless Cloud", 0.6, 26.9, chorale, 0.8, true,
       breakdown_beats},
  };
  return list;
}

std::vector<project::MidiPedalPoint> pedal_points(const std::vector<PedalPoint>& points) {
  std::vector<project::MidiPedalPoint> out;
  for (const auto& point : points)
    out.push_back({{ticks(point.beat)}, point.value});
  return out;
}

// The piano part alone with its pedals, as a MIDI file.
void write_midi(const fs::path& path, std::string_view name, const Clip& clip) {
  std::vector<midi_out::Control> controls;
  if (clip.sustain > 0.0)
    for (const auto& point : sustain_by_region(clip.sustain, clip.beats))
      controls.push_back({point.beat, midi_out::sustain_cc, point.value});
  if (clip.una_corda) {
    for (const auto& point : soft_throughout())
      controls.push_back({point.beat, midi_out::soft_cc, point.value});
    controls.push_back({clip.beats, midi_out::soft_cc, 0.0});
  }
  midi_out::write(path, name, tempo_bpm, clip.part(), controls);
}

constexpr std::string_view trim = "trim";
constexpr std::string_view trim_unity = "Unity";
constexpr double trim_max_db = 24.0;
constexpr double decibels_per_decade = 20.0;
constexpr double decade = 10.0;

void add_piano(Session& session, const Clip& clip) {
  const double trimmed = std::min(clip.gain_db, trim_max_db);
  const double level = std::pow(decade, (clip.gain_db - trimmed) / decibels_per_decade);
  const auto track = parts::add_track(session, concert, clip.preset, level);
  const auto space = parts::add_effect(session, track, expanse, clip.space);
  parts::set_effect_value(session, track, space, expanse, parts::space_mix_id, clip.space_mix);
  const auto gain = parts::add_effect(session, track, trim, trim_unity);
  parts::set_effect_value(session, track, gain, trim, "trim.gain", trimmed);
  const auto placed = parts::add_clip(session, track, 0.0, clip.part(), 0, clip.beats);
  if (clip.sustain > 0.0)
    done(session.pedals().set_midi_pedal_lane(
        placed, project::MidiPedal::sustain,
        pedal_points(sustain_by_region(clip.sustain, clip.beats))));
  if (clip.una_corda)
    done(session.pedals().set_midi_pedal_lane(placed, project::MidiPedal::soft,
                                              pedal_points(soft_throughout())));
}

} // namespace

void render_set(const fs::path& output, bool stems, Round round) {
  fs::create_directories(output);
  const auto work = output / ".projects";
  // Never clear a directory the tool did not make.
  if (!fs::create_directory(work))
    fail("scratch directory already exists: " + work.string());
  for (const auto& clip : round == Round::first ? clips() : breakdown_clips()) {
    const std::string label = std::string(clip.name) + " - " + std::string(clip.preset);
    std::cout << label << '\n';
    const auto root = work / label;
    auto session = take(Session::create(root));
    done(session->song().set_tempo_event({0}, tempo_bpm, timeline::TempoShape::step));
    parts::add_picked_pads(*session, clip.beats);
    add_piano(*session, clip);
    audition::render(*session, root, output / (label + ".wav"), render_rate,
                     exporting::WavFormat::float32);
    write_midi(output / (label + " (piano).mid"), label, clip);
    if (stems)
      audition::render_stems(*session, root, output, label, render_rate);
  }
  fs::remove_all(work);
}

} // namespace oscillate::tools::keys
