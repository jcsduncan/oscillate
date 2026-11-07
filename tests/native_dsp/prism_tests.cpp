#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/oscillator.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/prism.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using namespace oscillate;
using namespace oscillate::native_dsp::synth;

using oscillate::test::require;

const SynthEngine& prism_engine() { return prism::engine(); }
const EngineDescriptor& table() { return *prism_engine().descriptor; }

std::size_t index(std::string_view id) {
  const auto found = find_parameter(table(), id);
  REQUIRE_OK(found, "parameter exists");
  return *found;
}

std::vector<double> preset(std::string_view name) {
  for (const auto& p : table().presets) {
    if (p.name == name) {
      auto values = preset_values(table(), p);
      REQUIRE_OK(values, "preset resolves");
      return values.value();
    }
  }
  throw std::runtime_error("missing preset");
}

// A clean sine voice: Init with osc 1 as a sine and everything else neutral.
std::vector<double> sine_patch() {
  auto values = preset("Init");
  values[index("prism.osc1.wave")] = 3;
  values[index("prism.osc1.phase")] = 1;
  values[index("prism.filter.mode")] = 0;
  values[index("prism.lfo1.wheel")] = 0;
  return values;
}

struct Event {
  std::uint64_t frame;
  engine::MidiEventKind kind;
  std::uint8_t key;
  std::uint8_t velocity;
  std::uint64_t note;
};

engine::BlockMidiEvent block_event(const Event& e, std::uint64_t block_start) {
  engine::BlockMidiEvent out;
  out.offset = static_cast<std::uint32_t>(e.frame - block_start);
  out.clip_id_bits = {1, 1};
  out.note_id_bits = {0, e.note};
  out.kind = e.kind;
  out.key = e.key;
  out.velocity = e.velocity;
  return out;
}

std::vector<Event> sorted(std::vector<Event> events) {
  std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
    if (a.frame != b.frame) {
      return a.frame < b.frame;
    }
    if (a.kind != b.kind) {
      return a.kind < b.kind;
    }
    return a.note < b.note;
  });
  return events;
}

std::vector<Event> note(std::uint64_t start, std::uint64_t length, std::uint8_t key,
                        std::uint64_t id, std::uint8_t velocity = 100) {
  return {{start, engine::MidiEventKind::note_on, key, velocity, id},
          {start + length, engine::MidiEventKind::note_off, key, 0, id}};
}

std::unique_ptr<NativeSynthRuntime>
make_runtime(const std::vector<double>& values, double rate = oscillate::test::sample_rate_48k_hz) {
  auto runtime = NativeSynthRuntime::create(prism_engine(), values, rate);
  REQUIRE_OK(runtime, "runtime is created");
  return std::move(runtime.value());
}

struct Rendered {
  std::vector<double> left;
  std::vector<double> right;
  KernelStatus last{};
};

// Renders frames from frame zero with a fixed block size (0 cycles an
// irregular pattern), slicing the absolute event list into blocks.
Rendered render(NativeSynthRuntime& runtime, std::vector<Event> events, std::size_t frames,
                std::size_t block = 128, double bpm = 120.0, std::uint64_t generation = 1) {
  events = sorted(std::move(events));
  Rendered out;
  out.left.resize(frames);
  out.right.resize(frames);
  const std::size_t irregular[] = {37, 100, 513, 64, 1, 255};
  std::size_t next = 0;
  std::size_t position = 0;
  std::size_t cycle = 0;
  std::vector<engine::StereoFrame> buffer;
  std::vector<engine::BlockMidiEvent> block_events;
  while (position < frames) {
    const std::size_t size =
        std::min(frames - position, block == 0 ? irregular[cycle++ % 6] : block);
    buffer.assign(size, {0.0F, 0.0F});
    block_events.clear();
    while (next < events.size() && events[next].frame < position + size) {
      block_events.push_back(block_event(events[next++], position));
    }
    SynthTransport transport{static_cast<double>(position) * bpm / 60.0 / 48'000.0,
                             bpm / 60.0 / 48'000.0};
    const auto report = runtime.process(buffer, block_events, position, generation, transport, {});
    REQUIRE_EQ(report.status, SynthProcessStatus::rendered, "block renders");
    out.last = report.kernel;
    for (std::size_t i = 0; i < size; ++i) {
      out.left[position + i] = buffer[i].left;
      out.right[position + i] = buffer[i].right;
    }
    position += size;
  }
  return out;
}

double measured_hz(const std::vector<double>& x, std::size_t from, std::size_t to, double rate) {
  double first = -1.0;
  double last = -1.0;
  int crossings = 0;
  for (std::size_t i = from + 1; i < to; ++i) {
    if (x[i - 1] < 0.0 && x[i] >= 0.0) {
      const double t = static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
      if (first < 0.0) {
        first = t;
      } else {
        ++crossings;
      }
      last = t;
    }
  }
  return rate * crossings / (last - first);
}

double cents(double measured, double expected) { return 1200.0 * std::log2(measured / expected); }

double peak(const std::vector<double>& x, std::size_t from = 0, std::size_t to = SIZE_MAX) {
  double result = 0.0;
  for (std::size_t i = from; i < std::min(to, x.size()); ++i) {
    result = std::max(result, std::fabs(x[i]));
  }
  return result;
}

// ---- Descriptor --------------------------------------------------------------

void descriptor_and_presets_validate() {
  const auto valid = validate_engine(table());
  REQUIRE_OK(valid, valid.has_value() ? "" : valid.error().message);
  REQUIRE_EQ(table().id, "prism", "engine identity is prism v1");
  REQUIRE_EQ(table().version, 1, "engine identity is prism v1");
  REQUIRE_EQ(table().presets.size(), 16, "sixteen factory presets");
  REQUIRE_EQ(find_engine("prism"), &prism_engine(), "registry finds prism");
  REQUIRE_EQ(&default_engine(), &prism_engine(), "prism is the default engine");
  for (const auto& parameter : table().parameters) {
    require(parameter.id.starts_with("prism."), "ids are namespaced");
    if (parameter.scale == ParameterScale::linear ||
        parameter.scale == ParameterScale::logarithmic) {
      for (double n = 0.0; n <= 1.0; n += 0.125) {
        const double value = from_normalised(parameter, n);
        REQUIRE_LT(std::fabs(to_normalised(parameter, value) - n), tolerance::double_accumulation,
                   "knob travel round-trips");
      }
    }
    require(parameter.automatable, "every parameter is automatable (ADR-0034)");
  }
}

void sparse_values_round_trip() {
  const auto values = preset("Acid");
  const auto sparse = sparse_values(table(), values);
  REQUIRE_TRUE(!sparse.empty(), "only changes are saved");
  REQUIRE_LT(sparse.size(), values.size(), "only changes are saved");
  const auto restored = resolve_values(table(), sparse);
  REQUIRE_OK(restored, "sparse values restore exactly");
  REQUIRE_EQ(restored.value(), values, "sparse values restore exactly");
  require(!resolve_values(table(), std::vector<NamedValue>{{"prism.nope", 1.0}}).has_value(),
          "unknown ids are rejected");
  require(
      !resolve_values(table(), std::vector<NamedValue>{{"prism.filter.cutoff", 1e9}}).has_value(),
      "out-of-range values are rejected, not clamped");
  require(!resolve_values(table(), std::vector<NamedValue>{{"prism.osc1.unison", 2.5}}).has_value(),
          "fractional discrete values are rejected");
}

// ---- Pitch, envelope, rates --------------------------------------------------

void pitch_is_within_half_a_cent_at_every_rate() {
  for (double rate : {44'100.0, 48'000.0, 88'200.0, 96'000.0}) {
    for (std::uint8_t key : {std::uint8_t{33}, std::uint8_t{69}, std::uint8_t{96}}) {
      auto runtime = make_runtime(sine_patch(), rate);
      const auto frames = static_cast<std::size_t>(rate);
      const auto out = render(*runtime, note(0, frames, key, 1), frames);
      const double expected = 440.0 * std::exp2((key - 69.0) / 12.0);
      const double error = cents(measured_hz(out.left, frames / 4, frames, rate), expected);
      REQUIRE_LT(std::fabs(error), 0.5, "pitch within 0.5 cent");
    }
  }
}

void tuning_controls_shift_pitch_exactly() {
  auto values = sine_patch();
  values[index("prism.osc1.octave")] = -1;
  values[index("prism.osc1.semi")] = 7;
  values[index("prism.osc1.fine")] = -25;
  auto runtime = make_runtime(values);
  const auto out = render(*runtime, note(0, 48'000, 69, 1), 48'000);
  const double expected = 440.0 * std::exp2((-12.0 + 7.0 - 0.25) / 12.0);
  REQUIRE_LT(std::fabs(cents(measured_hz(out.left, 12'000, 48'000, 48'000.0), expected)), 0.5,
             "octave, semitone and fine combine exactly");
}

void amp_attack_lands_on_time() {
  auto values = sine_patch();
  values[index("prism.amp.attack")] = 0.1;
  values[index("prism.amp.curve")] = 0.0;
  auto runtime = make_runtime(values);
  const auto out = render(*runtime, note(0, 24'000, 81, 1), 24'000);
  // Envelope follower: peak per 1 ms window.
  const double top = peak(out.left, 9'600, 14'400);
  std::size_t reached = 0;
  for (std::size_t i = 0; i < out.left.size(); ++i) {
    if (std::fabs(out.left[i]) >= 0.98 * top) {
      reached = i;
      break;
    }
  }
  // A linear 100 ms attack reaches 98% at 98 ms, plus the oscillator delay.
  const double expected = 0.098 * 48'000.0 + BlepOscillator::latency_frames;
  REQUIRE_LT(std::fabs(static_cast<double>(reached) - expected), 0.01 * 4'800.0 + 32.0,
             "attack timing within 1% (plus one sine half-cycle)");
}

void released_notes_fall_silent_and_free_their_voice() {
  auto values = preset("Prism Default");
  auto runtime = make_runtime(values);
  const auto tail = prism_engine().tail_frames(values, 48'000.0);
  REQUIRE_GE(tail, static_cast<std::uint64_t>(0.35 * 48'000.0), "tail covers the release");
  const auto out = render(*runtime, note(0, 4'800, 60, 1), 4'800 + tail + 4'800);
  REQUIRE_EQ(out.last.active_voices, 0, "voice is freed after its release");
  REQUIRE_LT(peak(out.left, 4'800 + tail), 1e-4, "output is silent after the tail bound");
  REQUIRE_GT(peak(out.left, 0, 4'800), 0.01, "the note sounded");
}

// ---- Determinism -------------------------------------------------------------

std::vector<Event> busy_sequence() {
  std::vector<Event> events;
  std::uint64_t id = 1;
  for (std::uint64_t bar = 0; bar < 4; ++bar) {
    for (std::uint8_t key : {std::uint8_t{48}, std::uint8_t{55}, std::uint8_t{60}, std::uint8_t{64},
                             std::uint8_t{67}}) {
      auto n = note(bar * 12'000 + key * 13, 9'000, static_cast<std::uint8_t>(key + bar), id++,
                    static_cast<std::uint8_t>(40 + key));
      events.insert(events.end(), n.begin(), n.end());
    }
  }
  return events;
}

void any_block_split_renders_identically() {
  for (std::string_view name : {"Prism Default", "Wobble", "Acid", "Supersaw"}) {
    const auto values = preset(name);
    auto a = make_runtime(values);
    auto b = make_runtime(values);
    auto c = make_runtime(values);
    const auto x = render(*a, busy_sequence(), 60'000, 64);
    const auto y = render(*b, busy_sequence(), 60'000, 128);
    const auto z = render(*c, busy_sequence(), 60'000, 0);
    REQUIRE_EQ(x.left, y.left, "64 and 128 frame splits are identical");
    REQUIRE_EQ(x.right, y.right, "64 and 128 frame splits are identical");
    REQUIRE_EQ(x.left, z.left, "irregular splits are identical");
    REQUIRE_EQ(x.right, z.right, "irregular splits are identical");
  }
}

// ---- Stealing, seeks and click safety ----------------------------------------

void voice_stealing_is_bounded_and_declicked() {
  auto values = sine_patch();
  values[index("prism.out.level")] = -18;
  values[index("prism.amp.release")] = 2.0;
  auto runtime = make_runtime(values);
  std::vector<Event> events;
  // Seventeen held sines; the eighteenth steals the oldest (key 40) near
  // its waveform peak, which would click without the declick.
  for (std::uint64_t i = 0; i < 17; ++i) {
    auto n = note(i * 480, 40'000, static_cast<std::uint8_t>(40 + i), i + 1);
    events.insert(events.end(), n.begin(), n.end());
  }
  const auto out = render(*runtime, events, 24'000);
  REQUIRE_EQ(out.last.active_voices, prism::voice_limit, "polyphony is bounded at 16");
  REQUIRE_EQ(out.last.voice_steals, 1, "one voice was stolen");

  const std::size_t steal = 16 * 480;
  double steady = 0.0;
  double around = 0.0;
  for (std::size_t i = steal - 2'000; i < steal - 50; ++i) {
    steady = std::max(steady, std::fabs(out.left[i] - out.left[i - 1]));
  }
  for (std::size_t i = steal; i < steal + 200; ++i) {
    around = std::max(around, std::fabs(out.left[i] - out.left[i - 1]));
  }
  REQUIRE_LT(around, 1.5 * steady, "a steal adds no step");
}

void seek_resets_voices_without_a_step() {
  auto values = sine_patch();
  values[index("prism.amp.release")] = 5.0;
  auto runtime = make_runtime(values);
  std::vector<engine::StereoFrame> buffer(256);
  std::vector<engine::BlockMidiEvent> events{
      block_event({0, engine::MidiEventKind::note_on, 57, 127, 1}, 0)};
  double previous = 0.0;
  double largest = 0.0;
  KernelStatus status{};
  for (std::uint64_t block = 0; block < 100; ++block) {
    std::fill(buffer.begin(), buffer.end(), engine::StereoFrame{0.0F, 0.0F});
    const std::uint64_t generation = block < 50 ? 1 : 2; // Seek at block 50.
    const auto report = runtime->process(buffer, block == 0 ? events : decltype(events){},
                                         block * 256, generation, {}, {});
    status = report.kernel;
    for (const auto& frame : buffer) {
      if (block > 2) {
        largest = std::max(largest, std::fabs(frame.left - previous));
      }
      previous = frame.left;
    }
  }
  REQUIRE_EQ(status.active_voices, 0, "seek silences every voice");
  // A 220 Hz sine at this level moves at most ~0.01 per frame.
  REQUIRE_LT(largest, 0.02, "seek fades instead of stepping");
  REQUIRE_LT(std::fabs(previous), tolerance::float_round_trip, "output is silent after the seek");
}

// ---- Live control, automation and deferral -----------------------------------

void live_offers_are_revision_ordered_and_bounded() {
  const auto values = preset("Prism Default");
  auto runtime = make_runtime(values);
  auto changed = values;
  changed[index("prism.filter.cutoff")] = 800.0;
  require(!runtime->offer_values(changed, 0), "revision zero is rejected");
  require(runtime->offer_values(changed, 1), "first offer accepted");
  require(!runtime->offer_values(changed, 1), "repeated revision rejected");
  REQUIRE_TRUE(runtime->offer_values(changed, 2), "mailbox holds four offers");
  REQUIRE_TRUE(runtime->offer_values(changed, 3), "mailbox holds four offers");
  REQUIRE_TRUE(runtime->offer_values(changed, 4), "mailbox holds four offers");
  require(!runtime->offer_values(changed, 5), "a full mailbox rejects");
  auto bad = changed;
  bad[index("prism.filter.cutoff")] = 1e9;
  REQUIRE_EQ(runtime->applied_revision(), 0, "nothing applied before a block");

  auto out = render(*runtime, note(0, 9'600, 60, 1), 4'800);
  REQUIRE_EQ(runtime->applied_revision(), 4, "newest offer applied at the next block");
  REQUIRE_EQ(out.last.active_voices, 1, "a live edit keeps the sounding voice");
  require(!runtime->offer_values(bad, 6), "invalid values are rejected");
  require(runtime->offer_values(changed, 6), "the mailbox drained");
}

void structural_changes_apply_live() {
  const auto values = preset("Prism Default");
  auto changed = values;
  changed[index("prism.osc1.unison")] = 5;
  changed[index("prism.osc2.unison")] = 3;
  changed[index("prism.sub.octave")] = 1;
  changed[index("prism.filter.mode")] = 2;
  auto kept = make_runtime(values);
  auto switched = make_runtime(values);
  std::vector<engine::StereoFrame> a(128);
  std::vector<engine::StereoFrame> b(128);
  std::vector<engine::BlockMidiEvent> on{
      block_event({0, engine::MidiEventKind::note_on, 60, 100, 1}, 0)};
  (void)kept->process(a, on, 0, 1, {}, {});
  (void)switched->process(b, on, 0, 1, {}, {});
  require(switched->offer_values(changed, 1), "structural change offered");
  bool deferred = false;
  bool differs = false;
  for (std::uint64_t block = 1; block < 40; ++block) {
    a.assign(128, {0.0F, 0.0F});
    b.assign(128, {0.0F, 0.0F});
    (void)kept->process(a, {}, block * 128, 1, {}, {});
    const auto report = switched->process(b, {}, block * 128, 1, {}, {});
    deferred = deferred || report.kernel.deferred_changes;
    if (block > 12) {
      for (std::size_t i = 0; i < a.size(); ++i) {
        differs = differs || a[i].left != b[i].left;
      }
    }
  }
  require(!deferred, "unison, sub octave and filter mode are never deferred");
  require(differs, "the sounding note now plays the new structure");
}

void note_start_changes_are_reported_as_deferred() {
  const auto values = preset("Prism Default");
  auto runtime = make_runtime(values);
  std::vector<engine::StereoFrame> buffer(128);
  std::vector<engine::BlockMidiEvent> on{
      block_event({0, engine::MidiEventKind::note_on, 60, 100, 1}, 0)};
  auto report = runtime->process(buffer, on, 0, 1, {}, {});
  require(!report.kernel.deferred_changes, "nothing deferred initially");
  auto changed = values;
  changed[index("prism.osc1.phase")] = 1 - values[index("prism.osc1.phase")];
  require(runtime->offer_values(changed, 1), "phase mode change offered");
  report = runtime->process(buffer, {}, 128, 1, {}, {});
  require(report.kernel.deferred_changes, "a held note keeps its start phase: deferred");
  REQUIRE_TRUE(runtime->observed_status().deferred_changes,
               "control can observe the deferral without touching the kernel");
  REQUIRE_EQ(runtime->observed_status().active_voices, 1,
             "control can observe the deferral without touching the kernel");
  std::vector<engine::BlockMidiEvent> off{
      block_event({256, engine::MidiEventKind::note_off, 60, 0, 1}, 256)};
  report = runtime->process(buffer, off, 256, 1, {}, {});
  for (std::uint64_t block = 3; block < 200; ++block) {
    report = runtime->process(buffer, {}, block * 128, 1, {}, {});
  }
  REQUIRE_EQ(report.kernel.active_voices, 0, "deferral clears once old notes end");
  REQUIRE_TRUE(!report.kernel.deferred_changes, "deferral clears once old notes end");
}

void automation_lanes_are_validated_and_applied() {
  const auto values = preset("Prism Default");
  auto runtime = make_runtime(values);
  std::vector<engine::StereoFrame> buffer(64);
  std::vector<double> lane_values(64, 300.0);
  const auto cutoff = static_cast<std::uint32_t>(index("prism.filter.cutoff"));
  const auto unison = static_cast<std::uint32_t>(index("prism.osc1.unison"));
  std::vector<ParameterLane> lanes{{cutoff, lane_values}};
  REQUIRE_EQ(runtime->process(buffer, {}, 0, 1, {}, lanes).status, SynthProcessStatus::rendered,
             "valid lane accepted");
  // Discrete and next-note parameters take stepped lanes: whole values only.
  std::vector<double> voices(64, 3.0);
  std::vector<ParameterLane> topology{{unison, voices}};
  REQUIRE_EQ(runtime->process(buffer, {}, 64, 1, {}, topology).status, SynthProcessStatus::rendered,
             "a stepped lane on a next-note parameter is accepted");
  voices[7] = 2.5;
  REQUIRE_EQ(runtime->process(buffer, {}, 128, 1, {}, topology).status,
             SynthProcessStatus::invalid_lanes, "stepped lanes reject fractional values");
  std::vector<double> short_lane(10, 300.0);
  std::vector<ParameterLane> wrong_size{{cutoff, short_lane}};
  REQUIRE_EQ(runtime->process(buffer, {}, 64, 1, {}, wrong_size).status,
             SynthProcessStatus::invalid_lanes, "lanes must cover the block");
  lane_values[5] = 1e9;
  buffer.assign(64, {0.25F, 0.25F});
  REQUIRE_EQ(runtime->process(buffer, {}, 64, 1, {}, lanes).status,
             SynthProcessStatus::invalid_lanes, "out-of-range lane values are rejected");
  REQUIRE_EQ(buffer[0].left, 0.25F, "a rejected block leaves output untouched");

  // An automated cutoff sweep darkens the sound compared with the knob value.
  auto open = make_runtime(values);
  auto closed = make_runtime(values);
  const auto bright = render(*open, note(0, 24'000, 48, 1), 24'000);
  std::vector<double> dark_lane(24'000, 150.0);
  std::vector<ParameterLane> dark{{cutoff, dark_lane}};
  std::vector<engine::StereoFrame> whole(24'000);
  std::vector<engine::BlockMidiEvent> on{
      block_event({0, engine::MidiEventKind::note_on, 48, 100, 1}, 0)};
  (void)closed->process(
      std::span(whole).subspan(0, 8'192), on, 0, 1, {},
      std::vector<ParameterLane>{{cutoff, std::span<const double>(dark_lane).subspan(0, 8'192)}});
  double bright_change = 0.0;
  double dark_change = 0.0;
  for (std::size_t i = 4'001; i < 8'192; ++i) {
    bright_change += std::fabs(bright.left[i] - bright.left[i - 1]);
    dark_change += std::fabs(whole[i].left - whole[i - 1].left);
  }
  REQUIRE_LT(dark_change, 0.5 * bright_change, "automated cutoff closes the filter");
}

// ---- Voicing -----------------------------------------------------------------

void legato_glides_on_one_voice() {
  auto values = sine_patch();
  values[index("prism.voice.mode")] = 2;
  values[index("prism.voice.glide")] = 0.05;
  auto runtime = make_runtime(values);
  std::vector<Event> events = note(0, 24'000, 57, 1);
  auto second = note(12'000, 24'000, 69, 2);
  events.insert(events.end(), second.begin(), second.end());
  const auto out = render(*runtime, events, 36'000);
  REQUIRE_EQ(out.last.active_voices, 1, "legato uses a single voice");
  REQUIRE_LT(std::fabs(cents(measured_hz(out.left, 20'000, 34'000, 48'000.0), 440.0)), 1.0,
             "glide arrives at the new note");
  const double mid = measured_hz(out.left, 12'400, 13'600, 48'000.0);
  REQUIRE_GT(mid, 225.0, "pitch is between notes mid-glide");
  REQUIRE_LT(mid, 430.0, "pitch is between notes mid-glide");
}

void mono_returns_to_held_note() {
  auto values = sine_patch();
  values[index("prism.voice.mode")] = 1;
  auto runtime = make_runtime(values);
  std::vector<Event> events = note(0, 36'000, 57, 1);
  auto second = note(9'600, 9'600, 64, 2);
  events.insert(events.end(), second.begin(), second.end());
  const auto out = render(*runtime, events, 36'000);
  REQUIRE_LT(std::fabs(cents(measured_hz(out.left, 24'000, 35'000, 48'000.0), 220.0)), 1.0,
             "releasing the top note returns to the held one");
}

// ---- Presets and real-time ---------------------------------------------------

// The index of a choice parameter's option, by its label.
double choice(std::string_view id, std::string_view label) {
  const auto& choices = table().parameters[index(id)].choices;
  const auto found = std::ranges::find(choices, label);
  require(found != choices.end(), "choice exists");
  return static_cast<double>(found - choices.begin());
}

// A mono LFO follows its rate routes from sources every voice shares: the
// mod wheel doubling LFO 1's rate renders exactly as the doubled rate set
// directly, and unlike the undoubled one.
void mono_lfo_follows_rate_modulation() {
  constexpr double slow_hz = 0.5;
  constexpr double doubled_hz = 1.0;
  // A quarter of the four-octave range: one octave, so double the rate.
  constexpr double one_octave = 0.25;
  constexpr double full_wheel = 1.0;
  constexpr double vibrato_cents = 100.0;
  constexpr std::size_t frames = 48'000;
  constexpr std::uint8_t key = 69;
  const auto patch = [&](double hz, bool routed) {
    auto values = sine_patch();
    values[index("prism.lfo1.mode")] = choice("prism.lfo1.mode", "Mono");
    values[index("prism.lfo1.rate")] = hz;
    values[index("prism.lfo1.vibrato")] = vibrato_cents;
    values[index("prism.perf.wheel")] = full_wheel;
    if (routed) {
      values[index("prism.mod1.source")] = choice("prism.mod1.source", "Mod Wheel");
      values[index("prism.mod1.target")] = choice("prism.mod1.target", "LFO 1 Rate");
      values[index("prism.mod1.amount")] = one_octave;
    }
    auto runtime = make_runtime(values);
    return render(*runtime, note(0, frames, key, 1), frames).left;
  };
  const auto routed = patch(slow_hz, true);
  const auto direct = patch(doubled_hz, false);
  const auto unrouted = patch(slow_hz, false);
  double routed_error = 0.0;
  double unrouted_change = 0.0;
  for (std::size_t i = 0; i < frames; ++i) {
    routed_error = std::max(routed_error, std::fabs(routed[i] - direct[i]));
    unrouted_change = std::max(unrouted_change, std::fabs(unrouted[i] - direct[i]));
  }
  REQUIRE_LT(routed_error, tolerance::float_processing, "the wheel doubles the mono LFO's rate");
  REQUIRE_GT(unrouted_change, tolerance::coarse, "the rate changes the vibrato");
}

void every_preset_renders_sane_audio() {
  for (const auto& p : table().presets) {
    const auto values = preset(p.name);
    auto runtime = make_runtime(values);
    std::vector<Event> chord;
    std::uint64_t id = 1;
    for (std::uint8_t key :
         {std::uint8_t{48}, std::uint8_t{55}, std::uint8_t{60}, std::uint8_t{64}}) {
      auto n = note(0, 36'000, key, id++);
      chord.insert(chord.end(), n.begin(), n.end());
    }
    const auto out = render(*runtime, chord, 48'000);
    double power = 0.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < out.left.size(); ++i) {
      REQUIRE_TRUE(std::isfinite(out.left[i]), "preset is finite");
      REQUIRE_TRUE(std::isfinite(out.right[i]), "preset is finite");
      power += out.left[i] * out.left[i] + out.right[i] * out.right[i];
      sum += out.left[i] + out.right[i];
    }
    const double rms_db = 10.0 * std::log10(power / (2.0 * 48'000.0));
    std::cout << "  " << p.name << ": rms " << rms_db << " dBFS, peak "
              << 20.0 * std::log10(std::max(peak(out.left), peak(out.right))) << " dBFS\n";
    REQUIRE_LE(std::max(peak(out.left), peak(out.right)), 1.0, "preset never exceeds 0 dBFS");
    REQUIRE_GT(rms_db, -40.0, "preset is audible and not slammed");
    REQUIRE_LT(rms_db, -3.0, "preset is audible and not slammed");
    REQUIRE_LT(std::fabs(sum / (2.0 * 48'000.0)), 0.01, "preset has no DC offset");
  }
}

void full_load_process_does_not_allocate() {
  auto values = preset("Supersaw");
  values[index("prism.osc1.unison")] = 8;
  values[index("prism.osc2.unison")] = 8;
  values[index("prism.filter.mode")] = 8;
  auto runtime = make_runtime(values);
  std::vector<engine::StereoFrame> buffer(128);
  std::vector<engine::BlockMidiEvent> chord;
  for (std::uint8_t i = 0; i < 20; ++i) {
    chord.push_back(block_event(
        {0, engine::MidiEventKind::note_on, static_cast<std::uint8_t>(40 + i), 100, i + 1u}, 0));
  }
  std::vector<double> lane(128, 900.0);
  const std::vector<ParameterLane> lanes{
      {static_cast<std::uint32_t>(index("prism.filter.cutoff")), lane}};
  counting_allocations = true;
  counted_allocations = 0;
  KernelStatus status{};
  for (std::uint64_t block = 0; block < 64; ++block) {
    const auto report =
        runtime->process(buffer, block == 0 ? std::span(chord) : std::span(chord).subspan(0, 0),
                         block * 128, 1, {}, lanes);
    status = report.kernel;
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "the callback path never allocates");
  REQUIRE_EQ(status.active_voices, prism::voice_limit, "full polyphony is active");
}

// ---- Golden preset fingerprints ----------------------------------------------

void fft(std::vector<std::complex<double>>& data) {
  const std::size_t n = data.size();
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1U;
    for (; (j & bit) != 0; bit >>= 1U) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      std::swap(data[i], data[j]);
    }
  }
  for (std::size_t length = 2; length <= n; length <<= 1U) {
    const double angle = -2.0 * 3.14159265358979323846 / static_cast<double>(length);
    const std::complex<double> root(std::cos(angle), std::sin(angle));
    for (std::size_t start = 0; start < n; start += length) {
      std::complex<double> w(1.0, 0.0);
      for (std::size_t k = 0; k < length / 2; ++k) {
        const auto even = data[start + k];
        const auto odd = data[start + k + length / 2] * w;
        data[start + k] = even + odd;
        data[start + k + length / 2] = even - odd;
        w *= root;
      }
    }
  }
}

struct Fingerprint {
  double hold_db;
  double tail_db;
  double centroid_hz;
};

// A one-second C major chord, then one second of release, at 48 kHz.
Fingerprint fingerprint(std::string_view name) {
  auto runtime = make_runtime(preset(name));
  std::vector<Event> chord;
  std::uint64_t id = 1;
  for (std::uint8_t key :
       {std::uint8_t{48}, std::uint8_t{55}, std::uint8_t{60}, std::uint8_t{64}}) {
    auto n = note(0, 48'000, key, id++);
    chord.insert(chord.end(), n.begin(), n.end());
  }
  const auto out = render(*runtime, chord, 96'000);
  const auto rms_db = [&](std::size_t from, std::size_t to) {
    double power = 0.0;
    for (std::size_t i = from; i < to; ++i) {
      power += out.left[i] * out.left[i] + out.right[i] * out.right[i];
    }
    return 10.0 * std::log10(power / (2.0 * static_cast<double>(to - from)) + 1e-30);
  };
  std::vector<std::complex<double>> window(16'384);
  for (std::size_t i = 0; i < window.size(); ++i) {
    const double hann = oscillate::native_dsp::dsp::hann(static_cast<double>(i),
                                                         static_cast<double>(window.size()));
    window[i] = (out.left[4'096 + i] + out.right[4'096 + i]) * hann;
  }
  fft(window);
  double weighted = 0.0;
  double total = 0.0;
  for (std::size_t bin = 1; bin < window.size() / 2; ++bin) {
    const double magnitude = std::abs(window[bin]);
    weighted +=
        magnitude * static_cast<double>(bin) * 48'000.0 / static_cast<double>(window.size());
    total += magnitude;
  }
  return {rms_db(12'000, 48'000), rms_db(60'000, 72'000), total > 0.0 ? weighted / total : 0.0};
}

void factory_presets_match_their_golden_fingerprints() {
  // Re-baseline deliberately: PRISM_REBASELINE=1 prints the current table.
  struct Golden {
    std::string_view name;
    Fingerprint expected;
  };
  static constexpr std::array<Golden, 16> golden{{
      {"Init", {-16.359, -149.629, 4062.9}},
      {"Prism Default", {-18.646, -48.326, 2360.0}},
      {"Supersaw", {-19.413, -39.971, 4580.3}},
      {"Saw Lead", {-18.630, -102.504, 3135.9}},
      {"Square Lead", {-17.776, -114.066, 4584.0}},
      {"Reese Bass", {-16.496, -105.030, 621.6}},
      {"Sub Bass", {-15.855, -115.226, 297.0}},
      {"Pluck", {-46.367, -291.289, 484.7}},
      {"Soft Pad", {-18.033, -21.558, 2040.9}},
      {"Brass", {-14.845, -112.971, 3887.1}},
      {"Strings", {-17.723, -27.859, 3081.7}},
      {"Bell", {-25.183, -40.850, 300.7}},
      {"Sync Lead", {-16.289, -96.359, 4199.6}},
      {"Acid", {-21.313, -109.429, 858.4}},
      {"Wobble", {-14.127, -99.254, 1590.1}},
      {"Juno Chorus Pad", {-16.537, -21.282, 1961.9}},
  }};
  const bool rebaseline = std::getenv("PRISM_REBASELINE") != nullptr;
  for (const auto& entry : golden) {
    const auto actual = fingerprint(entry.name);
    if (rebaseline) {
      std::printf("      {\"%s\", {%.3f, %.3f, %.1f}},\n", std::string(entry.name).c_str(),
                  actual.hold_db, actual.tail_db, actual.centroid_hz);
      continue;
    }
    // Tails under -80 dB are numerical noise floor; only require silence.
    const bool silent_tail = entry.expected.tail_db < -80.0;
    REQUIRE_LT(std::fabs(actual.hold_db - entry.expected.hold_db), 0.2,
               std::string(entry.name) + ": level changed");
    require(silent_tail ? actual.tail_db < -80.0
                        : std::fabs(actual.tail_db - entry.expected.tail_db) < 0.5,
            std::string(entry.name) + ": release tail changed");
    REQUIRE_LT(std::fabs(actual.centroid_hz / entry.expected.centroid_hz - 1.0), 0.02,
               std::string(entry.name) + ": tone changed");
  }
}

} // namespace

int main() {
  try {
    descriptor_and_presets_validate();
    sparse_values_round_trip();
    pitch_is_within_half_a_cent_at_every_rate();
    tuning_controls_shift_pitch_exactly();
    amp_attack_lands_on_time();
    released_notes_fall_silent_and_free_their_voice();
    any_block_split_renders_identically();
    voice_stealing_is_bounded_and_declicked();
    seek_resets_voices_without_a_step();
    live_offers_are_revision_ordered_and_bounded();
    structural_changes_apply_live();
    note_start_changes_are_reported_as_deferred();
    automation_lanes_are_validated_and_applied();
    legato_glides_on_one_voice();
    mono_returns_to_held_note();
    mono_lfo_follows_rate_modulation();
    every_preset_renders_sane_audio();
    full_load_process_does_not_allocate();
    factory_presets_match_their_golden_fingerprints();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "prism engine contract passed\n";
  return 0;
}
