#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/anvil.hpp"
#include "oscillate/native_dsp/synth/lock.hpp"
#include "oscillate/native_dsp/synth/nova.hpp"
#include "oscillate/native_dsp/synth/nova/parameters.hpp"
#include "oscillate/native_dsp/synth/project_binding.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/signal.hpp"
#include "synth_test_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Kick & Bass Lock's engine contract (ADR-0072), with the followed notes
// offered straight to each runtime. Nova: Lock off leaves the output
// untouched; on, the crossover keeps every level, folds the lows to mono,
// and ducks them under each followed hit's own curve from the hit's first
// frame. Anvil: each hit's tail lands on the root or fifth of the bass note
// under it. Both render alike for any block split with Lock working.
namespace {
using namespace synth_test;
namespace anvil = oscillate::native_dsp::synth::anvil;
namespace nova = oscillate::native_dsp::synth::nova;

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr double bpm = 120.0;

double midi_hz(double key) { return 440.0 * std::pow(2.0, (key - 69.0) / 12.0); }

// The windowed DFT magnitude of x at hz over [from, from + length).
double magnitude(const std::vector<double>& x, std::size_t from, std::size_t length, double hz) {
  double re = 0.0;
  double im = 0.0;
  for (std::size_t i = 0; i < length && from + i < x.size(); ++i) {
    const double window =
        oscillate::native_dsp::dsp::hann(static_cast<double>(i), static_cast<double>(length - 1));
    const double t = 2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate;
    re += x[from + i] * window * std::cos(t);
    im += x[from + i] * window * std::sin(t);
  }
  return std::sqrt(re * re + im * im);
}

using oscillate::test::rms;

double db(double ratio) { return oscillate::test::gain_to_db(ratio); }

std::vector<double> mid(const Rendered& r) {
  std::vector<double> out(r.left.size());
  for (std::size_t i = 0; i < out.size(); ++i)
    out[i] = 0.5 * (r.left[i] + r.right[i]);
  return out;
}

std::vector<double> side(const Rendered& r) {
  std::vector<double> out(r.left.size());
  for (std::size_t i = 0; i < out.size(); ++i)
    out[i] = 0.5 * (r.left[i] - r.right[i]);
  return out;
}

bool identical(const Rendered& a, const Rendered& b) {
  return a.left == b.left && a.right == b.right;
}

void offer(NativeSynthRuntime& runtime, LockState state) {
  require(runtime.offer_lock(std::make_shared<const LockState>(std::move(state))),
          "the runtime takes the lock state");
}

std::vector<Event> join(std::initializer_list<std::vector<Event>> parts) {
  std::vector<Event> events;
  for (const auto& part : parts)
    events.insert(events.end(), part.begin(), part.end());
  return events;
}

// ---- Nova -------------------------------------------------------------------------

struct Patch {
  std::vector<double> values;
  project::NativeSynthDocument document;
};

void set(Patch& patch, std::string_view id, double value) {
  patch.values[index(nova::descriptor(), id)] = value;
}

// One steady oscillator, nothing moving: a panned sine unless `shape` says.
Patch plain(double shape = 0.0) {
  auto made = native_patch_for_preset(nova::engine(), std::nullopt);
  REQUIRE_OK(made, "the init patch is made");
  auto resolved = resolve_native_instrument(made.value());
  REQUIRE_OK(resolved, "the init patch resolves");
  Patch patch{resolved.value().values, resolved.value().document};
  set(patch, "nova.osc.a.type", 0.0);
  set(patch, "nova.osc.a.shape", shape);
  set(patch, "nova.osc.a.drift", 0.0);
  set(patch, "nova.osc.a.random_phase", 0.0);
  set(patch, "nova.global.drift", 0.0);
  set(patch, "nova.filter.1.on", 0.0);
  set(patch, "nova.osc.a.pan", -1.0); // Stereo: the side carries half of it.
  return patch;
}

// A wide, detuned unison saw bass: stereo all the way down.
Patch unison_bass() {
  auto patch = plain(2.0);
  set(patch, "nova.osc.a.pan", 0.0);
  set(patch, "nova.osc.a.unison.voices", 7.0);
  set(patch, "nova.osc.a.unison.detune", 30.0);
  set(patch, "nova.osc.a.unison.width", 1.0);
  set(patch, "nova.osc.a.unison.blend", 1.0);
  return patch;
}

void set_lock(Patch& patch, bool on, bool mono) {
  set(patch, lock_bass_parameter, on ? lock_choice_on : lock_choice_off);
  set(patch, "nova.lock.mono", mono ? lock_choice_on : lock_choice_off);
}

std::unique_ptr<NativeSynthRuntime> nova_runtime(const Patch& patch) {
  auto runtime = NativeSynthRuntime::create(nova::engine(), patch.values, patch.document, {}, rate);
  REQUIRE_OK(runtime, "a Nova runtime is created");
  return std::move(runtime).value();
}

Rendered render_nova(const Patch& patch, const std::vector<Event>& events, std::size_t frames,
                     std::optional<LockState> state = std::nullopt, std::size_t block = 128) {
  auto runtime = nova_runtime(patch);
  if (state)
    offer(*runtime, std::move(*state));
  return render(*runtime, events, frames, block, rate, bpm);
}

// One followed hit at `start`: full for `hold` frames, then a straight fall
// to silence over `fall`, sampled every lock_envelope_frames.
LockState one_hit(std::uint64_t start, std::uint64_t hold, std::uint64_t fall) {
  LockState state;
  std::vector<float> envelope;
  for (std::uint64_t frame = 0; frame <= hold + fall; frame += lock_envelope_frames) {
    const double level =
        frame <= hold ? 1.0 : 1.0 - static_cast<double>(frame - hold) / static_cast<double>(fall);
    envelope.push_back(static_cast<float>(std::max(0.0, level)));
  }
  envelope.push_back(0.0F);
  state.envelopes.push_back(std::move(envelope));
  state.notes.push_back({start, start + hold + fall, 36, 127, 0});
  return state;
}

// Off is no processing at all: the Lock settings and any followed notes
// leave the output bit for bit as the defaults render it.
void nova_lock_off_is_untouched() {
  const auto chord = join({note(0, 30'000, 24, 1), note(0, 30'000, 55, 2), note(0, 30'000, 81, 3)});
  const auto defaults = plain();
  auto moved = defaults;
  set(moved, "nova.lock.duck", 1.0);
  set(moved, "nova.lock.crossover", 200.0);
  set(moved, "nova.lock.mono", lock_choice_off);
  const auto a = render_nova(defaults, chord, 36'000);
  const auto b = render_nova(moved, chord, 36'000);
  const auto c = render_nova(moved, chord, 36'000, one_hit(6'000, 4'800, 4'800));
  require(identical(a, b), "Lock off ignores its settings");
  require(identical(a, c), "Lock off ignores the followed hits");
}

// With nothing to follow, Lock keeps every level (its crossover sums to an
// allpass): the mid is untouched in level and so is the side above the
// crossover, while Mono Lows takes the side away below it.
void nova_lock_keeps_levels_and_monos_the_lows() {
  constexpr std::uint8_t low_key = 24;  // 32.7 Hz, well under the 120 Hz crossover.
  constexpr std::uint8_t high_key = 81; // 880 Hz, well over it.
  const auto notes = join({note(0, 96'000, low_key, 1), note(0, 96'000, high_key, 2)});
  auto off = plain();
  auto mono = off;
  set_lock(mono, true, true);
  auto wide = off;
  set_lock(wide, true, false);
  const auto a = render_nova(off, notes, 96'000);
  const auto b = render_nova(mono, notes, 96'000);
  const auto c = render_nova(wide, notes, 96'000);
  constexpr std::size_t from = 24'000; // Past the switch fade and the attack.
  constexpr std::size_t length = 48'000;
  const double low = midi_hz(low_key);
  const double high = midi_hz(high_key);
  const auto ratio = [&](const std::vector<double>& after, const std::vector<double>& before,
                         double hz) {
    return magnitude(after, from, length, hz) / magnitude(before, from, length, hz);
  };
  REQUIRE_GT(magnitude(side(a), from, length, low), 1e-3, "the input has a low side");
  REQUIRE_LT(std::fabs(db(ratio(mid(b), mid(a), low))), 0.05,
             "the mid keeps its level below and above the crossover");
  REQUIRE_LT(std::fabs(db(ratio(mid(b), mid(a), high))), 0.05,
             "the mid keeps its level below and above the crossover");
  REQUIRE_LT(std::fabs(db(ratio(side(b), side(a), high))), 0.1,
             "the side above the crossover keeps its level");
  REQUIRE_LT(ratio(side(b), side(a), low), 0.02, "Mono Lows folds the side below the crossover");
  REQUIRE_LT(std::fabs(db(ratio(side(c), side(a), low))), 0.05,
             "without Mono Lows the low side keeps its level");
}

// A wide unison bass: with Mono Lows its fundamental has almost no side
// left, while its upper harmonics stay wide.
void nova_mono_lows_on_a_unison_bass() {
  constexpr std::uint8_t key = 24;
  const auto held = note(0, 96'000, key, 1);
  auto off = unison_bass();
  auto mono = off;
  set_lock(mono, true, true);
  const auto a = render_nova(off, held, 96'000);
  const auto b = render_nova(mono, held, 96'000);
  constexpr std::size_t from = 24'000;
  constexpr std::size_t length = 48'000;
  const double fundamental = midi_hz(key);
  const double before = magnitude(side(a), from, length, fundamental);
  REQUIRE_GT(before, 1e-3, "the unison bass is wide at its fundamental");
  REQUIRE_LT(magnitude(side(b), from, length, fundamental) / before, 0.05,
             "the fundamental's side is gone");
  // The side's slope weighs the upper harmonics: they stay wide.
  const auto slope = [&](const std::vector<double>& x) {
    std::vector<double> out(x.size(), 0.0);
    for (std::size_t i = 1; i < x.size(); ++i)
      out[i] = x[i] - x[i - 1];
    return rms(out, from, from + length);
  };
  const double kept = slope(side(b)) / slope(side(a));
  REQUIRE_GT(kept, 0.9, "the upper harmonics stay wide");
  REQUIRE_LT(kept, 1.05, "the upper harmonics stay wide");
}

// The duck: nothing changes before the hit's frame, the lows dip by Duck
// under it from that frame on, and they recover once it has passed.
void nova_duck_follows_the_hit_with_no_latency() {
  constexpr std::uint8_t key = 28; // 41.2 Hz.
  constexpr std::uint64_t hit = 24'000;
  constexpr std::uint64_t hold = 4'800;
  constexpr std::uint64_t fall = 4'800;
  constexpr double duck = 0.65;
  const auto held = note(0, 72'000, key, 1);
  auto patch = plain();
  set(patch, "nova.osc.a.pan", 0.0);
  set_lock(patch, true, false);
  set(patch, "nova.lock.duck", duck);
  const auto ducked = render_nova(patch, held, 72'000, one_hit(hit, hold, fall));
  const auto plain_lock = render_nova(patch, held, 72'000, LockState{});
  for (std::size_t i = 0; i < hit; ++i)
    require(ducked.left[i] == plain_lock.left[i] && ducked.right[i] == plain_lock.right[i],
            "nothing changes before the hit");
  bool starts = false;
  for (std::size_t i = hit; i < hit + lock_envelope_frames; ++i)
    starts = starts || ducked.left[i] != plain_lock.left[i];
  require(starts, "the duck starts on the hit's frame");
  const auto x = mid(ducked);
  const auto y = mid(plain_lock);
  const double dip = rms(x, hit + 480, hit + hold) / rms(y, hit + 480, hit + hold);
  REQUIRE_GT(dip, (1.0 - duck) - 0.03, "the lows dip by Duck under the hit");
  REQUIRE_LT(dip, (1.0 - duck) + 0.05, "the lows dip by Duck under the hit");
  double largest = 0.0;
  double loudest = 0.0;
  for (std::size_t i = hit + hold + fall + 2'400; i < x.size(); ++i) {
    largest = std::max(largest, std::fabs(x[i] - y[i]));
    loudest = std::max(loudest, std::fabs(y[i]));
  }
  REQUIRE_GT(loudest, 0.01, "the lows recover after the hit");
  REQUIRE_LT(largest, 1e-4 * loudest, "the lows recover after the hit");
}

// Lock working (duck, mono and switch fade) renders alike for any split.
void nova_block_splits_render_identically_with_lock() {
  auto patch = unison_bass();
  set_lock(patch, true, true);
  // Four hits sharing one envelope, the way a velocity's hits do.
  auto hits = one_hit(1'000, 2'000, 4'000);
  for (std::uint64_t h = 1; h < 4; ++h)
    hits.notes.push_back({1'000 + h * 12'000, 7'000 + h * 12'000, 36, 127, 0});
  const auto notes = join({note(0, 30'000, 24, 1), note(30'000, 18'000, 31, 2)});
  const auto x = render_nova(patch, notes, 60'000, hits, 128);
  const auto y = render_nova(patch, notes, 60'000, hits, 0);
  const auto z = render_nova(patch, notes, 60'000, hits, 1'024);
  REQUIRE_TRUE(identical(x, y), "block splits render identically with Lock on");
  REQUIRE_TRUE(identical(x, z), "block splits render identically with Lock on");
}

// ---- Anvil ------------------------------------------------------------------------

const EngineDescriptor& anvil_table() { return *anvil::engine().descriptor; }
std::size_t at(std::string_view id) { return index(anvil_table(), id); }

// Only the body, nothing after it but a transparent master.
std::vector<double> body_only() {
  auto v = preset(anvil_table(), "Init");
  v[at("anvil.click.level")] = 0.0;
  v[at("anvil.rumble.on")] = 0.0;
  v[at("anvil.sub.on")] = 0.0;
  v[at("anvil.body.drive")] = 0.0;
  v[at("anvil.body.harmonics")] = 0.0;
  v[at("anvil.drive.amount")] = 0.0;
  v[at("anvil.punch")] = 0.0;
  v[at("anvil.glue")] = 0.0;
  v[at("anvil.tone")] = 1.0;
  v[at("anvil.output")] = 0.0;
  v[at("anvil.velocity")] = 0.0;
  return v;
}

project::NativeCurvePoint pt(double x, double y, double tension = 0.0) {
  return {x, y, tension, project::NativeCurveSegment::curve};
}

// A body that falls from 300 Hz to a held `tail` Hz, at a flat level.
project::NativeSynthDocument held_tail(double tail) {
  project::NativeSynthDocument document;
  document.curves.push_back({std::string(anvil::pitch_curve),
                             {pt(0, anvil::y_of_hz(300), -0.6), pt(0.3, anvil::y_of_hz(tail)),
                              pt(1, anvil::y_of_hz(tail))}});
  document.curves.push_back({std::string(anvil::amp_curve), {pt(0, 0.8), pt(1, 0.8)}});
  document.curves.push_back({std::string(anvil::drive_curve), {pt(0, 0), pt(1, 0)}});
  return document;
}

std::unique_ptr<NativeSynthRuntime> anvil_runtime(const std::vector<double>& values,
                                                  const project::NativeSynthDocument& document) {
  auto runtime = NativeSynthRuntime::create(anvil::engine(), values, document, {}, rate);
  REQUIRE_OK(runtime, "an Anvil runtime is created");
  return std::move(runtime).value();
}

LockState bass(std::initializer_list<LockNote> notes) {
  LockState state;
  state.notes = notes;
  return state;
}

// The tail's pitch for one hit at frame 0 with `state` followed, Lock at
// `choice` (Off, Bass note or Keep tone).
double tail_hz(double choice, std::optional<LockState> state) {
  constexpr double drawn = 55.0;
  auto values = body_only();
  values[at(lock_kick_parameter)] = choice;
  auto runtime = anvil_runtime(values, held_tail(drawn));
  if (state)
    offer(*runtime, std::move(*state));
  const auto x = mid(render(*runtime, note(0, 100, 60, 1), 20'000, 128, rate, bpm));
  return measured_hz(x, 12'000, 19'000, rate);
}

// The tail moves to the bass note's root or fifth, whichever octave of the
// two lies nearest the drawn 55 Hz; with Lock off, no bass under the hit or
// a bass note too late, it stays drawn.
void anvil_lock_tunes_the_tail_to_the_bass() {
  constexpr double drawn = 55.0;
  constexpr std::uint64_t held = 48'000;
  // A#1 (58.27 Hz): its root is nearest.
  const double root = midi_hz(34);
  constexpr double on = lock_kick_bass_note;
  constexpr double off = lock_choice_off;
  REQUIRE_LT(std::fabs(cents(tail_hz(on, bass({{0, held, 34, 100}})), root)), 2.0,
             "the tail lands on the bass note's root");
  // E2: its root's octaves (41.2, 82.4 Hz) are further than its fifth's
  // (B1, 61.7 Hz).
  const double fifth = midi_hz(47) / 2.0;
  REQUIRE_LT(std::fabs(cents(tail_hz(on, bass({{0, held, 40, 100}})), fifth)), 2.0,
             "the tail lands on the bass note's fifth");
  // A bass note starting a 1/32 after the hit still counts as under it; one
  // a quarter note later does not (the lookahead is a 1/16).
  const auto quarter = static_cast<std::uint64_t>(rate * 60.0 / bpm);
  REQUIRE_LT(std::fabs(cents(tail_hz(on, bass({{quarter / 8, held, 34, 100}})), root)), 2.0,
             "a bass note just after the hit counts");
  REQUIRE_LT(std::fabs(cents(tail_hz(on, bass({{quarter, held, 34, 100}})), drawn)), 2.0,
             "a later bass note does not");
  REQUIRE_LT(std::fabs(cents(tail_hz(off, bass({{0, held, 34, 100}})), drawn)), 2.0,
             "Lock off keeps the drawn tail");
  REQUIRE_LT(std::fabs(cents(tail_hz(on, LockState{}), drawn)), 2.0,
             "with no bass under the hit the tail stays drawn");
}

// Keep tone locks the kick (Nova follows its hits) but leaves the drawn
// tail where the user drew it, whatever the bass plays.
void anvil_keep_tone_locks_without_retuning() {
  constexpr double drawn = 55.0;
  constexpr std::uint64_t held = 48'000;
  REQUIRE_LT(std::fabs(cents(tail_hz(lock_kick_keep_tone, bass({{0, held, 34, 100}})), drawn)), 2.0,
             "Keep tone keeps the drawn tail");
  REQUIRE_LT(std::fabs(cents(tail_hz(lock_kick_keep_tone, bass({{0, held, 40, 100}})), drawn)), 2.0,
             "Keep tone keeps the drawn tail");
  auto values = body_only();
  values[at(lock_kick_parameter)] = lock_kick_keep_tone;
  require(anvil::engine().lock_enabled(values), "Keep tone locks the kick");
  values[at(lock_kick_parameter)] = lock_kick_bass_note;
  require(anvil::engine().lock_enabled(values), "Bass note locks the kick");
  values[at(lock_kick_parameter)] = lock_choice_off;
  require(!anvil::engine().lock_enabled(values), "Off does not");
}

// Hits over a moving bassline render alike for any split.
void anvil_block_splits_render_identically_with_lock() {
  auto values = preset(anvil_table(), "Warehouse Rumble");
  values[at(lock_kick_parameter)] = lock_kick_bass_note;
  std::size_t which = 0;
  while (anvil_table().presets[which].name != "Warehouse Rumble")
    ++which;
  const auto document = anvil::engine().preset_document(which);
  const auto line = bass({{0, 23'000, 33, 100},
                          {23'000, 46'000, 36, 100},
                          {46'000, 70'000, 40, 100},
                          {70'000, 120'000, 31, 100}});
  std::vector<Event> hits;
  for (std::uint64_t i = 0; i < 4; ++i) {
    const auto hit = note(i * 23'225 + 7, 100, 36, i + 1, static_cast<std::uint8_t>(70 + 15 * i));
    hits.insert(hits.end(), hit.begin(), hit.end());
  }
  const auto run = [&](std::size_t block) {
    auto runtime = anvil_runtime(values, document);
    offer(*runtime, line);
    return render(*runtime, hits, 120'000, block, rate, bpm);
  };
  const auto x = run(128);
  REQUIRE_TRUE(identical(x, run(0)), "block splits render identically with Lock on");
  REQUIRE_TRUE(identical(x, run(1'024)), "block splits render identically with Lock on");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "kick and bass lock contract passed",
      {
          {"nova_lock_off_is_untouched", [] { nova_lock_off_is_untouched(); }},
          {"nova_lock_keeps_levels_and_monos_the_lows",
           [] { nova_lock_keeps_levels_and_monos_the_lows(); }},
          {"nova_mono_lows_on_a_unison_bass", [] { nova_mono_lows_on_a_unison_bass(); }},
          {"nova_duck_follows_the_hit_with_no_latency",
           [] { nova_duck_follows_the_hit_with_no_latency(); }},
          {"nova_block_splits_render_identically_with_lock",
           [] { nova_block_splits_render_identically_with_lock(); }},
          {"anvil_lock_tunes_the_tail_to_the_bass",
           [] { anvil_lock_tunes_the_tail_to_the_bass(); }},
          {"anvil_keep_tone_locks_without_retuning",
           [] { anvil_keep_tone_locks_without_retuning(); }},
          {"anvil_block_splits_render_identically_with_lock",
           [] { anvil_block_splits_render_identically_with_lock(); }},
      });
}
