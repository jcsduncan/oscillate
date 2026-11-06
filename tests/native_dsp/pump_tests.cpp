#include "effect_test_support.hpp"

#include "oscillate/native_dsp/effects/pump.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

// Pump (ADR-0058) against measured oracles: drawn edges that never click,
// the duck reaching its drawn depth on the drawn beat, the kick trigger
// ducking on the kick with lookahead, Lows Only sparing the top, documents
// that change the sound live, block-split determinism and the panel's
// telemetry.
namespace {
using namespace effect_test;
namespace pump = oscillate::native_dsp::effects::pump;
namespace project = oscillate::project;

const effects::EffectEngine& engine() { return pump::engine(); }
const EngineDescriptor& table() { return *engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }

constexpr double rate = oscillate::test::sample_rate_48k_hz;
// 120 BPM: a quarter-note cycle is half a second.
constexpr std::size_t cycle = 24'000;

std::size_t preset_index(std::string_view name) {
  for (std::size_t i = 0; i < table().presets.size(); ++i) {
    if (table().presets[i].name == name)
      return i;
  }
  require(false, "preset exists");
  return 0;
}

std::unique_ptr<effects::NativeEffectRuntime> make(const std::vector<double>& values,
                                                   const project::NativeSynthDocument& document) {
  auto runtime = effects::NativeEffectRuntime::create(engine(), values, document, rate);
  REQUIRE_OK(runtime, "pump runtime is created");
  return std::move(runtime.value());
}

project::NativeSynthDocument shape_of(std::string_view preset) {
  return engine().preset_document(preset_index(preset));
}

// A constant level of 1: the output is the gain itself.
Stereo dc(std::size_t frames) {
  return {std::vector<double>(frames, 1.0), std::vector<double>(frames, 1.0)};
}

// Eight cascaded second-order high-passes at 1 kHz: a 40 Hz tone falls by
// more than 200 dB, so whatever comes through is the click.
std::vector<double> treble(const std::vector<double>& x) {
  const double w = 2.0 * std::numbers::pi * 1'000.0 / rate;
  const double alpha = std::sin(w) / std::numbers::sqrt2; // Q = 1 / sqrt(2)
  const double cosw = std::cos(w);
  const double a0 = 1.0 + alpha;
  const double b0 = (1.0 + cosw) / 2.0 / a0, b1 = -(1.0 + cosw) / a0, b2 = b0;
  const double a1 = -2.0 * cosw / a0, a2 = (1.0 - alpha) / a0;
  std::vector<double> y = x;
  for (int stage = 0; stage < 8; ++stage) {
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (double& sample : y) {
      const double in = sample;
      const double out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
      x2 = x1;
      x1 = in;
      y2 = y1;
      y1 = out;
      sample = out;
    }
  }
  return y;
}

// The click: the loudest 5 ms of energy above 1 kHz against the input's
// level, in dB, after the filters settle.
double click_db(const Stereo& out, const Stereo& input) {
  const auto high = treble(out.left);
  const std::size_t window = 240;
  double worst = 0.0;
  for (std::size_t from = 2 * cycle; from + window <= out.size(); from += window / 2)
    worst = std::max(worst, rms(high, from, from + window));
  return db(worst / rms(input.left, 0, input.size()));
}

void gate_edges_never_click() {
  // A 40 Hz sine through the hardest shape: Gate drops to silence in no
  // time at the top of each beat. The default anti-click keeps the click
  // 60 dB under the bass, and switching it off shows the click it removes.
  auto values = preset(table(), "Hard Gate");
  values[at("pump.anticlick")] = table().parameters[at("pump.anticlick")].default_value;
  const auto input = sine(40.0, 0.5, 8 * cycle);
  auto smooth = make(values, shape_of("Hard Gate"));
  const double smooth_db = click_db(process(*smooth, input), input);
  values[at("pump.anticlick")] = table().parameters[at("pump.anticlick")].minimum;
  auto sharp = make(values, shape_of("Hard Gate"));
  const double sharp_db = click_db(process(*sharp, input), input);
  std::cout << "gate click: " << smooth_db << " dB with the default, " << sharp_db
            << " dB at the minimum\n";
  REQUIRE_LT(smooth_db, -60.0, "the default anti-click keeps a gate's click under -60 dB");
  REQUIRE_GT(sharp_db, smooth_db + 6.0, "anti-click is what removes the click");
}

void the_duck_reaches_its_depth_on_the_beat() {
  auto values = preset(table(), "Hard Gate");
  auto runtime = make(values, shape_of("Hard Gate"));
  const auto out = process(*runtime, dc(4 * cycle));
  // Gate holds silence from 0 to 0.18 of the cycle and is back up by 0.2.
  const std::size_t beat = 2 * cycle;
  for (std::size_t i = beat + cycle / 50; i < beat + cycle * 16 / 100; ++i)
    REQUIRE_LT(std::fabs(out.left[i]), tolerance::coarse,
               "full depth silences the held part of the gate");
  for (std::size_t i = beat + cycle * 25 / 100; i < beat + cycle * 95 / 100; ++i)
    REQUIRE_LT(std::fabs(out.left[i] - 1.0), tolerance::coarse,
               "the rest of the cycle is untouched");
  // The smoothed edge crosses half way where the drawn edge does: the
  // shape is read early by the smoother's delay.
  const std::size_t drawn = beat + cycle * 19 / 100;
  std::size_t crossed = beat + cycle / 10;
  while (out.left[crossed] < 0.5)
    ++crossed;
  const double error_ms = (static_cast<double>(crossed) - static_cast<double>(drawn)) * 1e3 / rate;
  std::cout << "edge lands " << error_ms << " ms from the drawn one\n";
  REQUIRE_LT(std::fabs(error_ms), 0.5, "the release edge lands within half a millisecond");
  // Half depth stops at half the level.
  values[at("pump.depth")] = 0.5;
  auto half = make(values, shape_of("Hard Gate"));
  const auto halved = process(*half, dc(4 * cycle));
  REQUIRE_LT(std::fabs(halved.left[beat + cycle / 10] - 0.5), tolerance::coarse,
             "depth scales the duck");
}

void nudge_moves_the_shape() {
  auto values = preset(table(), "Hard Gate");
  values[at("pump.nudge")] = 10.0;
  auto runtime = make(values, shape_of("Hard Gate"));
  const auto out = process(*runtime, dc(4 * cycle));
  const std::size_t beat = 2 * cycle;
  std::size_t crossed = beat + cycle / 10;
  while (out.left[crossed] < 0.5)
    ++crossed;
  const double late_ms =
      (static_cast<double>(crossed) - static_cast<double>(beat + cycle * 19 / 100)) * 1e3 / rate;
  REQUIRE_LT(std::fabs(late_ms - 10.0), 0.5, "a 10 ms nudge plays the shape 10 ms later");
}

void kicks_trigger_the_duck_on_time() {
  // A kick burst in the key every 400 ms (off the tempo grid); with 5 ms
  // lookahead the duck is down when the kick reaches the output.
  auto values = preset(table(), "Kick Follower");
  values[at("pump.depth")] = 1.0;
  REQUIRE_EQ(engine().latency_frames(values, rate), 240, "5 ms of lookahead is the latency");
  const std::size_t frames = 4 * cycle;
  Stereo key{std::vector<double>(frames), std::vector<double>(frames)};
  const std::array<std::size_t, 4> kicks{9'000, 28'200, 47'400, 66'600};
  for (const auto kick : kicks) {
    for (std::size_t i = 0; i < 4'800; ++i) {
      const double x = 0.8 * std::exp(-static_cast<double>(i) / 1'200.0) *
                       std::sin(2.0 * std::numbers::pi * 55.0 * static_cast<double>(i) / rate);
      key.left[kick + i] = x;
      key.right[kick + i] = x;
    }
  }
  auto runtime = make(values, shape_of("Hard Gate"));
  const auto out = process(*runtime, dc(frames), {.sidechain = &key});
  for (const auto kick : kicks) {
    const std::size_t heard = kick + 240; // The kick's frame at the output.
    REQUIRE_GT(out.left[heard - 480], 0.99, "nothing ducks before the kick");
    // The drawn edge is at the kick; anti-click centres its S-curve there.
    REQUIRE_LT(out.left[heard], 0.6, "the duck is half down as the kick arrives");
    REQUIRE_LT(out.left[heard + 144], 0.05, "the duck is down 3 ms into the kick");
    REQUIRE_GT(out.left[heard + cycle / 4], 0.99, "the gate reopens after its drawn hold");
  }
}

void midi_notes_trigger_the_duck_ahead() {
  // Notes off the tempo grid restart the gate exactly on their frames, with
  // no lookahead: the stage hands the kernel the notes ahead of time.
  auto values = preset(table(), "Hard Gate");
  values[at("pump.trigger")] = 2.0;
  require(engine().key_notes, "Pump reads its keying tracks' notes");
  REQUIRE_EQ(engine().latency_frames(values, rate), 0, "the MIDI trigger needs no lookahead");
  auto runtime = make(values, shape_of("Hard Gate"));
  const std::vector<effects::KeyNote> notes{
      {9'000, 36, 100}, {28'200, 36, 100}, {47'400, 36, 100}, {66'600, 36, 100}};
  require(runtime->offer_key_notes(notes), "the notes are offered");
  require(!runtime->offer_key_notes(notes), "the same notes are not offered twice");
  const auto out = process(*runtime, dc(4 * cycle), {.block = 0});
  REQUIRE_GT(out.left[8'000], 0.999, "nothing ducks before the first note");
  for (const auto& note : notes) {
    const auto at_note = static_cast<std::size_t>(note.frame);
    REQUIRE_GT(out.left[at_note - 240], 0.99, "nothing ducks before a note");
    REQUIRE_LT(out.left[at_note], 0.6, "the duck is half down on the note");
    REQUIRE_LT(out.left[at_note + 96], 0.05, "the duck is down 2 ms into the note");
    REQUIRE_GT(out.left[at_note + cycle / 4], 0.99, "the gate reopens after its drawn hold");
  }
  // New notes replace the old ones at the next block.
  require(runtime->offer_key_notes({{30'000, 36, 100}}), "edited notes are offered");
  const auto moved = process(*runtime, dc(2 * cycle), {.generation = 2});
  REQUIRE_GT(moved.left[9'000 + 480], 0.99, "the duck follows the edited notes");
  REQUIRE_LT(moved.left[30'000 + 480], 0.05, "the duck follows the edited notes");
}

void lows_only_spares_the_top() {
  auto values = preset(table(), "Lows Pump");
  auto runtime = make(values, shape_of("Hard Gate"));
  const std::size_t frames = 4 * cycle;
  auto low = sine(50.0, 0.4, frames);
  auto high = sine(3'000.0, 0.4, frames);
  Stereo both{std::vector<double>(frames), std::vector<double>(frames)};
  for (std::size_t i = 0; i < frames; ++i) {
    both.left[i] = low.left[i] + high.left[i];
    both.right[i] = both.left[i];
  }
  const auto out = process(*runtime, both);
  // Inside the gate's hold the 50 Hz is gone and the 3 kHz stays.
  const std::size_t from = 2 * cycle + cycle / 25;
  const std::size_t to = 2 * cycle + cycle * 15 / 100;
  const auto top = treble(out.left);
  const auto reference = treble(high.left);
  REQUIRE_LT(std::fabs(db(rms(top, from, to) / rms(reference, from, to))), 0.2,
             "the treble keeps its level through the duck");
  REQUIRE_LT(db(rms(out.left, from, to) / rms(high.left, from, to)), 0.6,
             "the bass is ducked away under the crossover");
}

void documents_change_the_sound_live() {
  const auto values = preset(table(), "Classic Pump");
  auto runtime = make(values, shape_of("Classic Pump"));
  const std::size_t beat = 2 * cycle;
  const auto before = process(*runtime, dc(4 * cycle));
  REQUIRE_LT(before.left[beat + cycle / 4], 0.95, "Classic Pump is still recovering a quarter in");
  auto gate = shape_of("Hard Gate");
  REQUIRE_OK(runtime->offer_document(gate, 1), "a new shape is offered");
  require(runtime->offer_values(values, 1), "values follow the shape");
  const auto after = process(*runtime, dc(4 * cycle), {.generation = 2});
  REQUIRE_LT(std::fabs(after.left[beat + cycle / 4] - 1.0), tolerance::coarse,
             "the gate shape is heard next");
  project::NativeSynthDocument wrong = gate;
  wrong.curves.front().id = "other";
  require(!engine().validate_document(wrong).has_value(), "only the shape curve is accepted");
  wrong = gate;
  wrong.labels.push_back({"x", "y"});
  require(!engine().validate_document(wrong).has_value(), "nothing but the shape is accepted");
  REQUIRE_OK(engine().validate_document({}), "an empty document plays Classic Pump");
  wrong = gate;
  wrong.curves.push_back(gate.curves.front());
  require(!engine().validate_document(wrong).has_value(), "a curve is drawn once");
  auto banded = gate;
  banded.curves.push_back(gate.curves.front());
  banded.curves.back().id = pump::curve_id(1, 2);
  REQUIRE_EQ(banded.curves.back().id, "shape.b.high", "slots and bands each take a curve");
  REQUIRE_TRUE(engine().validate_document(banded), "slots and bands each take a curve");
}

// A flat shape: no duck at all.
project::NativeCurve flat(std::string id) {
  return {std::move(id), {{0.0, 1.0, 0.0}, {1.0, 1.0, 0.0}}};
}

// A kick burst at 55 Hz, as kicks_trigger_the_duck_on_time draws it.
void add_kick(Stereo& key, std::size_t at, double level = 0.8) {
  for (std::size_t i = 0; i < 4'800 && at + i < key.size(); ++i) {
    const double x = level * std::exp(-static_cast<double>(i) / 1'200.0) *
                     std::sin(2.0 * std::numbers::pi * 55.0 * static_cast<double>(i) / rate);
    key.left[at + i] += x;
    key.right[at + i] += x;
  }
}
// A loud, bright hat: 7 kHz with a fast decay.
void add_hat(Stereo& key, std::size_t at) {
  for (std::size_t i = 0; i < 2'400 && at + i < key.size(); ++i) {
    const double x = 0.9 * std::exp(-static_cast<double>(i) / 400.0) *
                     std::sin(2.0 * std::numbers::pi * 7'000.0 * static_cast<double>(i) / rate);
    key.left[at + i] += x;
    key.right[at + i] += x;
  }
}

void the_detector_hears_only_the_kick() {
  // A drum bus in the key: kicks with loud hats between them. Only the
  // kick's band restarts the cycle; opening the band to the hats shows they
  // would.
  auto values = preset(table(), "Drum Bus Kick");
  values[at("pump.follow")] = 0.0;
  values[at("pump.depth")] = 1.0;
  const std::size_t frames = 4 * cycle;
  Stereo key{std::vector<double>(frames), std::vector<double>(frames)};
  const std::array<std::size_t, 3> kicks{9'000, 33'000, 57'000};
  const std::array<std::size_t, 3> hats{21'000, 45'000, 69'000};
  for (const auto kick : kicks)
    add_kick(key, kick);
  for (const auto hat : hats)
    add_hat(key, hat);
  auto runtime = make(values, shape_of("Hard Gate"));
  const auto out = process(*runtime, dc(frames), {.sidechain = &key});
  for (const auto kick : kicks)
    REQUIRE_LT(out.left[kick + 240 + 144], 0.05, "each kick ducks");
  for (const auto hat : hats)
    REQUIRE_GT(out.left[hat + 240 + 144], 0.99, "no hat restarts the cycle");
  values[at("pump.detect_high")] = 4'000.0;
  values[at("pump.detect_low")] = 20.0;
  auto open = make(values, shape_of("Hard Gate"));
  const auto wide = process(*open, dc(frames), {.sidechain = &key});
  REQUIRE_LT(wide.left[hats[0] + 240 + 144], 0.05, "a wide-open detector hears the hats");

  // Listen plays what the detector hears: the kick's band, not the hats.
  values = preset(table(), "Drum Bus Kick");
  values[at("pump.listen")] = 1.0;
  auto listening = make(values, shape_of("Hard Gate"));
  const auto heard = process(*listening, dc(frames), {.sidechain = &key});
  const auto bright = treble(heard.left);
  REQUIRE_LT(db(rms(bright, hats[1], hats[1] + 2'400) / rms(key.left, hats[1], hats[1] + 2'400)),
             -40.0, "Listen leaves the hats out");
  REQUIRE_GT(rms(heard.left, kicks[1], kicks[1] + 2'400), 0.1, "Listen plays the kick");
}

void the_note_range_and_velocity_pick_the_kick() {
  // A drum rack keys Pump: kick on C1, hats on F#1 between. Only C1
  // triggers, and a softer kick ducks less.
  auto values = preset(table(), "Hard Gate");
  values[at("pump.trigger")] = 2.0;
  values[at("pump.note_low")] = 36.0;
  values[at("pump.note_high")] = 36.0;
  values[at("pump.velocity")] = 1.0;
  auto runtime = make(values, shape_of("Hard Gate"));
  require(runtime->offer_key_notes({{9'000, 36, 127},
                                    {9'000, 42, 90},
                                    {18'000, 42, 90},
                                    {33'000, 36, 64},
                                    {42'000, 42, 90}}),
          "the rack's notes are offered");
  const auto out = process(*runtime, dc(2 * cycle), {.block = 0});
  REQUIRE_LT(out.left[9'000 + 96], 0.05, "the full kick ducks fully");
  REQUIRE_GT(out.left[18'000 + 96], 0.99, "hats outside the range never trigger");
  REQUIRE_GT(out.left[42'000 + 96], 0.99, "hats outside the range never trigger");
  const double soft = 64.0 / 127.0;
  REQUIRE_LT(std::fabs(out.left[33'000 + 1'000] - soft), 0.02, "a half-velocity kick ducks half");
  std::vector<float> telemetry;
  REQUIRE_TRUE(runtime->read_telemetry(telemetry),
               "the last note on the keying tracks is reported for Learn");
  REQUIRE_EQ(telemetry[pump::telemetry::last_note], 42.0F,
             "the last note on the keying tracks is reported for Learn");
}

void kick_spacing_stretches_the_cycle() {
  // Half-time: notes two cycles apart. Following the spacing stretches the
  // gate's hold to the gap; on the rate it reopens a cycle early.
  auto values = preset(table(), "Hard Gate");
  values[at("pump.trigger")] = 2.0;
  const std::vector<effects::KeyNote> notes{
      {0, 36, 100}, {2 * cycle, 36, 100}, {4 * cycle, 36, 100}};
  auto fixed = make(values, shape_of("Hard Gate"));
  (void)fixed->offer_key_notes(notes);
  const auto straight = process(*fixed, dc(4 * cycle));
  values[at("pump.follow")] = 1.0;
  auto following = make(values, shape_of("Hard Gate"));
  (void)following->offer_key_notes(notes);
  const auto stretched = process(*following, dc(4 * cycle));
  const std::size_t probe = 2 * cycle + cycle * 3 / 10;
  REQUIRE_GT(straight.left[probe], 0.99, "on the rate the gate is open 0.3 cycles in");
  REQUIRE_LT(stretched.left[probe], 0.05, "stretched to the gap it is still held");

  // Kick Audio predicts the cycle from the last gap.
  auto audio = preset(table(), "Drum Bus Kick");
  audio[at("pump.depth")] = 1.0;
  const std::size_t frames = 7 * cycle;
  Stereo key{std::vector<double>(frames), std::vector<double>(frames)};
  for (const std::size_t kick : {std::size_t{2'000}, 2'000 + 2 * cycle, 2'000 + 4 * cycle})
    add_kick(key, kick);
  auto spaced = make(audio, shape_of("Hard Gate"));
  const auto out = process(*spaced, dc(frames), {.sidechain = &key});
  REQUIRE_LT(out.left[2'000 + 4 * cycle + 240 + cycle * 3 / 10], 0.05,
             "the kick's cycle follows the gap before it");
}

void bands_and_stereo_choose_what_pumps() {
  // Three Band with only the lows ducking: the bass goes, mids and highs
  // stay.
  auto values = preset(table(), "Three Band");
  values[at("pump.depth_mid")] = 0.0;
  values[at("pump.depth_high")] = 0.0;
  auto runtime = make(values, shape_of("Hard Gate"));
  const std::size_t frames = 4 * cycle;
  const auto low = sine(50.0, 0.3, frames);
  const auto mid = sine(700.0, 0.3, frames);
  const auto high = sine(6'000.0, 0.3, frames);
  Stereo all{std::vector<double>(frames), std::vector<double>(frames)};
  for (std::size_t i = 0; i < frames; ++i)
    all.left[i] = all.right[i] = low.left[i] + mid.left[i] + high.left[i];
  const auto out = process(*runtime, all);
  const std::size_t from = 2 * cycle + cycle / 25;
  const std::size_t to = 2 * cycle + cycle * 15 / 100;
  const auto above = treble(out.left);
  const auto reference = treble(all.left);
  REQUIRE_LT(std::fabs(db(rms(above, from, to) / rms(reference, from, to))), 0.3,
             "the highs pass the duck");
  std::vector<double> upper(frames);
  for (std::size_t i = 0; i < frames; ++i)
    upper[i] = mid.left[i] + high.left[i];
  REQUIRE_LT(std::fabs(db(rms(out.left, from, to) / rms(upper, from, to))), 0.5,
             "only the bass ducks: the output is the mids and highs");
  // Mid: the centre ducks, the sides stay.
  values = preset(table(), "Wide Pump");
  auto wide = make(values, shape_of("Hard Gate"));
  Stereo stereo{std::vector<double>(frames), std::vector<double>(frames)};
  for (std::size_t i = 0; i < frames; ++i) {
    stereo.left[i] = 0.5;
    stereo.right[i] = -0.5; // All side.
  }
  const auto sides = process(*wide, stereo);
  REQUIRE_LT(std::fabs(sides.left[2 * cycle + cycle / 10] - 0.5), tolerance::coarse,
             "Mid leaves the sides untouched");
  auto centre = make(values, shape_of("Hard Gate"));
  const auto ducked = process(*centre, dc(frames));
  REQUIRE_LT(std::fabs(ducked.left[2 * cycle + cycle / 10]), tolerance::coarse,
             "Mid ducks the centre");
}

void swing_patterns_and_morph_shape_the_bar() {
  // Swing 75 %: the second cycle of each pair starts half a cycle late.
  auto values = preset(table(), "Hard Gate");
  values[at("pump.swing")] = 0.75;
  auto swung = make(values, shape_of("Hard Gate"));
  const auto out = process(*swung, dc(4 * cycle));
  REQUIRE_GT(out.left[3 * cycle + cycle / 25], 0.99, "the off cycle has not started on the grid");
  REQUIRE_LT(out.left[3 * cycle + cycle / 2 + cycle / 25], 0.01, "it starts half a cycle late");

  // A four-cycle pattern with B (flat) on step 4.
  auto document = shape_of("Hard Gate");
  document.curves.push_back(flat(pump::curve_id(1, 0)));
  values = preset(table(), "Hard Gate");
  values[at("pump.pattern")] = 4.0;
  values[at("pump.step.4")] = 1.0;
  auto patterned = make(values, document);
  const auto bar = process(*patterned, dc(8 * cycle));
  for (std::size_t c = 0; c < 8; ++c) {
    const double level = bar.left[c * cycle + cycle / 10];
    require(c % 4 == 3 ? level > 0.99 : level < 0.01, "step 4 plays B, the others A");
  }
  // Morph halfway between the gate and flat: half the duck.
  values = preset(table(), "Hard Gate");
  values[at("pump.slot")] = 5.0;
  values[at("pump.morph")] = 0.5;
  auto morphing = make(values, document);
  const auto half = process(*morphing, dc(4 * cycle));
  REQUIRE_LT(std::fabs(half.left[2 * cycle + cycle / 10] - 0.5), tolerance::coarse,
             "morph blends A into B");
  // Automating Slot to B stops the duck.
  values[at("pump.slot")] = 2.0;
  auto fixed = make(values, document);
  REQUIRE_GT(process(*fixed, dc(4 * cycle)).left[2 * cycle + cycle / 10], 0.99, "Slot B plays B");
}

void block_splits_are_deterministic() {
  for (const auto band : {1.0, 2.0}) {
    auto values = preset(table(), "Bar Fill");
    values[at("pump.band")] = band;
    values[at("pump.stereo")] = 1.0;
    values[at("pump.swing")] = 0.6;
    const auto input = noise(0.3, 9 * cycle);
    auto a = make(values, shape_of("Bar Fill"));
    auto b = make(values, shape_of("Bar Fill"));
    const auto one = process(*a, input, {.block = 128});
    const auto two = process(*b, input, {.block = 0});
    REQUIRE_EQ(one.left, two.left, "any block split renders the same");
    REQUIRE_EQ(one.right, two.right, "any block split renders the same");
  }
  // MIDI with a note range, a seek and the spacing followed.
  auto values = preset(table(), "Rack Kick");
  const std::vector<effects::KeyNote> notes{
      {1'000, 36, 120}, {9'000, 42, 80}, {20'000, 36, 60}, {50'000, 36, 100}};
  const auto input = noise(0.3, 3 * cycle);
  auto a = make(values, shape_of("Rack Kick"));
  auto b = make(values, shape_of("Rack Kick"));
  (void)a->offer_key_notes(notes);
  (void)b->offer_key_notes(notes);
  const auto one = process(*a, input, {.block = 128}, 4'000);
  const auto two = process(*b, input, {.block = 0}, 4'000);
  REQUIRE_EQ(one.left, two.left, "MIDI renders the same from a seek in any block split");
}

void telemetry_feeds_the_panel() {
  auto values = preset(table(), "Hard Gate");
  auto runtime = make(values, shape_of("Hard Gate"));
  const auto key = sine(60.0, 0.5, 3 * cycle);
  (void)process(*runtime, dc(3 * cycle), {.sidechain = &key});
  std::vector<float> telemetry;
  REQUIRE_TRUE(runtime->read_telemetry(telemetry), "telemetry is published");
  REQUIRE_EQ(telemetry.size(), pump::telemetry::size, "telemetry is published");
  REQUIRE_GE(telemetry[pump::telemetry::phase], 0.0F, "the playhead is in the cycle");
  REQUIRE_LT(telemetry[pump::telemetry::phase], 1.0F, "the playhead is in the cycle");
  REQUIRE_LT(std::fabs(telemetry[pump::telemetry::cycle_seconds] - 0.5F), tolerance::near_silence_f,
             "the cycle is a quarter note at 120 BPM");
  REQUIRE_EQ(telemetry[pump::telemetry::keyed], 1.0F, "the key is reported");
  const auto kick = std::span(telemetry).subspan(pump::telemetry::kick, pump::view_points);
  REQUIRE_GT(*std::ranges::max_element(kick), 0.45F, "the key's cycle is captured");
  const auto applied = std::span(telemetry).subspan(pump::telemetry::applied, pump::view_points);
  REQUIRE_LT(*std::ranges::min_element(applied), 0.01F,
             "the applied curve shows the gate as heard");
  REQUIRE_GT(applied[pump::view_points / 2], 0.99F, "the applied curve shows the gate as heard");
  const auto before = std::span(telemetry).subspan(pump::telemetry::input, pump::view_points);
  const auto after = std::span(telemetry).subspan(pump::telemetry::output, pump::view_points);
  REQUIRE_GT(*std::ranges::min_element(before), 0.99F, "the input is captured before the duck");
  REQUIRE_LT(*std::ranges::min_element(after), 0.01F, "the output is captured after it");
  REQUIRE_GT(after[pump::view_points / 2], 0.99F, "the output is captured after it");
  REQUIRE_EQ(telemetry[pump::telemetry::slot], 0.0F, "slot A plays the one-step pattern");
  REQUIRE_EQ(telemetry[pump::telemetry::step], 0.0F, "slot A plays the one-step pattern");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "native pump contract passed",
      {
          {"gate_edges_never_click", [] { gate_edges_never_click(); }},
          {"the_duck_reaches_its_depth_on_the_beat",
           [] { the_duck_reaches_its_depth_on_the_beat(); }},
          {"nudge_moves_the_shape", [] { nudge_moves_the_shape(); }},
          {"kicks_trigger_the_duck_on_time", [] { kicks_trigger_the_duck_on_time(); }},
          {"midi_notes_trigger_the_duck_ahead", [] { midi_notes_trigger_the_duck_ahead(); }},
          {"lows_only_spares_the_top", [] { lows_only_spares_the_top(); }},
          {"documents_change_the_sound_live", [] { documents_change_the_sound_live(); }},
          {"the_detector_hears_only_the_kick", [] { the_detector_hears_only_the_kick(); }},
          {"the_note_range_and_velocity_pick_the_kick",
           [] { the_note_range_and_velocity_pick_the_kick(); }},
          {"kick_spacing_stretches_the_cycle", [] { kick_spacing_stretches_the_cycle(); }},
          {"bands_and_stereo_choose_what_pumps", [] { bands_and_stereo_choose_what_pumps(); }},
          {"swing_patterns_and_morph_shape_the_bar",
           [] { swing_patterns_and_morph_shape_the_bar(); }},
          {"block_splits_are_deterministic", [] { block_splits_are_deterministic(); }},
          {"telemetry_feeds_the_panel", [] { telemetry_feeds_the_panel(); }},
      });
}
