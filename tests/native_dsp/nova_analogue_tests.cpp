#include "oscillate/native_dsp/synth/nova.hpp"
#include "oscillate/native_dsp/synth/nova/analogue.hpp"
#include "oscillate/native_dsp/synth/nova/parameters.hpp"
#include "oscillate/native_inserts/instrument_binding.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "synth_test_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Nova Analogue (ADR-0073, contract 08a §2.12b, §3.7, §4): Off and Age 0 are
// exact, seeds are repeatable and distinct, any block split renders the
// same, a voice's pitch offset is the seeded (or painted) value, drift stays
// within its amount, the lanes validate strictly, the appended unison curves
// are deterministic and the voice sources route. Voice cards deal notes
// round-robin, re-roll moves each note repeatably, flutter stays within its
// maximum for any block split and the noise level follows an oscillator.
namespace {
// The smallest seeded offset the seed searches accept as audible, in cents.
constexpr double audible_cents = 2.0;
using namespace synth_test;
using namespace oscillate::native_inserts;
namespace nova = oscillate::native_dsp::synth::nova;

struct Patch {
  std::vector<double> values;
  project::NativeSynthDocument document;
};

Patch preset_patch(std::optional<std::size_t> preset) {
  auto patch = native_patch_for_preset(nova::engine(), preset);
  REQUIRE_OK(patch, "the preset makes a patch");
  auto resolved = resolve_native_instrument(patch.value());
  REQUIRE_OK(resolved, "the preset resolves");
  return {resolved.value().values, resolved.value().document};
}

std::unique_ptr<NativeSynthRuntime> runtime_for(const Patch& patch,
                                                double rate = oscillate::test::sample_rate_48k_hz) {
  auto runtime = NativeSynthRuntime::create(nova::engine(), patch.values, patch.document, {}, rate);
  REQUIRE_OK(runtime, "a Nova runtime is created");
  return std::move(runtime).value();
}

void set(Patch& patch, std::string_view id, double value) {
  patch.values[index(nova::descriptor(), id)] = value;
}

Rendered play(const Patch& patch, const std::vector<Event>& events, std::size_t frames,
              std::size_t block = 128) {
  auto runtime = runtime_for(patch);
  return render(*runtime, events, frames, block);
}

bool identical(const Rendered& a, const Rendered& b) {
  return a.left == b.left && a.right == b.right;
}

std::vector<Event> chord(std::uint64_t length) {
  std::vector<Event> events;
  std::uint64_t id = 1;
  for (std::uint8_t key :
       {std::uint8_t{48}, std::uint8_t{55}, std::uint8_t{60}, std::uint8_t{64}}) {
    auto n = note(0, length, key, id++);
    events.insert(events.end(), n.begin(), n.end());
  }
  return events;
}

// Two oscillators (one a unison stack) through the filter: every part
// Analogue touches is in use.
Patch busy_patch() {
  auto patch = preset_patch(std::nullopt);
  set(patch, "nova.osc.a.type", 0.0);
  set(patch, "nova.osc.a.shape", 2.0);
  set(patch, "nova.osc.a.unison.voices", 5.0);
  set(patch, "nova.osc.b.on", 1.0);
  set(patch, "nova.osc.b.type", 0.0);
  set(patch, "nova.osc.b.shape", 4.0);
  set(patch, "nova.filter.1.on", 1.0);
  set(patch, "nova.filter.1.cutoff", 1'500.0);
  set(patch, "nova.env.1.decay", 0.2);
  set(patch, "nova.env.1.sustain", 0.6);
  return patch;
}

Patch analogue(Patch patch) {
  set(patch, "nova.analogue.on", 1.0);
  set(patch, "nova.analogue.age", 0.8);
  set(patch, "nova.analogue.drift.amount", 0.7);
  set(patch, "nova.analogue.drift.rate", 1.0);
  return patch;
}

// A bare sine on oscillator A, with every other source of pitch movement
// off, so a voice's measured tuning is Analogue's alone.
Patch sine_patch() {
  auto patch = preset_patch(std::nullopt);
  set(patch, "nova.osc.a.type", 0.0);
  set(patch, "nova.osc.a.shape", 0.0);
  set(patch, "nova.osc.a.drift", 0.0);
  set(patch, "nova.global.drift", 0.0);
  set(patch, "nova.filter.1.on", 0.0);
  set(patch, "nova.osc.a.random_phase", 0.0);
  set(patch, "nova.analogue.on", 1.0);
  set(patch, "nova.analogue.scope", 1.0); // Notes only.
  set(patch, "nova.analogue.freerun", 0.0);
  set(patch, "nova.analogue.drift.amount", 0.0);
  set(patch, "nova.analogue.drift.cutoff", 0.0);
  return patch;
}

double measured_cents(const Patch& patch, std::size_t frames = 48'000) {
  const auto out = play(patch, note(0, frames, 69, 1), frames);
  const double hz = measured_hz(out.left, frames / 4, frames - 100, 48'000.0);
  return cents(hz, 440.0);
}

// The first note takes voice slot 0.
double lane_value(const Patch& patch, nova::AnalogueKind kind, nova::AnalogueLane lane,
                  std::size_t voice = 0) {
  const auto values =
      nova::analogue_lane_values(patch.values, patch.document, kind, 0, lane, voice + 1);
  REQUIRE_EQ(values.size(), voice + 1, "the lane has the voices asked for");
  return values[voice].value;
}

// ---- Cases ------------------------------------------------------------------------------

void off_ignores_every_analogue_setting() {
  const auto plain = busy_patch();
  auto off = busy_patch();
  set(off, "nova.analogue.age", 1.0);
  set(off, "nova.analogue.seed", 17.0);
  set(off, "nova.analogue.pitch", 1.0);
  set(off, "nova.analogue.drift.amount", 1.0);
  set(off, "nova.analogue.freerun", 1.0);
  set(off, "nova.analogue.cards", 5.0);
  set(off, "nova.analogue.reroll", 1.0);
  set(off, "nova.analogue.flutter.amount", 1.0);
  require(nova::set_analogue_lane_value(off.document, nova::AnalogueKind::notes, 0,
                                        nova::AnalogueLane::pitch, 0, 0.9),
          "a pitch lane is painted");
  require(identical(play(plain, chord(24'000), 36'000), play(off, chord(24'000), 36'000)),
          "with Analogue off, its settings and painted lanes change nothing");
}

void age_zero_without_drift_is_exact() {
  const auto plain = busy_patch();
  auto aged = busy_patch();
  set(aged, "nova.analogue.on", 1.0);
  set(aged, "nova.analogue.age", 0.0);
  set(aged, "nova.analogue.freerun", 0.0);
  set(aged, "nova.analogue.drift.amount", 0.0);
  set(aged, "nova.analogue.drift.cutoff", 0.0);
  require(identical(play(plain, chord(24'000), 36'000), play(aged, chord(24'000), 36'000)),
          "Age 0 with no drift and no free-running phase renders exactly as off");
}

void seeds_repeat_and_differ() {
  auto a = analogue(busy_patch());
  auto b = a;
  set(b, "nova.analogue.seed", 77.0);
  const auto first = play(a, chord(24'000), 36'000);
  require(identical(first, play(a, chord(24'000), 36'000)), "the same seed renders the same");
  require(!identical(first, play(b, chord(24'000), 36'000)), "another seed renders differently");
}

void any_block_split_renders_the_same() {
  auto patch = analogue(busy_patch());
  set(patch, "nova.analogue.drift.cutoff", 1.0);
  std::vector<Event> events = chord(18'000);
  auto late = note(9'001, 12'000, 67, 9);
  events.insert(events.end(), late.begin(), late.end());
  require(identical(play(patch, events, 36'000, 128), play(patch, events, 36'000, 0)),
          "steady and irregular blocks render the same audio");
}

void a_voice_is_tuned_by_its_seeded_offset() {
  auto patch = sine_patch();
  set(patch, "nova.analogue.age", 1.0);
  set(patch, "nova.analogue.pitch", 0.5);
  // A seed whose first voice is audibly off.
  double expected = 0.0;
  const auto last_seed = static_cast<int>(nova::analogue_seed_max);
  for (int seed = 1; seed <= last_seed && std::fabs(expected) < audible_cents; ++seed) {
    set(patch, "nova.analogue.seed", static_cast<double>(seed));
    expected = lane_value(patch, nova::AnalogueKind::notes, nova::AnalogueLane::pitch) *
               nova::analogue_pitch_cents;
  }
  REQUIRE_GE(std::fabs(expected), audible_cents, "some seed puts the first voice audibly off");
  REQUIRE_LE(std::fabs(expected), nova::analogue_pitch_cents, "within the part's maximum");
  REQUIRE_LT(std::fabs(measured_cents(patch) - expected), 0.2,
             "the note sounds at its seeded offset within 0.2 cent");
}

void a_painted_value_overrides_the_seed() {
  auto patch = sine_patch();
  set(patch, "nova.analogue.age", 1.0);
  require(nova::set_analogue_lane_value(patch.document, nova::AnalogueKind::notes, 0,
                                        nova::AnalogueLane::pitch, 0, 0.5),
          "voice 0's pitch is painted");
  const auto values = nova::analogue_lane_values(
      patch.values, patch.document, nova::AnalogueKind::notes, 0, nova::AnalogueLane::pitch, 2);
  REQUIRE_EQ(values.size(), 2, "the lane reports the painted voice");
  REQUIRE_TRUE(values[0].edited, "the lane reports the painted voice");
  REQUIRE_EQ(values[0].value, 0.5, "the lane reports the painted voice");
  REQUIRE_TRUE(!values[1].edited, "the lane reports the painted voice");
  REQUIRE_LT(std::fabs(measured_cents(patch) - 0.5 * nova::analogue_pitch_cents), 0.2,
             "the painted +6 cents is what plays");
  REQUIRE_TRUE(nova::set_analogue_lane_value(patch.document, nova::AnalogueKind::notes, 0,
                                             nova::AnalogueLane::pitch, 0, std::nullopt),
               "back to seed leaves no lane behind");
  REQUIRE_TRUE(patch.document.voice_lanes.empty(), "back to seed leaves no lane behind");
}

void drift_stays_within_its_amount() {
  auto patch = sine_patch();
  set(patch, "nova.analogue.age", 0.0);
  set(patch, "nova.analogue.drift.amount", 1.0);
  set(patch, "nova.analogue.drift.rate", 1.0);
  constexpr std::size_t frames = std::size_t{4} * 48'000;
  constexpr std::size_t window = 4'800;
  const auto out = play(patch, note(0, frames, 69, 1), frames);
  double lowest = 0.0, highest = 0.0;
  for (std::size_t at = window; at + window < frames; at += window) {
    const double c = cents(measured_hz(out.left, at, at + window, 48'000.0), 440.0);
    lowest = std::min(lowest, c);
    highest = std::max(highest, c);
  }
  REQUIRE_GT(lowest, -(nova::analogue_drift_cents + 0.5),
             "drift stays within ±8 cents at Amount 1");
  REQUIRE_LT(highest, nova::analogue_drift_cents + 0.5, "drift stays within ±8 cents at Amount 1");
  REQUIRE_GT(highest - lowest, 1.0, "and it moves");
}

void lanes_validate_strictly() {
  const auto& engine = nova::engine();
  const auto accepts = [&](project::NativeVoiceLane lane) {
    project::NativeSynthDocument document;
    document.voice_lanes.push_back(std::move(lane));
    return engine.validate_document(document).has_value();
  };
  const double nan = std::numeric_limits<double>::quiet_NaN();
  require(accepts({"notes.cutoff", {{3, 0.4}}}), "a note lane is accepted");
  require(accepts({"unison.b.pan", {{15, -1.0}}}), "a unison lane is accepted");
  require(!accepts({"notes.wobble", {{0, 0.1}}}), "an unknown lane is rejected");
  require(!accepts({"unison.a.cutoff", {{0, 0.1}}}), "a lane unison voices lack is rejected");
  require(!accepts({"unison.d.pitch", {{0, 0.1}}}), "an unknown oscillator is rejected");
  require(
      !accepts({"notes.pitch", {{static_cast<std::uint32_t>(nova::analogue_note_voices), 0.1}}}),
      "a note voice out of range is rejected");
  require(!accepts({"unison.a.pitch",
                    {{static_cast<std::uint32_t>(nova::analogue_unison_voices), 0.1}}}),
          "a unison voice out of range is rejected");
  require(!accepts({"notes.level", {{0, nan}}}), "a non-finite value is rejected");
  require(!accepts({"notes.level", {{0, 1.5}}}), "a value outside -1..1 is rejected");
  project::NativeSynthDocument document;
  REQUIRE_TRUE(!nova::set_analogue_lane_value(document, nova::AnalogueKind::unison, 0,
                                              nova::AnalogueLane::env, 0, 0.2),
               "the editing helper refuses a lane the kind lacks");
  REQUIRE_TRUE(document.voice_lanes.empty(), "the editing helper refuses a lane the kind lacks");
  require(nova::analogue_lane_values(preset_patch(std::nullopt).values, document,
                                     nova::AnalogueKind::unison, 0, nova::AnalogueLane::cutoff, 8)
              .empty(),
          "and the panel gets no values for it");
}

void the_appended_unison_curves_are_deterministic() {
  const auto stack = [](double curve) {
    auto patch = preset_patch(std::nullopt);
    set(patch, "nova.osc.a.type", 0.0);
    set(patch, "nova.osc.a.random_phase", 0.0);
    set(patch, "nova.osc.a.unison.voices", 7.0);
    set(patch, "nova.osc.a.unison.detune", 30.0);
    set(patch, "nova.osc.a.unison.curve", curve);
    return play(patch, note(0, 24'000, 57, 1), 24'000);
  };
  const auto linear = stack(0.0);
  for (const double curve : {3.0, 4.0}) {
    const auto once = stack(curve);
    require(identical(once, stack(curve)), "Inverse and Random render the same every time");
    require(!identical(once, linear), "and differ from Linear");
  }
}

void voice_mod_a_reaches_each_voice() {
  const auto level_with = [](double painted) {
    auto patch = sine_patch();
    set(patch, "nova.analogue.on", 0.0); // The voice sources need no Analogue.
    set(patch, "nova.osc.a.level", 0.5);
    set(patch, "nova.mod.0.amount", 0.5);
    patch.document.routes.push_back({0, "voice.mod_a", "", "nova.osc.a.level", true, 0.0, 1.0});
    require(nova::set_analogue_lane_value(patch.document, nova::AnalogueKind::notes, 0,
                                          nova::AnalogueLane::mod_a, 0, painted),
            "voice 0's Mod A is painted");
    return peak(play(patch, note(0, 24'000, 69, 1), 24'000).left, 12'000, 24'000);
  };
  REQUIRE_GT(level_with(1.0), 1.5 * level_with(-1.0),
             "voice.mod_a raises one voice's level and lowers another's");
}

void analogue_presets_play_well_under_full_scale() {
  const auto& presets = nova::descriptor().presets;
  std::size_t found = 0;
  for (std::size_t p = 0; p < presets.size(); ++p) {
    const auto patch = preset_patch(p);
    if (patch.values[nova::index().analogue.on] < 0.5)
      continue;
    ++found;
    const auto out = play(patch, chord(48'000), 96'000);
    const double loudest = std::max(peak(out.left), peak(out.right));
    require(loudest > 0.01 && loudest < 0.9,
            std::string(presets[p].name) + " is audible and peaks under -1 dBFS");
  }
  REQUIRE_GE(found, 5, "five factory presets use Analogue");
}

// ---- Voice cards, re-roll, flutter, noise AM ----------------------------------------------

// Notes `length` frames long every `spacing` frames, all on A4: each takes a
// fresh voice (the release is over before the next starts).
std::vector<Event> repeated_a4(std::size_t count, std::uint64_t length, std::uint64_t spacing) {
  std::vector<Event> events;
  for (std::size_t n = 0; n < count; ++n) {
    auto e = note(n * spacing, length, 69, n + 1);
    events.insert(events.end(), e.begin(), e.end());
  }
  return events;
}

constexpr std::uint64_t repeat_length = 12'000;
constexpr std::uint64_t repeat_spacing = 24'000;
// The steady middle of a repeated note, clear of its attack and release.
constexpr std::uint64_t repeat_settle = 3'000;
constexpr std::uint64_t repeat_window = 8'000;

double note_cents(const Rendered& out, std::size_t n) {
  const std::size_t from = n * repeat_spacing + repeat_settle;
  return cents(measured_hz(out.left, from, from + repeat_window, 48'000.0), 440.0);
}

// Everything the new controls touch in use: noise on, unison, Analogue on.
Patch noisy_patch() {
  auto patch = analogue(busy_patch());
  set(patch, "nova.noise.on", 1.0);
  set(patch, "nova.noise.level", 0.4);
  return patch;
}

void new_controls_at_their_defaults_are_exact() {
  const auto plain = noisy_patch();
  auto idle = noisy_patch();
  // Their values with nothing switched on: cards 0, re-roll 0, flutter 0
  // and noise AM off, whatever the other knobs say.
  set(idle, "nova.analogue.cards", 0.0);
  set(idle, "nova.analogue.reroll", 0.0);
  set(idle, "nova.analogue.flutter.amount", 0.0);
  set(idle, "nova.analogue.flutter.tone", 47.0);
  set(idle, "nova.noise.am.source", 0.0);
  set(idle, "nova.noise.am.amount", 0.3);
  set(idle, "nova.noise.am.curve", 0.9);
  require(identical(play(plain, chord(24'000), 36'000), play(idle, chord(24'000), 36'000)),
          "voice cards, re-roll, flutter and noise AM off change nothing");
}

void the_seventh_note_reuses_the_first_card() {
  auto patch = sine_patch();
  set(patch, "nova.analogue.age", 1.0);
  set(patch, "nova.analogue.pitch", 0.5);
  set(patch, "nova.analogue.cards", 6.0);
  // A seed whose first two cards are audibly apart.
  double first = 0.0, second = 0.0;
  const auto last_seed = static_cast<int>(nova::analogue_seed_max);
  for (int seed = 1; seed <= last_seed && std::fabs(first - second) < audible_cents; ++seed) {
    set(patch, "nova.analogue.seed", static_cast<double>(seed));
    first = lane_value(patch, nova::AnalogueKind::notes, nova::AnalogueLane::pitch, 0) *
            nova::analogue_pitch_cents;
    second = lane_value(patch, nova::AnalogueKind::notes, nova::AnalogueLane::pitch, 1) *
             nova::analogue_pitch_cents;
  }
  REQUIRE_GE(std::fabs(first - second), audible_cents, "some seed sets two cards audibly apart");
  constexpr std::size_t notes = 7;
  const auto out =
      play(patch, repeated_a4(notes, repeat_length, repeat_spacing), notes * repeat_spacing);
  REQUIRE_LT(std::fabs(note_cents(out, 0) - first), 0.2, "the first note plays card 1's offset");
  REQUIRE_LT(std::fabs(note_cents(out, 1) - second), 0.2, "the second note plays card 2's");
  REQUIRE_LT(std::fabs(note_cents(out, 6) - first), 0.2,
             "the seventh note takes card 1 again, though it took the same voice slot");
}

void reroll_moves_each_note_and_repeats() {
  auto patch = sine_patch();
  set(patch, "nova.analogue.age", 1.0);
  set(patch, "nova.analogue.pitch", 0.5);
  set(patch, "nova.analogue.cards", 1.0); // Every note on one card.
  constexpr std::size_t notes = 4;
  const auto events = repeated_a4(notes, repeat_length, repeat_spacing);
  const auto fixed = play(patch, events, notes * repeat_spacing);
  for (std::size_t n = 1; n < notes; ++n)
    REQUIRE_LT(std::fabs(note_cents(fixed, n) - note_cents(fixed, 0)), 0.2,
               "without re-roll one card plays every note alike");
  set(patch, "nova.analogue.reroll", 1.0);
  const auto rolled = play(patch, events, notes * repeat_spacing);
  double lowest = note_cents(rolled, 0), highest = lowest;
  for (std::size_t n = 0; n < notes; ++n) {
    const double c = note_cents(rolled, n);
    REQUIRE_LE(std::fabs(c), nova::analogue_pitch_cents + 0.2, "a re-roll stays within the part");
    lowest = std::min(lowest, c);
    highest = std::max(highest, c);
  }
  REQUIRE_GT(highest - lowest, 1.0, "re-roll moves successive notes on the same card");
  require(identical(rolled, play(patch, events, notes * repeat_spacing)),
          "and renders the same every time");
}

void flutter_jitters_within_its_maximum() {
  auto patch = sine_patch();
  set(patch, "nova.analogue.age", 0.0);
  set(patch, "nova.analogue.flutter.amount", 1.0);
  constexpr std::size_t frames = std::size_t{2} * 48'000;
  constexpr std::size_t window = 960; // 20 ms: shorter than a 12 Hz wobble.
  const auto events = note(0, frames, 69, 1);
  const auto out = play(patch, events, frames);
  double lowest = 0.0, highest = 0.0;
  for (std::size_t at = window; at + window < frames; at += window) {
    const double c = cents(measured_hz(out.left, at, at + window, 48'000.0), 440.0);
    lowest = std::min(lowest, c);
    highest = std::max(highest, c);
  }
  REQUIRE_GT(lowest, -(nova::analogue_flutter_cents + 0.5),
             "flutter stays within its maximum at Amount 1");
  REQUIRE_LT(highest, nova::analogue_flutter_cents + 0.5,
             "flutter stays within its maximum at Amount 1");
  REQUIRE_GT(highest - lowest, 1.0, "and it moves");
  require(identical(out, play(patch, events, frames, 0)),
          "steady and irregular blocks draw the same flutter");
}

// A low square on Osc A, silent at its own level, and noise through no
// filter: with AM the noise sounds only on the square's high half.
Patch gated_noise_patch(double source) {
  auto patch = preset_patch(std::nullopt);
  set(patch, "nova.osc.a.type", 0.0);
  set(patch, "nova.osc.a.shape", 3.0); // Square.
  set(patch, "nova.filter.1.on", 0.0);
  set(patch, "nova.noise.on", 1.0);
  set(patch, "nova.noise.level", 0.5);
  set(patch, "nova.noise.am.source", source);
  return patch;
}

void noise_follows_an_oscillator() {
  constexpr std::size_t frames = 36'000;
  constexpr std::uint8_t low_a = 33; // 55 Hz: 873 frames a cycle.
  const auto events = note(0, frames, low_a, 1);
  // The square alone, as the reference for which half is which.
  auto square = gated_noise_patch(0.0);
  set(square, "nova.noise.on", 0.0);
  const auto reference = play(square, events, frames);
  const double threshold = 0.2 * peak(reference.left);
  // Noise energy over the square's high half against its low half.
  const auto high_to_low = [&](double source, double curve) {
    auto patch = gated_noise_patch(source);
    set(patch, "nova.osc.a.level", 0.0);
    set(patch, "nova.noise.am.curve", curve);
    const auto out = play(patch, events, frames);
    double high = 0.0, low = 0.0;
    for (std::size_t i = frames / 3; i < frames; ++i) {
      if (reference.left[i] > threshold)
        high += out.left[i] * out.left[i];
      else if (reference.left[i] < -threshold)
        low += out.left[i] * out.left[i];
    }
    REQUIRE_GT(high + low, 0.0, "the noise sounds");
    return high / std::max(low, 1e-30);
  };
  const double off = high_to_low(0.0, 0.0);
  REQUIRE_GT(off, 0.5, "with AM off the noise ignores the oscillator");
  REQUIRE_LT(off, 2.0, "with AM off the noise ignores the oscillator");
  REQUIRE_GT(high_to_low(1.0, 0.0), 20.0,
             "following Osc A, the noise sounds on its high half only");

  // Curve: on a sine, below zero holds the level up, above zero gates it.
  const auto energy = [&](double curve) {
    auto patch = gated_noise_patch(1.0);
    set(patch, "nova.osc.a.shape", 0.0); // Sine.
    set(patch, "nova.osc.a.level", 0.0);
    set(patch, "nova.noise.am.curve", curve);
    const auto out = play(patch, events, frames);
    double sum = 0.0;
    for (std::size_t i = frames / 3; i < frames; ++i)
      sum += out.left[i] * out.left[i];
    return sum;
  };
  REQUIRE_GT(energy(-1.0), 2.0 * energy(1.0), "a smoother curve keeps more of the noise");
}

void cards_reroll_flutter_and_noise_am_render_alike_in_any_blocks() {
  auto patch = noisy_patch();
  set(patch, "nova.analogue.cards", 3.0);
  set(patch, "nova.analogue.reroll", 0.5);
  set(patch, "nova.analogue.flutter.amount", 0.6);
  set(patch, "nova.noise.am.source", 2.0); // Osc B.
  set(patch, "nova.noise.am.curve", 0.4);
  std::vector<Event> events = chord(18'000);
  auto late = note(9'001, 12'000, 67, 9);
  events.insert(events.end(), late.begin(), late.end());
  require(identical(play(patch, events, 36'000, 128), play(patch, events, 36'000, 0)),
          "steady and irregular blocks render the same audio");
}

} // namespace

// Two notes a glide apart on a mono sine: the second slides from the first.
std::vector<Event> slide() {
  auto events = note(0, 12'000, 57, 1);
  auto second = note(12'000, 36'000, 69, 2);
  events.insert(events.end(), second.begin(), second.end());
  return events;
}

Patch gliding(Patch patch, double seconds) {
  set(patch, "nova.global.voicing", 1.0);         // Mono.
  set(patch, "nova.global.portamento_mode", 1.0); // Always.
  set(patch, "nova.global.portamento", seconds);
  return patch;
}

void synced_portamento_is_its_note_value() {
  // With no transport Nova keeps 120 BPM: two beats a second.
  constexpr double beats_per_second = 2.0;
  for (const auto division : {std::size_t{12}, std::size_t{9}}) {
    const double seconds = nova::portamento_division_beats[division] / beats_per_second;
    auto timed = gliding(preset_patch(std::nullopt), seconds);
    auto synced = gliding(preset_patch(std::nullopt), 0.0);
    set(synced, "nova.global.portamento_sync", 1.0);
    set(synced, "nova.global.portamento_division", static_cast<double>(division));
    require(identical(play(timed, slide(), 48'000), play(synced, slide(), 48'000)),
            "a synced glide lasts its note value at the tempo");
  }
}

void each_card_glides_at_its_own_speed() {
  auto patch = sine_patch();
  set(patch, "nova.analogue.age", 1.0);
  for (const auto* part : {"pitch", "cutoff", "resonance", "env", "level", "pan", "shape"})
    set(patch, std::string("nova.analogue.") + part, 0.0);
  constexpr double glide_seconds = 0.2;
  auto exact = gliding(patch, glide_seconds);
  set(exact, "nova.analogue.glide", 0.0);
  auto loose = gliding(patch, glide_seconds);
  set(loose, "nova.analogue.glide", 1.0);
  const auto a = play(exact, slide(), 48'000);
  const auto b = play(loose, slide(), 48'000);
  const auto c = play(loose, slide(), 48'000, 61);
  require(identical(b, c), "a loose glide renders the same in any blocks");
  require(!identical(a, b), "glide tolerance changes the slide");
  // After both slides the note sounds in tune.
  const double hz_a = measured_hz(a.left, 36'000, 47'900, 48'000.0);
  const double hz_b = measured_hz(b.left, 36'000, 47'900, 48'000.0);
  REQUIRE_LT(std::fabs(cents(hz_b, hz_a)), 0.2, "both glides arrive at the note");
}

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "Nova Analogue contract passed",
      {{"off_ignores_every_analogue_setting", off_ignores_every_analogue_setting},
       {"age_zero_without_drift_is_exact", age_zero_without_drift_is_exact},
       {"seeds_repeat_and_differ", seeds_repeat_and_differ},
       {"any_block_split_renders_the_same", any_block_split_renders_the_same},
       {"a_voice_is_tuned_by_its_seeded_offset", a_voice_is_tuned_by_its_seeded_offset},
       {"a_painted_value_overrides_the_seed", a_painted_value_overrides_the_seed},
       {"drift_stays_within_its_amount", drift_stays_within_its_amount},
       {"lanes_validate_strictly", lanes_validate_strictly},
       {"the_appended_unison_curves_are_deterministic",
        the_appended_unison_curves_are_deterministic},
       {"voice_mod_a_reaches_each_voice", voice_mod_a_reaches_each_voice},
       {"analogue_presets_play_well_under_full_scale", analogue_presets_play_well_under_full_scale},
       {"new_controls_at_their_defaults_are_exact", new_controls_at_their_defaults_are_exact},
       {"the_seventh_note_reuses_the_first_card", the_seventh_note_reuses_the_first_card},
       {"reroll_moves_each_note_and_repeats", reroll_moves_each_note_and_repeats},
       {"flutter_jitters_within_its_maximum", flutter_jitters_within_its_maximum},
       {"noise_follows_an_oscillator", noise_follows_an_oscillator},
       {"cards_reroll_flutter_and_noise_am_render_alike_in_any_blocks",
        cards_reroll_flutter_and_noise_am_render_alike_in_any_blocks},
       {"synced_portamento_is_its_note_value", synced_portamento_is_its_note_value},
       {"each_card_glides_at_its_own_speed", each_card_glides_at_its_own_speed}});
}
