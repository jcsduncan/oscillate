#include "oscillate/native_dsp/synth/wren_breath.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Wren's Breath (ADR-0089): Feel for a flute, decided from what is known
// when a note starts. The oracle is the reference model's output on the
// audition line drawn flat at velocity 100 in D dorian: every note's
// velocity, vibrato, swell, grace and tongue must match it in both styles.
// The reference's t/k check always passed; Breath alternates for real (k
// only after a t), which plays the line the same.
namespace {

using namespace oscillate::native_dsp::synth::wren::breath;

using oscillate::test::require;

constexpr double beats_per_bar = 4.0;
constexpr int drawn_velocity = 100;
constexpr int semitones_per_octave = 12;
// Tolerances: decisions are computed exactly as the prototype computes
// them, so only floating-point noise separates them.
constexpr double exact = 1e-9;

// Keys of the line.
constexpr int d4 = 62;
constexpr int f4 = 65;
constexpr int g4 = 67;
constexpr int a4 = 69;
constexpr int c5 = 72;
constexpr int d5 = 74;
constexpr int e5 = 76;
constexpr int f5 = 77;
constexpr int g5 = 79;
constexpr int a5 = 81;
constexpr int c6 = 84;

// D dorian, as the prototype's scale.
constexpr int tonic_d = 2;
constexpr std::array dorian_steps{0, 2, 3, 5, 7, 9, 10};
Scale d_dorian() {
  Scale scale{tonic_d, 0};
  for (const int step : dorian_steps) {
    scale.pitch_classes = static_cast<std::uint16_t>(scale.pitch_classes | (1U << step));
  }
  return scale;
}

struct Drawn {
  double beat;
  int key;
  double length;
};

// The audition line (its velocities are ignored: drawn flat).
constexpr std::array<Drawn, 36> line{{
    {0.5, a4, 0.5},   {1.0, d5, 1.6},   {2.75, c5, 0.25},  {3.0, d5, 0.5},   {3.5, f5, 2.1},
    {5.5, e5, 0.6},   {6.0, d5, 2.0},   {8.5, a5, 0.25},   {8.75, g5, 0.25}, {9.0, f5, 0.55},
    {9.5, e5, 0.55},  {10.0, d5, 0.75}, {11.0, c5, 1.0},   {12.0, a4, 2.5},  {16.5, d5, 0.55},
    {17.0, f5, 0.55}, {17.5, a5, 1.0},  {18.75, c6, 0.25}, {19.0, a5, 1.5},  {20.75, g5, 0.25},
    {21.0, f5, 0.55}, {21.5, g5, 0.55}, {22.0, e5, 2.0},   {24.5, d5, 0.25}, {24.75, d5, 0.25},
    {25.0, f5, 0.25}, {25.5, a5, 0.5},  {26.0, g5, 0.75},  {27.0, e5, 0.5},  {27.5, c5, 0.55},
    {28.0, d5, 3.5},  {32.5, d4, 1.0},  {33.5, f4, 0.55},  {34.0, a4, 0.55}, {34.5, g4, 1.5},
    {36.0, d4, 3.5},
}};

// The prototype's per-style figures: vibrato depth and delay, swell and
// grace time, with the controls at their defaults.
struct StyleFigures {
  double vibrato;
  double delay_ms;
  double swell_db;
  double grace_ms;
};
constexpr StyleFigures lyrical_figures{1.0, 160.0, 3.0, 70.0};
constexpr StyleFigures airy_figures{0.55, 350.0, 1.5, 35.0};

constexpr bool V = true;   // Vibrato.
constexpr bool S = true;   // Swell.
constexpr bool no = false; // Neither.
constexpr auto t = Tongue::t;
constexpr auto k = Tongue::k;
constexpr auto slur = Tongue::slur;

struct Expected {
  int velocity;
  bool vibrato;
  bool swell;
  int grace_step; // 0 for none.
  Tongue tongue;
};
using Oracle = std::array<Expected, line.size()>;

// The reference model's decisions, note by note.
constexpr Oracle lyrical_oracle{{
    {86, no, no, 0, t},   {98, V, S, 0, t},      {80, no, no, 0, t},    {92, no, no, 0, t},
    {93, V, S, 0, t},     {87, no, no, 0, slur}, {98, V, S, 0, slur},   {92, no, no, 0, t},
    {83, no, no, 0, t},   {94, no, no, 0, t},    {87, no, no, 0, slur}, {92, V, no, 0, slur},
    {97, V, no, 0, t},    {103, V, S, 2, t},     {89, no, no, 0, t},    {94, no, no, 0, slur},
    {95, V, no, 0, slur}, {86, no, no, 0, t},    {101, V, S, 2, t},     {83, no, no, 0, t},
    {94, no, no, 0, t},   {88, no, no, 0, slur}, {99, V, S, 0, slur},   {89, no, no, 0, t},
    {81, no, no, 0, k},   {94, no, no, 0, t},    {89, no, no, 0, t},    {94, V, no, 0, t},
    {93, no, no, 0, t},   {85, no, no, 0, t},    {106, V, S, 0, slur},  {88, V, no, 0, t},
    {81, no, no, 0, t},   {90, no, no, 0, slur}, {88, V, S, 0, slur},   {100, V, S, 2, t},
}};
constexpr Oracle airy_oracle{{
    {76, no, no, 0, t},    {84, V, no, 0, t},     {70, no, no, 0, t},    {82, no, no, 0, t},
    {79, V, S, 0, t},      {76, no, no, 0, slur}, {84, V, S, 2, slur},   {82, no, no, 0, t},
    {74, no, no, 0, t},    {83, no, no, 0, t},    {76, no, no, 0, slur}, {82, no, no, 0, slur},
    {83, no, no, 2, t},    {92, V, S, 2, t},      {78, no, no, 0, t},    {83, no, no, 0, slur},
    {81, no, no, 0, slur}, {76, no, no, 0, t},    {88, V, no, 2, t},     {74, no, no, 0, t},
    {83, no, no, 0, t},    {77, no, no, 0, slur}, {85, V, S, 1, slur},   {78, no, no, 0, t},
    {71, no, no, 0, k},    {83, no, no, 0, t},    {78, no, no, 0, t},    {84, no, no, 0, t},
    {82, no, no, 0, t},    {74, no, no, 0, t},    {95, V, S, 0, slur},   {75, no, no, 0, t},
    {70, no, no, 0, t},    {79, no, no, 0, slur}, {74, V, no, 0, slur},  {89, V, S, 2, t},
}};

Context context_at(double beat, int key, double length, int velocity = drawn_velocity,
                   std::optional<Scale> scale = d_dorian()) {
  Context c;
  const double bar = std::floor(beat / beats_per_bar);
  c.beat = beat;
  c.bar_beat = beat - bar * beats_per_bar;
  c.bar = static_cast<std::int64_t>(bar);
  c.beats_per_bar = beats_per_bar;
  c.length_beats = length;
  c.key = key;
  c.velocity = velocity;
  c.scale = scale;
  return c;
}

std::vector<Decision> play(const Settings& settings, double length_scale = 1.0) {
  std::vector<Decision> out;
  Previous previous;
  for (const auto& n : line) {
    const auto note = context_at(n.beat, n.key, n.length * length_scale);
    out.push_back(decide(note, previous, settings));
    previous = after(note, out.back());
  }
  return out;
}

void check_against(const Oracle& oracle, const StyleFigures& figures, const Settings& settings) {
  const Style style = settings.style;
  const auto played = play(settings);
  for (std::size_t i = 0; i < line.size(); ++i) {
    const auto& want = oracle[i];
    const auto& got = played[i];
    const std::string at =
        "note " + std::to_string(i) + " (style " + std::to_string(static_cast<int>(style)) + ")";
    REQUIRE_EQ(got.velocity, want.velocity,
               at + ": velocity " + std::to_string(got.velocity) + ", the prototype " +
                   std::to_string(want.velocity));
    require(std::fabs(got.vibrato - (want.vibrato ? figures.vibrato : 0.0)) < exact &&
                std::fabs(got.vibrato_delay_ms - (want.vibrato ? figures.delay_ms : 0.0)) < exact,
            at + ": vibrato");
    REQUIRE_LT(std::fabs(got.swell_db - (want.swell ? figures.swell_db : 0.0)), exact,
               at + ": swell");
    require(got.grace == (want.grace_step != 0) && got.grace_step == want.grace_step &&
                std::fabs(got.grace_ms - (want.grace_step != 0 ? figures.grace_ms : 0.0)) < exact,
            at + ": grace " + std::to_string(got.grace) + " step " +
                std::to_string(got.grace_step) + " ms " + std::to_string(got.grace_ms));
    REQUIRE_EQ(got.tongue, want.tongue, at + ": tongue");
    REQUIRE_EQ(got.staccato, (line[i].length < staccato_beats), at + ": short notes are staccato");
  }
}

void matches_the_prototype() {
  // The prototype's Lyrical took a grace at Some's chance, its Airy at
  // Lots' (the Lyrical Air and Folk Breeze presets).
  check_against(lyrical_oracle, lyrical_figures,
                {.style = Style::lyrical, .ornaments = Ornaments::some});
  check_against(airy_oracle, airy_figures, {.style = Style::airy, .ornaments = Ornaments::lots});
}

void places_on_the_grid() {
  constexpr double within = 1.0 / 60.0;
  constexpr double off_grid = 0.125;
  REQUIRE_EQ(place_of(0.0, beats_per_bar), Place::one,
             "the one, beats, eighths; the rest weigh as sixteenths");
  REQUIRE_EQ(place_of(1.0, beats_per_bar), Place::beat,
             "the one, beats, eighths; the rest weigh as sixteenths");
  REQUIRE_EQ(place_of(0.5, beats_per_bar), Place::eighth,
             "the one, beats, eighths; the rest weigh as sixteenths");
  REQUIRE_EQ(place_of(0.25, beats_per_bar), Place::sixteenth,
             "the one, beats, eighths; the rest weigh as sixteenths");
  REQUIRE_EQ(place_of(1.0 + within, beats_per_bar), Place::beat,
             "the one, beats, eighths; the rest weigh as sixteenths");
  REQUIRE_EQ(place_of(off_grid, beats_per_bar), Place::sixteenth,
             "the one, beats, eighths; the rest weigh as sixteenths");
  constexpr double three_four = 3.0;
  REQUIRE_EQ(place_of(three_four - within, three_four), Place::one,
             "a hair before the next bar is its one");
}

void as_drawn_and_no_amount_keep_the_notes() {
  constexpr int own = 77;
  for (const auto& settings : {Settings{.style = Style::as_drawn}, Settings{.amount = 0.0}}) {
    Previous previous;
    for (const auto& n : line) {
      const auto note = context_at(n.beat, n.key, n.length, own);
      const auto d = decide(note, previous, settings);
      REQUIRE_EQ(d.velocity, own, "As drawn, or Amount 0, plays the notes as written");
      REQUIRE_EQ(d.vibrato, 0.0, "As drawn, or Amount 0, plays the notes as written");
      REQUIRE_EQ(d.swell_db, 0.0, "As drawn, or Amount 0, plays the notes as written");
      REQUIRE_TRUE(!d.grace, "As drawn, or Amount 0, plays the notes as written");
      REQUIRE_EQ(d.cents, 0.0, "As drawn, or Amount 0, plays the notes as written");
      REQUIRE_NE(d.tongue, Tongue::k, "As drawn, or Amount 0, plays the notes as written");
      previous = after(note, d);
    }
  }
}

void amount_scales_breath() {
  constexpr double half = 0.5;
  const auto full = play({});
  const auto part = play({.amount = half});
  std::size_t full_graces = 0;
  std::size_t part_graces = 0;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const double expected = drawn_velocity + (full[i].velocity - drawn_velocity) * half;
    REQUIRE_LE(std::fabs(part[i].velocity - expected), 1.0, "Amount blends the velocity");
    REQUIRE_LT(std::fabs(part[i].vibrato - full[i].vibrato * half), exact,
               "Amount scales vibrato and swell");
    REQUIRE_LT(std::fabs(part[i].swell_db - full[i].swell_db * half), exact,
               "Amount scales vibrato and swell");
    require(!part[i].grace || full[i].grace, "a lower Amount never adds a grace");
    full_graces += full[i].grace ? 1U : 0U;
    part_graces += part[i].grace ? 1U : 0U;
  }
  REQUIRE_LT(part_graces, full_graces, "and halves the graces' chance");
}

void live_notes_breathe_without_their_length() {
  constexpr double live = 0.0;
  const Settings lyrical{};
  const auto first = context_at(0.0, d5, live);
  const auto d = decide(first, {}, lyrical);
  const auto drawn = decide(context_at(0.0, d5, line[1].length), {}, lyrical);
  REQUIRE_EQ(d.vibrato, lyrical_figures.vibrato,
             "a live note gets vibrato, blooming if it is held");
  REQUIRE_EQ(d.vibrato_delay_ms, lyrical_figures.delay_ms,
             "a live note gets vibrato, blooming if it is held");
  REQUIRE_EQ(d.swell_db, 0.0, "but no swell, grace or staccato");
  REQUIRE_TRUE(!d.grace, "but no swell, grace or staccato");
  REQUIRE_TRUE(!d.staccato, "but no swell, grace or staccato");
  REQUIRE_LT(d.velocity, drawn.velocity, "and no long-note accent");
  // Still held when the next starts: a slur; released first: tongued.
  auto previous = after(first, d);
  require(!previous.ended, "a live note's end is unknown until its note-off");
  REQUIRE_EQ(decide(context_at(1.0, e5, live), previous, lyrical).tongue, Tongue::slur,
             "a note over a held live note slurs");
  previous.ended = true;
  previous.end_beat = previous.beat + line[0].length;
  REQUIRE_EQ(decide(context_at(1.0, e5, live), previous, lyrical).tongue, Tongue::t,
             "after its note-off the next is tongued");
}

void fast_repeats_alternate_t_and_k() {
  constexpr double sixteenth = 0.25;
  constexpr int repeats = 4;
  Previous previous;
  std::vector<Tongue> tongues;
  for (int i = 0; i < repeats; ++i) {
    const auto note = context_at(sixteenth * i, d5, sixteenth);
    const auto d = decide(note, previous, {});
    tongues.push_back(d.tongue);
    previous = after(note, d);
  }
  REQUIRE_EQ(tongues, (std::vector<Tongue>{Tongue::t, Tongue::k, Tongue::t, Tongue::k}),
             "t k t k: a k only after a t");
  const auto slow = decide(
      context_at(1.0, d5, sixteenth),
      after(context_at(0.0, d5, sixteenth), decide(context_at(0.0, d5, sixteenth), {}, {})), {});
  REQUIRE_EQ(slow.tongue, Tongue::t, "repeats a beat apart are both t");
  const auto other = decide(
      context_at(sixteenth, e5, sixteenth),
      after(context_at(0.0, d5, sixteenth), decide(context_at(0.0, d5, sixteenth), {}, {})), {});
  REQUIRE_EQ(other.tongue, Tongue::t, "a different key is t");
}

void articulation_sets_how_notes_join() {
  constexpr double eighth = 0.5;
  constexpr double gap = 0.2; // Under a sixteenth.
  const auto first = context_at(0.0, d5, eighth - gap);
  const auto second = context_at(eighth, e5, eighth);
  const auto overlapping = context_at(0.0, d5, eighth + gap);
  const auto previous = after(first, decide(first, {}, {}));
  const auto held = after(overlapping, decide(overlapping, {}, {}));
  const auto join = [&](Articulation a, const Previous& p) {
    return decide(second, p, {.articulation = a});
  };
  REQUIRE_EQ(join(Articulation::automatic, previous).tongue, Tongue::t, "Auto slurs overlaps only");
  REQUIRE_EQ(join(Articulation::automatic, held).tongue, Tongue::slur, "Auto slurs overlaps only");
  REQUIRE_EQ(join(Articulation::legato, previous).tongue, Tongue::slur,
             "Legato also slurs across a gap up to a sixteenth");
  constexpr double wide_gap = 0.3;
  const auto far = context_at(0.0, d5, eighth - wide_gap);
  REQUIRE_EQ(join(Articulation::legato, after(far, decide(far, {}, {}))).tongue, Tongue::t,
             "but not a wider one");
  REQUIRE_EQ(join(Articulation::tongued, held).tongue, Tongue::t, "Tongued never slurs");
  const auto staccato = join(Articulation::staccato, held);
  REQUIRE_TRUE(staccato.staccato, "Staccato plays every note short");
  REQUIRE_EQ(staccato.tongue, Tongue::t, "Staccato plays every note short");
  require(!join(Articulation::automatic, held).staccato, "Auto keeps longer notes long");
}

void graces_follow_the_project_key() {
  constexpr int d_tonic = 2;
  const auto d_major =
      scale_of(oscillate::project::MusicalKey{d_tonic, oscillate::project::KeyMode::major});
  const auto d_minor =
      scale_of(oscillate::project::MusicalKey{d_tonic, oscillate::project::KeyMode::minor});
  require(!scale_of(std::nullopt).has_value(), "no key, no scale");
  constexpr int whole_tone = 2;
  constexpr int semitone = 1;
  REQUIRE_EQ(step_above(a4, d_major), whole_tone, "A to B in D major, A to B-flat in D minor");
  REQUIRE_EQ(step_above(a4, d_minor), semitone, "A to B in D major, A to B-flat in D minor");
  REQUIRE_EQ(step_above(a4, std::nullopt), whole_tone, "a whole tone without a key");
  REQUIRE_EQ(step_above(e5, d_dorian()), semitone, "E to F in D dorian; C to D in D minor");
  REQUIRE_EQ(step_above(c5 + semitones_per_octave, d_minor), whole_tone,
             "E to F in D dorian; C to D in D minor");
  // The line's graced A4 takes its step from the key it is given.
  constexpr std::size_t graced = 13;
  const auto& n = line[graced];
  const auto& before = line[graced - 1];
  const auto previous = after(context_at(before.beat, before.key, before.length), {});
  const auto in_minor =
      decide(context_at(n.beat, n.key, n.length, drawn_velocity, d_minor), previous, {});
  const auto in_major =
      decide(context_at(n.beat, n.key, n.length, drawn_velocity, d_major), previous, {});
  REQUIRE_TRUE(in_minor.grace, "the grace is the key's step above");
  REQUIRE_EQ(in_minor.grace_step, semitone, "the grace is the key's step above");
  REQUIRE_TRUE(in_major.grace, "the grace is the key's step above");
  REQUIRE_EQ(in_major.grace_step, whole_tone, "the grace is the key's step above");
  require(
      !decide(context_at(n.beat, n.key, n.length), previous, {.ornaments = Ornaments::off}).grace,
      "Ornaments Off plays none");
}

void humanize_is_seeded_bounded_and_loops() {
  const Settings plain{};
  const Settings human{.humanize = max_humanize};
  const auto a = play(plain);
  const auto b = play(human);
  const auto again = play(human);
  bool varied = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    REQUIRE_EQ(b[i].velocity, again[i].velocity, "humanize is seeded");
    REQUIRE_EQ(b[i].cents, again[i].cents, "humanize is seeded");
    REQUIRE_NEAR(b[i].velocity, a[i].velocity, max_humanize + 1.0,
                 "humanize stays within its range");
    REQUIRE_LE(std::fabs(b[i].cents), max_humanize_cents, "humanize stays within its range");
    varied = varied || b[i].velocity != a[i].velocity || b[i].cents != 0.0;
    REQUIRE_EQ(a[i].cents, 0.0, "no detune without Humanize");
  }
  require(varied, "humanize varies the notes");
  constexpr double cycle_beats = 2 * beats_per_bar;
  constexpr double beat = 1.5;
  constexpr double length = 0.5;
  const auto first = decide(context_at(beat, d5, length), {}, human);
  const auto later = decide(context_at(beat + 2 * cycle_beats, d5, length), {}, human);
  REQUIRE_EQ(first.velocity, later.velocity, "a loop repeats its breath");
  REQUIRE_EQ(first.cents, later.cents, "a loop repeats its breath");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "wren breath contract passed",
      {
          {"matches_the_prototype", [] { matches_the_prototype(); }},
          {"places_on_the_grid", [] { places_on_the_grid(); }},
          {"as_drawn_and_no_amount_keep_the_notes",
           [] { as_drawn_and_no_amount_keep_the_notes(); }},
          {"amount_scales_breath", [] { amount_scales_breath(); }},
          {"live_notes_breathe_without_their_length",
           [] { live_notes_breathe_without_their_length(); }},
          {"fast_repeats_alternate_t_and_k", [] { fast_repeats_alternate_t_and_k(); }},
          {"articulation_sets_how_notes_join", [] { articulation_sets_how_notes_join(); }},
          {"graces_follow_the_project_key", [] { graces_follow_the_project_key(); }},
          {"humanize_is_seeded_bounded_and_loops", [] { humanize_is_seeded_bounded_and_loops(); }},
      });
}
