#include "concert_test_library.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/concert.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/signal.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/tolerances.hpp"
#include "synth_test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

// Concert's engine contract (ADR-0071), on the synthetic test library: a
// missing or malformed library, the registry and presets, tuning through
// the resampler, velocity layers without steps, streaming past the
// resident start, the pedals (sustain with half-pedal, sostenuto, una
// corda, the mechanism's releases), block splits, allocation, the tail
// bound, sane presets, telemetry and goldens.
namespace {
using namespace synth_test;
using oscillate::project::MidiPedal;
namespace concert = oscillate::native_dsp::synth::concert;
namespace library = concert::library;

const SynthEngine& concert_engine() { return concert::engine(); }
const EngineDescriptor& table() { return *concert_engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }
std::vector<double> patch(std::string_view name) { return preset(table(), name); }

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr std::uint8_t loudest = 127;
const double headroom = std::pow(10.0, concert::headroom_db / 20.0);

// Installs the streamed test library once per process.
void use_test_library() {
  static const auto directory = concert_test::install();
  (void)directory;
}

double rms(const std::vector<double>& x, std::size_t from, std::size_t to) {
  double sum = 0.0;
  for (std::size_t i = from; i < std::min(to, x.size()); ++i) {
    sum += x[i] * x[i];
  }
  return std::sqrt(sum / static_cast<double>(std::min(to, x.size()) - from));
}

double db(double x) { return oscillate::test::gain_to_db(x); }

// Hann-windowed DFT magnitude at one frequency, and the peak near a guess.
double magnitude(const std::vector<double>& x, std::size_t from, std::size_t length, double hz) {
  double re = 0.0;
  double im = 0.0;
  for (std::size_t i = 0; i < length; ++i) {
    const double window =
        oscillate::native_dsp::dsp::hann(static_cast<double>(i), static_cast<double>(length - 1));
    const double t = 2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate;
    re += x[from + i] * window * std::cos(t);
    im += x[from + i] * window * std::sin(t);
  }
  return std::hypot(re, im);
}

double peak_hz(const std::vector<double>& x, std::size_t from, std::size_t length, double guess) {
  double lo = guess * 0.985;
  double hi = guess * 1.015;
  for (int i = 0; i < 60; ++i) {
    const double a = lo + 0.382 * (hi - lo);
    const double b = lo + 0.618 * (hi - lo);
    if (magnitude(x, from, length, a) > magnitude(x, from, length, b)) {
      hi = b;
    } else {
      lo = a;
    }
  }
  return 0.5 * (lo + hi);
}

// Without the library Concert loads nothing, says why, and plays silence.
// Runs first: nothing in this process has loaded a library yet.
void a_missing_library_is_silent() {
  // A test-owned directory; the override names a child that never exists.
  const oscillate::test::TempDir owned("oscillate-concert-test");
  const auto nowhere = owned / "no-library-here";
  ::setenv(std::string(library::directory_variable).c_str(), nowhere.c_str(), 1);
  REQUIRE_EQ(library::default_directory(), nowhere, "the directory follows the override");
  const auto status = library::status();
  REQUIRE_EQ(status.state, library::State::missing, "a missing library is reported with a reason");
  REQUIRE_TRUE(!status.message.empty(), "a missing library is reported with a reason");
  REQUIRE_TRUE(status.ring_seconds.empty(), "a missing library is reported with a reason");
  auto runtime = make_runtime(concert_engine(), patch("Concert Grand"));
  auto events = note(0, 9'600, 60, 1);
  auto chord = note(100, 9'600, 64, 2);
  events.insert(events.end(), chord.begin(), chord.end());
  const auto out = render(*runtime, events, 24'000, 128, rate, 120.0,
                          {{0, MidiPedal::sustain, 1.0}, {12'000, MidiPedal::sustain, 0.0}});
  REQUIRE_EQ(peak(out.left), 0.0, "without its library Concert is silent");
  REQUIRE_EQ(peak(out.right), 0.0, "without its library Concert is silent");
  std::vector<float> frame;
  REQUIRE_TRUE(runtime->read_telemetry(frame), "telemetry says the library is missing");
  REQUIRE_EQ(frame.size(), concert::telemetry::size, "telemetry says the library is missing");
  REQUIRE_EQ(frame[concert::telemetry::library],
             static_cast<float>(static_cast<int>(library::State::missing)),
             "telemetry says the library is missing");
  const auto tail = concert_engine().tail_frames(patch("Concert Grand"), rate);
  REQUIRE_GT(tail, 0, "the tail is still bounded");
  REQUIRE_LT(tail, static_cast<std::uint64_t>(60.0 * rate), "the tail is still bounded");
}

// The manifest and pack are untrusted: anything inconsistent is refused
// with a reason, and nothing is installed.
void malformed_libraries_are_rejected() {
  const auto base = concert_test::scratch_directory("concert-bad-library");
  const auto rewrite = [&](auto edit) {
    concert_test::write_library(base, concert_test::resident_note_seconds);
    edit(base);
    return library::load(base);
  };
  const auto manifest = [](const std::filesystem::path& dir) {
    return dir / library::manifest_file;
  };
  const auto replace_line = [&](const std::filesystem::path& dir, std::string_view starts,
                                const std::string& with) {
    std::ifstream in(manifest(dir));
    std::string text;
    std::string line;
    while (std::getline(in, line)) {
      if (line.starts_with(starts)) {
        if (!with.empty()) {
          text += with + "\n";
        }
        continue;
      }
      text += line + "\n";
    }
    in.close();
    std::ofstream(manifest(dir)) << text;
  };
  const auto refused = [](const library::Status& status, std::string_view why) {
    require(status.state == library::State::invalid && !status.message.empty(),
            std::string(why) + " is refused");
  };
  refused(rewrite([&](const auto& dir) { replace_line(dir, "oscillate", "a different format"); }),
          "another format");
  refused(rewrite([&](const auto& dir) {
            std::filesystem::resize_file(dir / "concert.pack",
                                         std::filesystem::file_size(dir / "concert.pack") - 4);
          }),
          "a truncated pack");
  refused(rewrite([&](const auto& dir) {
            replace_line(dir, "sample note 21 22 21 1 40 0 ",
                         "sample note 21 22 21 1 40 999999999 100 3.1e-05 -16.5 0 0 0 x");
          }),
          "a sample outside its pack");
  refused(rewrite([&](const auto& dir) { replace_line(dir, "sample note 21 22 21", ""); }),
          "a key without a note");
  refused(rewrite([&](const auto& dir) { replace_line(dir, "rate", "rate 44100"); }),
          "another sample rate");
  refused(rewrite([&](const auto& dir) { replace_line(dir, "pack", "pack ../concert.pack 4"); }),
          "a pack outside the library");
  const auto none = library::load(base / "nothing");
  REQUIRE_EQ(none.state, library::State::missing, "an empty directory is missing, not invalid");
}

void descriptor_registry_and_presets() {
  REQUIRE_EQ(find_engine("concert"), &concert_engine(), "Concert is registered");
  require(concert_engine().models_pedals, "Concert models its own pedals");
  REQUIRE_OK(validate_engine(table()), "the table validates");
  REQUIRE_EQ(table().presets.size(), 7, "seven presets, Concert Grand first");
  REQUIRE_EQ(table().presets.front().name, "Concert Grand", "seven presets, Concert Grand first");
  for (const auto& entry : table().presets) {
    REQUIRE_OK(preset_values(table(), entry), "preset resolves");
  }
  for (const auto& parameter : table().parameters) {
    require(parameter.id.starts_with("concert."), "ids are Concert's own");
  }
}

// Keys between the sampled roots are resampled exactly in tune: up and
// down a semitone, and with Tune, the fundamental lands within half a cent,
// and the waveform matches the source read at the new rate.
void keys_are_in_tune() {
  use_test_library();
  struct Case {
    std::uint8_t key;
    double tune;
  };
  for (const Case c :
       {Case{60, 0.0}, Case{61, 0.0}, Case{59, 0.0}, Case{60, 50.0}, Case{86, -50.0}}) {
    auto values = patch("Dry");
    values[at("concert.tune")] = c.tune;
    auto runtime = make_runtime(concert_engine(), values);
    const auto out = render(*runtime, note(0, 50'000, c.key, 1, loudest), 50'000);
    const int root = concert_test::root_of(c.key);
    const double semitones = static_cast<double>(c.key - root) + c.tune / 100.0;
    const double step = std::exp2(semitones / 12.0);
    const double target = concert_test::root_hz(root) * step;
    const double measured = peak_hz(out.left, 4'800, 38'400, target);
    REQUIRE_LT(std::fabs(1'200.0 * std::log2(measured / target)), 0.5,
               "key " + std::to_string(c.key) + " is in tune");
    // The band-limited source read at the new rate: the error is the
    // source's int16 rounding and the interpolator's, far below the tone.
    const double amplitude = concert_test::layers.back().amplitude;
    double error = 0.0;
    double signal = 0.0;
    for (std::size_t i = 2'000; i < 40'000; ++i) {
      const double expected =
          headroom * concert_test::tone_at(root, amplitude, static_cast<double>(i) * step);
      error += (out.left[i] - expected) * (out.left[i] - expected);
      signal += expected * expected;
    }
    REQUIRE_LT(db(std::sqrt(error / signal)), -70.0,
               "key " + std::to_string(c.key) + " is the source resampled cleanly");
  }
}

// Every velocity sounds a blend of the two layers around it, so the level
// rises with velocity without a step at any layer boundary.
void layers_crossfade_without_steps() {
  use_test_library();
  std::vector<double> level;
  for (int velocity = 1; velocity <= 127; ++velocity) {
    auto runtime = make_runtime(concert_engine(), patch("Dry"));
    const auto out =
        render(*runtime, note(0, 4'800, 60, 1, static_cast<std::uint8_t>(velocity)), 4'800);
    level.push_back(db(rms(out.left, 480, 4'800)));
  }
  for (std::size_t v = 1; v < level.size(); ++v) {
    const double rise = level[v] - level[v - 1];
    REQUIRE_GT(rise, -1e-6, "louder blows are never quieter");
    REQUIRE_LT(rise, 0.6,
               "no step between velocities " + std::to_string(v) + " and " + std::to_string(v + 1));
  }
  const double layer_span =
      db(concert_test::layers.back().amplitude / concert_test::layers.front().amplitude);
  REQUIRE_LT(std::fabs((level.back() - level.front()) - layer_span), 0.5,
             "the full range spans the layers' levels");
}

// The first frames come from memory and the rest stream from disk; across
// the join and deep into the stream the output is the sample itself.
void notes_stream_past_their_resident_start() {
  use_test_library();
  auto runtime = make_runtime(concert_engine(), patch("Dry"));
  const std::size_t frames = 56'000;
  const auto out = render(*runtime, note(0, frames, 60, 1, loudest), frames);
  const double amplitude = concert_test::layers.back().amplitude;
  for (std::size_t i = 0; i < frames; ++i) {
    const double source =
        static_cast<double>(concert_test::quantise(concert_test::tone(60, amplitude, i))) /
        concert_test::full_scale;
    const double right = static_cast<double>(concert_test::quantise(
                             concert_test::right_share * concert_test::tone(60, amplitude, i))) /
                         concert_test::full_scale;
    require(std::fabs(out.left[i] - headroom * source) < tolerance::float_round_trip &&
                std::fabs(out.right[i] - headroom * right) < tolerance::float_round_trip,
            "frame " + std::to_string(i) + " is the sample's own");
  }
  REQUIRE_LT(library::resident_frames, frames, "the render reaches the streamed part");
}

// The sustain pedal holds a released note; without it the dampers stop it;
// half-pedal sits between. Sostenuto holds only the note that was down when
// it went down. The key's release sounds at the key, the string release
// when the damper lands.
void pedals_hold_and_release() {
  use_test_library();
  const auto values = patch("Dry");
  const auto tail_level = [&](const std::vector<double>& patch_values,
                              std::vector<PedalEvent> pedals, std::vector<Event> events,
                              std::size_t from, std::size_t to) {
    auto runtime = make_runtime(concert_engine(), patch_values);
    const auto out = render(*runtime, std::move(events), to, 128, rate, 120.0, std::move(pedals));
    return rms(out.left, from, to);
  };
  const auto released = tail_level(values, {}, note(0, 4'800, 60, 1), 30'000, 40'000);
  const auto held =
      tail_level(values, {{0, MidiPedal::sustain, 1.0}}, note(0, 4'800, 60, 1), 30'000, 40'000);
  REQUIRE_GT(held, 30.0 * released, "the sustain pedal holds a released note");
  const auto soon = [&](double pedal) {
    return tail_level(values,
                      pedal > 0.0 ? std::vector<PedalEvent>{{0, MidiPedal::sustain, pedal}}
                                  : std::vector<PedalEvent>{},
                      note(0, 4'800, 60, 1), 9'600, 14'400);
  };
  const double up = soon(0.0);
  const double half = soon(0.5);
  const double down = soon(1.0);
  REQUIRE_GT(half, 1.5 * up, "half pedal sits between");
  REQUIRE_LT(half, down, "half pedal sits between");

  // Sostenuto: C3 is down when it goes down, E4 is struck after.
  auto c3 = note(0, 4'800, 48, 1);
  const auto e4 = note(2'400, 2'400, 64, 2);
  c3.insert(c3.end(), e4.begin(), e4.end());
  auto runtime = make_runtime(concert_engine(), values);
  const auto out =
      render(*runtime, c3, 40'000, 128, rate, 120.0, {{1'200, MidiPedal::sostenuto, 1.0}});
  const auto amplitude_at = [&](double hz) { return magnitude(out.left, 28'000, 10'000, hz); };
  REQUIRE_GT(amplitude_at(concert_test::root_hz(48)),
             30.0 * amplitude_at(concert_test::root_hz(64)),
             "sostenuto holds the note that was down, not the one struck after");

  // The mechanism: with the pedal down, the key's release sounds when the
  // key rises and the string release only when the pedal lets the damper
  // fall.
  constexpr std::size_t key_up = 9'600;
  constexpr std::size_t pedal_up = 24'000;
  constexpr std::size_t length = 36'000;
  const auto difference = [&](std::string_view noise) {
    const auto& quiet = values;
    auto loud = values;
    loud[at(noise)] = 1.0;
    const std::vector<PedalEvent> pedals{{0, MidiPedal::sustain, 1.0},
                                         {pedal_up, MidiPedal::sustain, 0.0}};
    auto a = make_runtime(concert_engine(), quiet);
    auto b = make_runtime(concert_engine(), loud);
    const auto x = render(*a, note(0, key_up, 60, 1), length, 128, rate, 120.0, pedals);
    const auto y = render(*b, note(0, key_up, 60, 1), length, 128, rate, 120.0, pedals);
    std::vector<double> d(length);
    for (std::size_t i = 0; i < length; ++i) {
      d[i] = y.left[i] - x.left[i];
    }
    return d;
  };
  const auto key_release = difference("concert.noise.release");
  REQUIRE_EQ(peak(key_release, 0, key_up), 0.0,
             "the key's release sounds as the key rises, pedal or not");
  REQUIRE_GT(peak(key_release, key_up, pedal_up), 1e-5,
             "the key's release sounds as the key rises, pedal or not");
  const auto string_release = difference("concert.noise.strings");
  REQUIRE_EQ(peak(string_release, 0, pedal_up), 0.0,
             "the string release sounds when the damper lands");
  REQUIRE_GT(peak(string_release, pedal_up), 1e-5,
             "the string release sounds when the damper lands");
}

// Una corda: the soft pedal lowers the layers, softens and darkens a blow.
void una_corda_softens() {
  use_test_library();
  const auto values = patch("Dry");
  auto plain = make_runtime(concert_engine(), values);
  auto soft = make_runtime(concert_engine(), values);
  const auto a = render(*plain, note(0, 24'000, 72, 1), 24'000);
  const auto b =
      render(*soft, note(0, 24'000, 72, 1), 24'000, 128, rate, 120.0, {{0, MidiPedal::soft, 1.0}});
  REQUIRE_LT(rms(b.left, 0, 24'000), rms(a.left, 0, 24'000), "una corda is quieter");
  const double hz = concert_test::root_hz(72);
  const auto brightness = [&](const std::vector<double>& x) {
    return magnitude(x, 2'400, 16'384, 4.0 * hz) / magnitude(x, 2'400, 16'384, hz);
  };
  REQUIRE_LT(brightness(b.left), brightness(a.left), "una corda is darker");
}

void any_block_split_renders_identically() {
  use_test_library();
  for (std::string_view name : {"Concert Grand", "Felt", "Audience"}) {
    auto a = make_runtime(concert_engine(), patch(name));
    auto b = make_runtime(concert_engine(), patch(name));
    auto events = note(0, 20'000, 48, 1);
    auto more = note(7'000, 20'000, 60, 2, 70);
    auto again = note(9'000, 6'000, 60, 3, 110);
    events.insert(events.end(), more.begin(), more.end());
    events.insert(events.end(), again.begin(), again.end());
    const std::vector<PedalEvent> pedals{{3'000, MidiPedal::sustain, 1.0},
                                         {12'000, MidiPedal::soft, 1.0},
                                         {30'000, MidiPedal::sustain, 0.0}};
    const auto x = render(*a, events, 40'000, 64, rate, 120.0, pedals);
    const auto y = render(*b, events, 40'000, 0, rate, 120.0, pedals);
    require(x.left == y.left && x.right == y.right,
            std::string(name) + ": block splits render identically");
  }
}

void full_load_does_not_allocate() {
  use_test_library();
  auto runtime = make_runtime(concert_engine(), patch("Concert Grand"));
  std::vector<engine::StereoFrame> buffer(128);
  std::vector<engine::BlockMidiEvent> chord;
  for (std::uint8_t key : {std::uint8_t{36}, std::uint8_t{48}, std::uint8_t{60}, std::uint8_t{64},
                           std::uint8_t{67}, std::uint8_t{96}}) {
    engine::BlockMidiEvent event;
    event.clip_id_bits = {1, 1};
    event.note_id_bits = {0, key};
    event.kind = engine::MidiEventKind::note_on;
    event.key = key;
    event.velocity = 110;
    chord.push_back(event);
  }
  const std::vector<engine::BlockPedalEvent> down{{0, MidiPedal::sustain, 1.0F}};
  std::vector<double> lane(128, 2.0);
  const std::vector<ParameterLane> lanes{{static_cast<std::uint32_t>(at("concert.tone")), lane}};
  counting_allocations = true;
  counted_allocations = 0;
  for (std::uint64_t block = 0; block < 400; ++block) {
    (void)runtime->process(buffer, block == 0 ? std::span(chord) : std::span(chord).subspan(0, 0),
                           block == 0 ? std::span(down) : std::span(down).subspan(0, 0),
                           block * 128, 1, {}, lanes);
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "the Concert callback never allocates");
}

void tail_bound_covers_release() {
  use_test_library();
  for (std::string_view name : {"Concert Grand", "Close", "Felt"}) {
    const auto values = patch(name);
    auto runtime = make_runtime(concert_engine(), values);
    const auto tail = concert_engine().tail_frames(values, rate);
    const auto out = render(*runtime, note(0, 4'800, 60, 1), 4'800 + tail + 9'600);
    require(peak(out.left, 4'800 + tail) < 1e-3 && peak(out.right, 4'800 + tail) < 1e-3,
            std::string(name) + ": output settles inside the tail bound");
  }
}

void presets_render_sane_audio() {
  use_test_library();
  for (const auto& entry : table().presets) {
    auto runtime = make_runtime(concert_engine(), patch(entry.name));
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
    std::cout << "  " << entry.name << ": rms " << rms_db << " dBFS, peak "
              << db(std::max(peak(out.left), peak(out.right))) << " dBFS\n";
    REQUIRE_LE(std::max(peak(out.left), peak(out.right)), 1.0, "preset never exceeds 0 dBFS");
    REQUIRE_GT(rms_db, -40.0, "preset is audible and not slammed");
    REQUIRE_LT(rms_db, -6.0, "preset is audible and not slammed");
    REQUIRE_LT(std::fabs(sum / (2.0 * 48'000.0)), 0.01, "preset has no DC offset");
  }
}

void telemetry_reports_keys_and_pedals() {
  use_test_library();
  auto runtime = make_runtime(concert_engine(), patch("Concert Grand"));
  (void)render(*runtime, note(0, 24'000, 60, 1), 12'000, 128, rate, 120.0,
               {{0, MidiPedal::sustain, 1.0}});
  std::vector<float> frame;
  REQUIRE_TRUE(runtime->read_telemetry(frame), "a telemetry frame is published");
  REQUIRE_EQ(frame.size(), concert::telemetry::size, "a telemetry frame is published");
  namespace t = concert::telemetry;
  REQUIRE_EQ(frame[t::sustain], 1.0F, "the sustain pedal is reported down");
  const auto c4 = t::first_energy + static_cast<std::size_t>(60 - concert::first_key);
  const auto c2 = t::first_energy + static_cast<std::size_t>(36 - concert::first_key);
  REQUIRE_GT(frame[c4], frame[c2], "C4 rings strongest");
  REQUIRE_GT(frame[c4], -6.0F, "C4 rings strongest");
  REQUIRE_EQ(frame[t::first_damper + static_cast<std::size_t>(60 - concert::first_key)], 0.0F,
             "C4's damper is up");
  REQUIRE_EQ(frame[t::library], static_cast<float>(static_cast<int>(library::State::ready)),
             "the library is reported ready");
  REQUIRE_EQ(frame[t::underruns], 0.0F, "an offline render never waits on the disk");
}

void presets_match_golden_fingerprints() {
  use_test_library();
  struct Golden {
    std::string_view name;
    Fingerprint expected;
  };
  // Taken on the test library; regenerate with CONCERT_REBASELINE=1.
  const std::array<Golden, 7> golden{{
      {"Concert Grand", {-20.115, -103.679, 438.1}},
      {"Bright", {-20.390, -104.866, 461.5}},
      {"Soft", {-20.234, -104.051, 428.5}},
      {"Close", {-19.924, -99.279, 438.1}},
      {"Felt", {-15.354, -99.972, 407.6}},
      {"Audience", {-20.138, -104.976, 438.1}},
      {"Dry", {-20.595, -300.000, 438.2}},
  }};
  const bool rebaseline = std::getenv("CONCERT_REBASELINE") != nullptr;
  for (const auto& entry : golden) {
    const auto actual = fingerprint(concert_engine(), entry.name);
    if (rebaseline) {
      std::printf("      {\"%s\", {%.3f, %.3f, %.1f}},\n", std::string(entry.name).c_str(),
                  actual.hold_db, actual.tail_db, actual.centroid_hz);
      continue;
    }
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

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "concert engine contract passed",
      {
          {"a_missing_library_is_silent", [] { a_missing_library_is_silent(); }},
          {"malformed_libraries_are_rejected", [] { malformed_libraries_are_rejected(); }},
          {"descriptor_registry_and_presets", [] { descriptor_registry_and_presets(); }},
          {"keys_are_in_tune", [] { keys_are_in_tune(); }},
          {"layers_crossfade_without_steps", [] { layers_crossfade_without_steps(); }},
          {"notes_stream_past_their_resident_start",
           [] { notes_stream_past_their_resident_start(); }},
          {"pedals_hold_and_release", [] { pedals_hold_and_release(); }},
          {"una_corda_softens", [] { una_corda_softens(); }},
          {"any_block_split_renders_identically", [] { any_block_split_renders_identically(); }},
          {"full_load_does_not_allocate", [] { full_load_does_not_allocate(); }},
          {"tail_bound_covers_release", [] { tail_bound_covers_release(); }},
          {"presets_render_sane_audio", [] { presets_render_sane_audio(); }},
          {"telemetry_reports_keys_and_pedals", [] { telemetry_reports_keys_and_pedals(); }},
          {"presets_match_golden_fingerprints", [] { presets_match_golden_fingerprints(); }},
      });
}
