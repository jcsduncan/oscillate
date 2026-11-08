#include "fret_test_library.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/fret.hpp"
#include "oscillate/native_dsp/synth/fret_feel.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "synth_test_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

// Fret's engine contract (ADR-0088), on a synthetic library whose
// articulations sound on different harmonics: a missing or malformed
// library is silent and says why; keys play in tune at any session rate;
// Feel's decisions reach the samples (ghosts, staccato, pushes, Pocket);
// a forced articulation plays on every note; round robins cycle and restart
// with the transport; slides glide; releases sound; streaming, block splits
// and resets are deterministic; the callback never allocates; the tail
// bound covers a released note.
namespace {

using namespace oscillate::native_dsp::synth;
using namespace synth_test;
namespace library = fret::library;

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr double bpm = 120.0;
constexpr double seconds_per_beat = 60.0 / bpm;
constexpr std::uint8_t e1 = 28;
constexpr std::uint8_t b1 = 35;
constexpr std::uint8_t e2 = 40;
constexpr std::uint8_t loud = 110;
constexpr double cents_per_octave = 1'200.0;
// A reset fades the last output out over 1.5 ms; compare past it.
constexpr std::size_t declick_frames = 128;

const ParameterDescriptor& parameter(std::string_view id) {
  for (const auto& p : fret::engine().descriptor->parameters) {
    if (p.id == id) {
      return p;
    }
  }
  require(false, std::string("unknown parameter ") + std::string(id));
  return fret::engine().descriptor->parameters.front();
}

std::size_t at(std::string_view id) {
  const auto& all = fret::engine().descriptor->parameters;
  return static_cast<std::size_t>(&parameter(id) - all.data());
}

double choice_of(std::string_view id, std::string_view name) {
  const auto& choices = parameter(id).choices;
  for (std::size_t i = 0; i < choices.size(); ++i) {
    if (choices[i] == name) {
      return static_cast<double>(i);
    }
  }
  require(false, std::string("unknown choice ") + std::string(name));
  return 0.0;
}

// Defaults with the tone stage out of the way and the notes as drawn.
std::vector<double> plain() {
  auto v = default_values(*fret::engine().descriptor);
  v[at("fret.feel.style")] = choice_of("fret.feel.style", "As drawn");
  v[at("fret.tone.amp")] = 0.0;
  v[at("fret.tone.squash")] = 0.0;
  v[at("fret.release")] = 0.0;
  return v;
}

std::filesystem::path scratch() {
  return std::filesystem::temp_directory_path() / ("oscillate-fret-" + std::to_string(::getpid()));
}

library::Status install(double note_seconds = fret_test::resident_note_seconds) {
  const auto base = scratch();
  fret_test::write_library(base, note_seconds);
  auto status = library::load(base);
  REQUIRE_EQ(status.state, library::State::ready, "the test library loads: " + status.message);
  return status;
}

// A note from a clip: its length is known when it starts, as Feel needs.
std::vector<Event> clip_note(std::uint64_t start, std::uint64_t length, std::uint8_t key,
                             std::uint64_t id, std::uint8_t velocity) {
  auto events = note(start, length, key, id, velocity);
  events.front().frames = static_cast<std::uint32_t>(length);
  return events;
}

std::size_t frames_of(double seconds) { return static_cast<std::size_t>(seconds * rate); }
std::uint64_t at_beat(double beat) {
  return static_cast<std::uint64_t>(std::llround(beat * seconds_per_beat * rate));
}

std::vector<double> mono(const Rendered& r) {
  std::vector<double> out(r.left.size());
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = 0.5 * (r.left[i] + r.right[i]);
  }
  return out;
}

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

// Which of the test library's harmonics (1..4) is loudest.
std::uint32_t strongest_harmonic(const std::vector<double>& x, std::size_t from, int key) {
  constexpr std::size_t window = 8'192;
  std::uint32_t best = 1;
  double loudest = -1.0;
  for (std::uint32_t h = 1; h <= 4; ++h) {
    const double m = magnitude(x, from, window, fret_test::key_hz(key) * h);
    if (m > loudest) {
      loudest = m;
      best = h;
    }
  }
  return best;
}

// The frequency near `hz` with the most energy, by golden-section search.
double pitch_near(const std::vector<double>& x, std::size_t from, std::size_t length, double hz) {
  constexpr double span = 0.03; // ±3 % around the guess.
  constexpr int iterations = 60;
  double lo = hz * (1.0 - span);
  double hi = hz * (1.0 + span);
  const double ratio = (std::sqrt(5.0) - 1.0) / 2.0;
  for (int i = 0; i < iterations; ++i) {
    const double a = hi - ratio * (hi - lo);
    const double b = lo + ratio * (hi - lo);
    if (magnitude(x, from, length, a) > magnitude(x, from, length, b)) {
      hi = b;
    } else {
      lo = a;
    }
  }
  return 0.5 * (lo + hi);
}

double rms(const std::vector<double>& x, std::size_t from, std::size_t length) {
  double sum = 0.0;
  for (std::size_t i = from; i < from + length && i < x.size(); ++i) {
    sum += x[i] * x[i];
  }
  return std::sqrt(sum / static_cast<double>(std::max<std::size_t>(1, length)));
}

std::size_t onset(const std::vector<double>& x, std::size_t from = 0) {
  constexpr double threshold = 1e-4;
  for (std::size_t i = from; i < x.size(); ++i) {
    if (std::fabs(x[i]) > threshold) {
      return i;
    }
  }
  return x.size();
}

std::vector<float> telemetry_of(NativeSynthRuntime& runtime) {
  std::vector<float> out;
  REQUIRE_TRUE(runtime.read_telemetry(out), "telemetry arrives");
  REQUIRE_GE(out.size(), fret::telemetry::size, "telemetry arrives");
  return out;
}

void a_missing_library_is_silent() {
  const auto nowhere = scratch() / "nothing-here";
  ::setenv(std::string(library::directory_variable).c_str(), nowhere.c_str(), 1);
  const auto status = library::status();
  REQUIRE_EQ(status.state, library::State::missing, "without the library Fret says why");
  REQUIRE_TRUE(!status.message.empty(), "without the library Fret says why");
  auto runtime = make_runtime(fret::engine(), plain());
  const auto r = mono(render(*runtime, note(0, frames_of(0.2), e1, 1, loud), frames_of(0.3)));
  REQUIRE_EQ(rms(r, 0, r.size()), 0.0, "and is silent");
  REQUIRE_EQ(telemetry_of(*runtime)[fret::telemetry::library],
             static_cast<float>(library::State::missing), "telemetry says the library is missing");
}

void malformed_libraries_are_rejected() {
  const auto base = scratch();
  fret_test::write_library(base, fret_test::resident_note_seconds);
  const auto manifest = base / library::manifest_file;
  std::string text;
  {
    std::ifstream in(manifest);
    text.assign(std::istreambuf_iterator<char>(in), {});
  }
  const auto broken = [&](const std::string& replaced) {
    std::ofstream(manifest) << replaced;
    return library::load(base).state;
  };
  REQUIRE_EQ(broken("not a fret library\n" + text), library::State::invalid, "format line");
  std::string channels = text;
  channels.replace(channels.find("channels 1"), 10, "channels 2");
  REQUIRE_EQ(broken(channels), library::State::invalid, "stereo packs are not Fret's");
  std::string no_e1;
  for (std::size_t from = 0; from < text.size();) {
    const auto end = text.find('\n', from);
    const auto line = text.substr(from, end - from + 1);
    if (line.find("sample pluck 28 ") == std::string::npos) {
      no_e1 += line;
    }
    from = end + 1;
  }
  REQUIRE_EQ(broken(no_e1), library::State::invalid, "a key without a pluck");
  std::ofstream(manifest) << text;
  std::filesystem::resize_file(base / "fret.pack",
                               std::filesystem::file_size(base / "fret.pack") / 2);
  REQUIRE_EQ(library::load(base).state, library::State::invalid, "a truncated pack");
}

void descriptor_registry_and_presets() {
  REQUIRE_EQ(find_engine("fret"), &fret::engine(), "Fret is registered");
  REQUIRE_OK(validate_engine(*fret::engine().descriptor), "the table validates");
  REQUIRE_EQ(fret::engine().descriptor->presets.size(), 6, "six presets, Funk Pocket the defaults");
  REQUIRE_EQ(fret::engine().descriptor->presets.front().name, "Funk Pocket",
             "six presets, Funk Pocket the defaults");
  REQUIRE_TRUE(fret::engine().descriptor->presets.front().values.empty(),
               "six presets, Funk Pocket the defaults");
  require(fret::engine().models_pedals, "a bass ignores the pedals");
}

void keys_are_in_tune() {
  REQUIRE_EQ(install().state, library::State::ready, "the test library loads");
  constexpr double max_cents = 1.0;
  for (const double session : {44'100.0, 48'000.0, 96'000.0}) {
    for (const auto key : {static_cast<std::uint8_t>(fret::first_key), e1, b1,
                           static_cast<std::uint8_t>(fret::last_key)}) {
      auto runtime = make_runtime(fret::engine(), plain(), session);
      const auto x =
          mono(render(*runtime, note(0, static_cast<std::uint64_t>(0.25 * session), key, 1, loud),
                      static_cast<std::size_t>(0.3 * session), 128, session));
      const double want = fret_test::key_hz(key);
      const auto from = static_cast<std::size_t>(0.02 * session);
      const auto length = static_cast<std::size_t>(0.2 * session);
      // magnitude() assumes 48 kHz: scale the measured frequency back.
      const double got = pitch_near(x, from, length, want * rate / session) * session / rate;
      REQUIRE_LT(std::fabs(cents_per_octave * std::log2(got / want)), max_cents,
                 "key " + std::to_string(key) + " in tune at " + std::to_string(session) + " Hz");
    }
  }
}

void velocity_layers_and_round_robins() {
  REQUIRE_EQ(install().state, library::State::ready, "the test library loads");
  const auto level = [](std::uint8_t velocity) {
    auto runtime = make_runtime(fret::engine(), plain());
    const auto x = mono(render(*runtime, note(0, frames_of(0.2), e1, 1, velocity), frames_of(0.2)));
    return rms(x, 0, x.size());
  };
  REQUIRE_GT(level(127), level(96), "louder velocities play louder");
  REQUIRE_GT(level(96), level(64), "louder velocities play louder");
  REQUIRE_GT(level(64), level(32), "louder velocities play louder");
  REQUIRE_GT(level(32), level(1), "louder velocities play louder");
  // Two hits on one key alternate round robins (1 dB apart) and restart
  // with the transport.
  auto runtime = make_runtime(fret::engine(), plain());
  std::vector<Event> events = note(0, frames_of(0.1), e1, 1, loud);
  const auto second = note(frames_of(0.25), frames_of(0.1), e1, 2, loud);
  events.insert(events.end(), second.begin(), second.end());
  const auto x = mono(render(*runtime, events, frames_of(0.4)));
  const auto from_declick = [&](const std::vector<double>& y) {
    return std::vector<double>(y.begin() + static_cast<std::ptrdiff_t>(declick_frames), y.end());
  };
  const double first_db = 20.0 * std::log10(rms(x, 0, frames_of(0.05)));
  const double second_db = 20.0 * std::log10(rms(x, frames_of(0.25), frames_of(0.05)));
  REQUIRE_LT(std::fabs(first_db - second_db - (-fret_test::round_robin_step_db)), 0.2,
             "the second hit plays the next round robin");
  constexpr std::uint64_t seek = 2;
  const auto again = mono(render(*runtime, events, frames_of(0.4), 128, rate, bpm, {}, seek));
  REQUIRE_EQ(from_declick(again), from_declick(x),
             "a seek restarts the round robins: export equals playback");
}

void feel_picks_articulations() {
  REQUIRE_EQ(install().state, library::State::ready, "the test library loads");
  auto v = default_values(*fret::engine().descriptor);
  v[at("fret.tone.amp")] = 0.0;
  v[at("fret.tone.squash")] = 0.0;
  v[at("fret.release")] = 0.0;
  // E1 on the one, then a sixteenth-long E1 on the off-beat eighth: a ghost.
  auto runtime = make_runtime(fret::engine(), v);
  auto events = clip_note(at_beat(0.0), at_beat(0.4), e1, 1, 100);
  const auto ghost = clip_note(at_beat(0.5), at_beat(0.25) - 1, e1, 2, 100);
  events.insert(events.end(), ghost.begin(), ghost.end());
  const auto x = mono(render(*runtime, events, frames_of(0.6)));
  REQUIRE_EQ(strongest_harmonic(x, at_beat(0.5) + frames_of(0.005), e1), 3,
             "Feel's ghost plays the ghost sample");
  const auto t = telemetry_of(*runtime);
  REQUIRE_EQ(t[fret::telemetry::last_articulation],
             static_cast<float>(fret::feel::Articulation::ghost), "telemetry reports the ghost");
  REQUIRE_EQ(t[fret::telemetry::last_velocity], static_cast<float>(fret::feel::ghost_velocity),
             "telemetry reports the ghost");
}

void forced_articulation_plays_every_note() {
  REQUIRE_EQ(install().state, library::State::ready, "the test library loads");
  auto v = plain();
  v[at("fret.articulation")] = choice_of("fret.articulation", "Behind the bridge");
  auto runtime = make_runtime(fret::engine(), v);
  const auto x = mono(render(*runtime, note(0, frames_of(0.3), e1, 1, loud), frames_of(0.3)));
  REQUIRE_EQ(strongest_harmonic(x, frames_of(0.01), e1), 4, "every note plays behind the bridge");
}

void pocket_plays_off_beats_late() {
  REQUIRE_EQ(install().state, library::State::ready, "the test library loads");
  constexpr double pocket_ms = 15.0;
  const auto onset_with = [&](double pocket, double beat) {
    auto v = default_values(*fret::engine().descriptor);
    v[at("fret.feel.pocket")] = pocket;
    v[at("fret.tone.amp")] = 0.0;
    v[at("fret.tone.squash")] = 0.0;
    auto runtime = make_runtime(fret::engine(), v);
    const auto x = mono(render(*runtime, note(at_beat(beat), at_beat(0.4), e1, 1, loud),
                               at_beat(beat) + frames_of(0.2)));
    return onset(x);
  };
  const auto late = static_cast<std::size_t>(std::llround(pocket_ms * rate / 1'000.0));
  REQUIRE_EQ(onset_with(pocket_ms, 0.5), onset_with(0.0, 0.5) + late, "an off-beat sits back");
  REQUIRE_EQ(onset_with(pocket_ms, 1.0), onset_with(0.0, 1.0), "a beat stays put");
}

void slides_glide_to_the_next_note() {
  REQUIRE_EQ(install(fret_test::streamed_note_seconds).state, library::State::ready,
             "the test library loads");
  constexpr double slide_ms = 50.0;
  auto v = plain();
  v[at("fret.slide")] = slide_ms;
  auto runtime = make_runtime(fret::engine(), v);
  // E1 held, B1 starts before it ends: the E1 string slides up to B1.
  auto events = note(0, frames_of(0.3), e1, 1, loud);
  const auto up = note(frames_of(0.2), frames_of(0.2), b1, 2, loud);
  events.insert(events.end(), up.begin(), up.end());
  const auto x = mono(render(*runtime, events, frames_of(0.45)));
  const double got = pitch_near(x, frames_of(0.3), frames_of(0.12), fret_test::key_hz(b1));
  REQUIRE_LT(std::fabs(cents_per_octave * std::log2(got / fret_test::key_hz(b1))), 2.0,
             "the slide arrives at B1");
  REQUIRE_EQ(strongest_harmonic(x, frames_of(0.3), b1), 1, "and keeps sounding the E1 pluck");
}

void releases_sound_at_note_off() {
  REQUIRE_EQ(install().state, library::State::ready, "the test library loads");
  const auto played = [&](double release) {
    auto v = plain();
    v[at("fret.release")] = release;
    auto runtime = make_runtime(fret::engine(), v);
    return mono(render(*runtime, note(0, frames_of(0.1), e2, 1, loud), frames_of(0.2)));
  };
  const auto with = played(1.0);
  const auto without = played(0.0);
  std::vector<double> noise(with.size());
  for (std::size_t i = 0; i < with.size(); ++i) {
    noise[i] = with[i] - without[i];
  }
  constexpr double audible = 1e-3;
  REQUIRE_EQ(rms(noise, 0, frames_of(0.1)), 0.0,
             "the string's release sounds at note-off, and only then");
  REQUIRE_GT(rms(noise, frames_of(0.1), frames_of(0.04)), audible,
             "the string's release sounds at note-off, and only then");
}

void streaming_block_splits_and_resets_are_deterministic() {
  REQUIRE_EQ(install(fret_test::streamed_note_seconds).state, library::State::ready,
             "the streamed test library loads");
  auto v = default_values(*fret::engine().descriptor);
  v[at("fret.feel.pocket")] = 9.0;
  v[at("fret.slide")] = 30.0;
  std::vector<Event> events;
  std::uint64_t id = 1;
  for (int beat = 0; beat < 4; ++beat) {
    for (const double offset : {0.0, 0.5, 0.75}) {
      const auto n = clip_note(at_beat(beat + offset), at_beat(offset == 0.0 ? 0.6 : 0.2),
                               beat % 2 == 0 ? e1 : e2, id++, loud);
      events.insert(events.end(), n.begin(), n.end());
    }
  }
  const auto frames = at_beat(4.5);
  auto reference = make_runtime(fret::engine(), v);
  const auto expected = render(*reference, events, frames, 128);
  for (const std::size_t block :
       {std::size_t{1}, std::size_t{31}, std::size_t{0}, std::size_t{512}}) {
    auto runtime = make_runtime(fret::engine(), v);
    const auto r = render(*runtime, events, frames, block);
    require(r.left == expected.left && r.right == expected.right,
            "block " + std::to_string(block) +
                " renders identically, streamed past the resident start");
  }
  constexpr std::uint64_t seek = 2;
  const auto again = render(*reference, events, frames, 128, rate, bpm, {}, seek);
  require(std::equal(again.left.begin() + static_cast<std::ptrdiff_t>(declick_frames),
                     again.left.end(),
                     expected.left.begin() + static_cast<std::ptrdiff_t>(declick_frames)),
          "a reset renders the same again, past the reset's declick");
}

void the_callback_does_not_allocate() {
  REQUIRE_EQ(install(fret_test::streamed_note_seconds).state, library::State::ready,
             "the streamed test library loads");
  auto v = default_values(*fret::engine().descriptor);
  v[at("fret.release")] = 1.0;
  v[at("fret.tone.grit")] = 0.5;
  v[at("fret.feel.pocket")] = 10.0;
  v[at("fret.slide")] = 40.0;
  auto runtime = make_runtime(fret::engine(), v);
  constexpr std::size_t block = 128;
  constexpr std::uint64_t blocks = 400;
  constexpr std::uint64_t every = 4;
  constexpr int keys = 12;
  std::vector<engine::StereoFrame> buffer(block);
  std::vector<engine::BlockMidiEvent> events(2);
  for (auto& e : events) {
    e.clip_id_bits = {1, 1};
    e.velocity = loud;
  }
  events[0].kind = engine::MidiEventKind::note_off;
  events[1].kind = engine::MidiEventKind::note_on;
  events[1].offset = block / 2;
  events[1].frames = static_cast<std::uint32_t>(block * every);
  counted_allocations = 0;
  counting_allocations = true;
  for (std::uint64_t b = 0; b < blocks; ++b) {
    const auto hit = b / every;
    events[0].note_id_bits = {0, hit};
    events[0].key = static_cast<std::uint8_t>(e1 + static_cast<int>((hit + keys - 1) % keys));
    events[1].note_id_bits = {0, hit + 1};
    events[1].key = static_cast<std::uint8_t>(e1 + static_cast<int>(hit % keys));
    const auto due = b % every == 0 ? std::span(events) : std::span(events).subspan(0, 0);
    runtime->await_content();
    (void)runtime->process(buffer, due, b * block, 1, {}, {});
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0,
             "the callback allocated " + std::to_string(counted_allocations) + " times");
}

void the_tail_bound_covers_a_released_note() {
  REQUIRE_EQ(install().state, library::State::ready, "the test library loads");
  auto v = default_values(*fret::engine().descriptor);
  v[at("fret.release")] = 1.0;
  const auto tail = fret::engine().tail_frames(v, rate);
  auto runtime = make_runtime(fret::engine(), v);
  const auto off = frames_of(0.1);
  const auto x = mono(render(*runtime, note(0, off, e1, 1, loud), off + tail + frames_of(0.1)));
  REQUIRE_LT(rms(x, off + tail, frames_of(0.1)), 1e-6, "silent after the tail bound");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "fret engine contract passed",
      {
          {"a_missing_library_is_silent", [] { a_missing_library_is_silent(); }},
          {"malformed_libraries_are_rejected", [] { malformed_libraries_are_rejected(); }},
          {"descriptor_registry_and_presets", [] { descriptor_registry_and_presets(); }},
          {"keys_are_in_tune", [] { keys_are_in_tune(); }},
          {"velocity_layers_and_round_robins", [] { velocity_layers_and_round_robins(); }},
          {"feel_picks_articulations", [] { feel_picks_articulations(); }},
          {"forced_articulation_plays_every_note", [] { forced_articulation_plays_every_note(); }},
          {"pocket_plays_off_beats_late", [] { pocket_plays_off_beats_late(); }},
          {"slides_glide_to_the_next_note", [] { slides_glide_to_the_next_note(); }},
          {"releases_sound_at_note_off", [] { releases_sound_at_note_off(); }},
          {"streaming_block_splits_and_resets_are_deterministic",
           [] { streaming_block_splits_and_resets_are_deterministic(); }},
          {"the_callback_does_not_allocate", [] { the_callback_does_not_allocate(); }},
          {"the_tail_bound_covers_a_released_note",
           [] { the_tail_bound_covers_a_released_note(); }},
      });
}
