#include "oscillate/native_dsp/synth/fret_feel.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// Fret's Feel (ADR-0088): realtime auto-articulation from what is known when
// a note starts. The oracle is the reference model's output on the
// original two-phrase line drawn flat at velocity 100 on a clean grid,
// humanize off: every style's velocities and articulations must match it.
namespace {

using namespace oscillate::native_dsp::synth::fret::feel;

using oscillate::test::require;

constexpr double beats_per_bar = 4.0;
constexpr int drawn_velocity = 100;
constexpr double prototype_dynamics = 20.0;

// MIDI keys of the line (sounding).
constexpr int e1 = 28;
constexpr int g1 = 31;
constexpr int a1 = 33;
constexpr int b1 = 35;
constexpr int d2 = 38;
constexpr int e2 = 40;

struct Drawn {
  double beat;
  int key;
  double length;
};

// The original line's first phrase, then its second with the walk-up fill.
constexpr std::array<Drawn, 35> line{{
    {0.00, e1, 0.40},  {0.50, e1, 0.15},  {0.75, e2, 0.20},  {1.25, d2, 0.15},  {1.50, b1, 0.45},
    {2.50, e1, 0.20},  {2.75, e1, 0.15},  {3.00, g1, 0.50},  {3.75, a1, 0.20},  {4.25, e1, 0.30},
    {4.75, e1, 0.15},  {5.00, e2, 0.15},  {5.50, d2, 0.40},  {6.25, b1, 0.20},  {6.50, a1, 0.15},
    {6.75, g1, 0.60},  {8.00, e1, 0.40},  {8.50, e1, 0.15},  {8.75, e2, 0.20},  {9.25, d2, 0.15},
    {9.50, b1, 0.45},  {10.50, e1, 0.20}, {10.75, e1, 0.15}, {11.00, g1, 0.50}, {11.75, a1, 0.20},
    {12.25, e1, 0.30}, {12.75, e1, 0.15}, {13.00, e2, 0.15}, {13.50, d2, 0.40}, {14.25, b1, 0.20},
    {14.50, a1, 0.15}, {15.00, g1, 0.22}, {15.25, a1, 0.22}, {15.50, b1, 0.22}, {15.75, d2, 0.22},
}};

constexpr auto P = Articulation::pluck;
constexpr auto S = Articulation::staccato;
constexpr auto G = Articulation::ghost;
struct Expected {
  int velocity;
  Articulation articulation;
};
using Oracle = std::array<Expected, line.size()>;

// The reference model's output for each style (centre 92, Dynamics 20, ghost velocity
// 44, ghosts on, humanize 0).
constexpr Oracle funk{{{117, P}, {44, G}, {82, S},  {72, S}, {91, P},  {93, S}, {44, G},
                       {102, P}, {72, S}, {92, P},  {44, G}, {106, P}, {88, P}, {72, S},
                       {91, S},  {98, P}, {117, P}, {44, G}, {82, S},  {72, S}, {91, P},
                       {93, S},  {44, G}, {102, P}, {72, S}, {92, P},  {44, G}, {106, P},
                       {88, P},  {72, S}, {91, S},  {96, P}, {75, S},  {94, S}, {81, S}}};
constexpr Oracle driving{{{117, P}, {44, G},  {90, S},  {80, S},  {103, P}, {105, S}, {44, G},
                          {110, P}, {80, S},  {100, P}, {44, G},  {114, P}, {100, P}, {80, S},
                          {103, S}, {106, P}, {117, P}, {44, G},  {90, S},  {80, S},  {103, P},
                          {105, S}, {44, G},  {110, P}, {80, S},  {100, P}, {44, G},  {114, P},
                          {100, P}, {80, S},  {103, S}, {104, P}, {83, S},  {106, S}, {89, S}}};
constexpr Oracle laid_back{{{109, P}, {44, G},  {94, S},  {84, S}, {95, P},  {97, S}, {44, G},
                            {102, P}, {84, S},  {104, P}, {44, G}, {106, P}, {92, P}, {84, S},
                            {95, S},  {110, P}, {109, P}, {44, G}, {94, S},  {84, S}, {95, P},
                            {97, S},  {44, G},  {102, P}, {84, S}, {104, P}, {44, G}, {106, P},
                            {92, P},  {84, S},  {95, S},  {96, P}, {87, S},  {98, S}, {93, S}}};

Note note_at(double beat, int key, double length, int velocity = drawn_velocity) {
  const auto bar = static_cast<std::int64_t>(std::floor(beat / beats_per_bar));
  return {beat, beat - static_cast<double>(bar) * beats_per_bar, bar, length, key, velocity};
}

std::vector<Decision> play(const Settings& settings, double length_scale = 1.0) {
  std::vector<Decision> out;
  Previous previous;
  for (const auto& d : line) {
    const auto note = note_at(d.beat, d.key, d.length * length_scale);
    out.push_back(decide(note, previous, settings));
    previous = after(note, out.back());
  }
  return out;
}

void matches_the_prototype() {
  const std::array<std::pair<Style, const Oracle*>, 3> styles{
      {{Style::funk, &funk}, {Style::driving, &driving}, {Style::laid_back, &laid_back}}};
  for (const auto& [style, oracle] : styles) {
    const auto played = play({.style = style, .dynamics = prototype_dynamics});
    for (std::size_t i = 0; i < line.size(); ++i) {
      require(played[i].velocity == (*oracle)[i].velocity &&
                  played[i].articulation == (*oracle)[i].articulation,
              "note " + std::to_string(i) + " plays as the prototype did (style " +
                  std::to_string(static_cast<int>(style)) + "): got " +
                  std::to_string(played[i].velocity));
    }
  }
}

void places_on_the_grid() {
  REQUIRE_EQ(place_of(0.0), Place::one, "the one, beats, eighths and sixteenths");
  REQUIRE_EQ(place_of(1.0), Place::beat, "the one, beats, eighths and sixteenths");
  REQUIRE_EQ(place_of(0.5), Place::eighth, "the one, beats, eighths and sixteenths");
  REQUIRE_EQ(place_of(0.25), Place::sixteenth, "the one, beats, eighths and sixteenths");
  REQUIRE_EQ(place_of(0.75), Place::sixteenth, "the one, beats, eighths and sixteenths");
  REQUIRE_EQ(place_of(3.5), Place::eighth, "the one, beats, eighths and sixteenths");
  constexpr double within = 1.0 / 60.0;
  constexpr double off_grid = 0.125;
  REQUIRE_EQ(place_of(1.0 + within), Place::beat,
             "a hair off the grid is on it; a thirty-second is between");
  REQUIRE_EQ(place_of(off_grid), Place::between,
             "a hair off the grid is on it; a thirty-second is between");
}

void as_drawn_and_no_amount_keep_the_notes() {
  constexpr int own = 77;
  Previous previous;
  for (const auto style : {Style::as_drawn, Style::funk}) {
    const double amount = style == Style::as_drawn ? 1.0 : 0.0;
    const auto d =
        decide(note_at(0.75, e1, 0.1, own), previous, {.style = style, .amount = amount});
    REQUIRE_EQ(d.velocity, own, "As drawn, or Amount 0, plays the note as written");
    REQUIRE_EQ(d.articulation, Articulation::pluck,
               "As drawn, or Amount 0, plays the note as written");
    REQUIRE_EQ(d.delay_ms, 0.0, "As drawn, or Amount 0, plays the note as written");
  }
}

void low_amount_blends_velocity_but_keeps_plucks() {
  constexpr double half_way_short = 0.4;
  const auto full = play({.style = Style::funk, .dynamics = prototype_dynamics});
  const auto part =
      play({.style = Style::funk, .amount = half_way_short, .dynamics = prototype_dynamics});
  for (std::size_t i = 0; i < line.size(); ++i) {
    const double expected = drawn_velocity + (full[i].velocity - drawn_velocity) * half_way_short;
    REQUIRE_EQ(part[i].articulation, Articulation::pluck, "below half Amount every note plucks");
    if (full[i].articulation == Articulation::pluck) {
      REQUIRE_NEAR(part[i].velocity, expected, 1.0, "Amount blends the velocity");
    }
  }
}

void live_notes_ghost_by_touch() {
  constexpr int soft = live_ghost_velocity;
  constexpr int firm = live_ghost_velocity + 1;
  const Settings funk_feel{.style = Style::funk};
  Previous previous = after(note_at(0.0, e1, 0.0), decide(note_at(0.0, e1, 0.0), {}, funk_feel));
  const auto soft_repeat = decide(note_at(0.5, e1, 0.0, soft), previous, funk_feel);
  const auto firm_repeat = decide(note_at(0.5, e1, 0.0, firm), previous, funk_feel);
  const auto other_key = decide(note_at(0.5, g1, 0.0, soft), previous, funk_feel);
  REQUIRE_EQ(soft_repeat.articulation, Articulation::ghost, "a soft off-beat repeat ghosts live");
  REQUIRE_EQ(firm_repeat.articulation, Articulation::pluck,
             "a firm repeat, or another key, plucks; live notes are never staccato");
  REQUIRE_EQ(other_key.articulation, Articulation::pluck,
             "a firm repeat, or another key, plucks; live notes are never staccato");
  const auto off =
      decide(note_at(0.5, e1, 0.0, soft), previous, {.style = Style::funk, .ghosts = false});
  REQUIRE_EQ(off.articulation, Articulation::pluck, "Ghost notes Off never ghosts");
}

void pocket_delays_only_off_beats() {
  constexpr double pocket = 12.0;
  const auto played = play({.style = Style::laid_back, .pocket_ms = pocket});
  for (std::size_t i = 0; i < line.size(); ++i) {
    const auto place = place_of(note_at(line[i].beat, line[i].key, line[i].length).bar_beat);
    const bool off = place == Place::eighth || place == Place::sixteenth;
    REQUIRE_EQ(played[i].delay_ms, (off ? pocket : 0.0), "off-beats sit back; beats stay put");
  }
  const auto clamped =
      decide(note_at(0.5, e1, 0.4), {}, {.style = Style::funk, .pocket_ms = 2 * max_pocket_ms});
  REQUIRE_EQ(clamped.delay_ms, max_pocket_ms, "Pocket stops at its maximum");
}

void humanize_is_seeded_bounded_and_loops() {
  const Settings plain{.style = Style::funk, .dynamics = prototype_dynamics};
  const Settings human{
      .style = Style::funk, .dynamics = prototype_dynamics, .humanize = max_humanize};
  const auto a = play(plain);
  const auto b = play(human);
  const auto again = play(human);
  bool varied = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    REQUIRE_EQ(b[i].velocity, again[i].velocity, "humanize is seeded");
    if (a[i].articulation != Articulation::ghost) {
      REQUIRE_NEAR(b[i].velocity, a[i].velocity, max_humanize + 1.0,
                   "humanize stays within its range");
      varied = varied || b[i].velocity != a[i].velocity;
    }
  }
  require(varied, "humanize varies the notes");
  // Two bars later the same note lands the same way.
  constexpr double cycle_beats = 2 * beats_per_bar;
  Previous previous;
  const auto first = decide(note_at(1.5, e1, 0.4), previous, human);
  const auto later = decide(note_at(1.5 + 2 * cycle_beats, e1, 0.4), previous, human);
  REQUIRE_EQ(first.velocity, later.velocity, "a loop repeats its groove");
}

void pushes_are_marked() {
  const auto played = play({.style = Style::funk, .dynamics = prototype_dynamics});
  // The E at 4.25, held 0.3 beats past 4.5, is pushed; the E at 0.0 is not.
  constexpr std::size_t pushed_note = 9;
  REQUIRE_TRUE(played[pushed_note].pushed, "an off-beat held over the eighth pushes");
  REQUIRE_TRUE(!played[0].pushed, "an off-beat held over the eighth pushes");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "fret feel contract passed",
      {
          {"matches_the_prototype", [] { matches_the_prototype(); }},
          {"places_on_the_grid", [] { places_on_the_grid(); }},
          {"as_drawn_and_no_amount_keep_the_notes",
           [] { as_drawn_and_no_amount_keep_the_notes(); }},
          {"low_amount_blends_velocity_but_keeps_plucks",
           [] { low_amount_blends_velocity_but_keeps_plucks(); }},
          {"live_notes_ghost_by_touch", [] { live_notes_ghost_by_touch(); }},
          {"pocket_delays_only_off_beats", [] { pocket_delays_only_off_beats(); }},
          {"humanize_is_seeded_bounded_and_loops", [] { humanize_is_seeded_bounded_and_loops(); }},
          {"pushes_are_marked", [] { pushes_are_marked(); }},
      });
}
