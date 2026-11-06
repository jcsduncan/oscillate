#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/wren.hpp"
#include "oscillate/native_dsp/synth/wren_breath.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "synth_test_support.hpp"
#include "wren_test_library.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <numbers>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

// Wren's engine contract (ADR-0089), on a synthetic library whose takes,
// keys and attacks sound on different harmonics (wren_test_library.hpp):
// a missing or malformed library is silent and says why; keys play in tune
// at any session rate, missing cells from their nearest neighbour; velocity
// picks the layer and trims within it; a slur starts past the attack and
// crossfades, a tongued note plays its attack; vibrato blooms after its
// delay; loops hold a note beyond its recording; releases end on time;
// graces take the project key's step; block splits and resets render the
// same; the callback never allocates; the tail bound covers the hall.
namespace {

using namespace oscillate;
using namespace oscillate::native_dsp::synth;
using namespace synth_test;
namespace library = wren::library;
namespace breath = wren::breath;

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr double bpm = 120.0;
constexpr double seconds_per_minute = 60.0;
constexpr double seconds_per_beat = seconds_per_minute / bpm;
constexpr double decibels = 20.0;
constexpr std::uint8_t c4 = 60;
constexpr std::uint8_t c_sharp4 = 61;
constexpr std::uint8_t a_sharp4 = 70;
constexpr std::uint8_t a4 = 69;
constexpr std::uint8_t b4 = 71;
constexpr std::uint8_t c5 = 72;
constexpr std::uint8_t d5 = 74;
constexpr std::uint8_t e5 = 76;
constexpr std::uint8_t mezzo = 80;
constexpr std::uint8_t loud = 110;
constexpr std::size_t block = 128;
// A reset fades the last output out over 1.5 ms; compare past it.
constexpr std::size_t declick_frames = 128;
// Analysis windows.
constexpr std::size_t window = 2'048;
constexpr std::size_t short_window = 96; // 2 ms.
// Below this a signal is silence.
constexpr double silence = 1e-6;
constexpr double audible = 1e-3;

const ParameterDescriptor& parameter(std::string_view id) {
  for (const auto& p : wren::engine().descriptor->parameters) {
    if (p.id == id) {
      return p;
    }
  }
  require(false, std::string("unknown parameter ") + std::string(id));
  return wren::engine().descriptor->parameters.front();
}

std::size_t at(std::string_view id) {
  const auto& all = wren::engine().descriptor->parameters;
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

// The notes as drawn, dry.
std::vector<double> plain() {
  auto v = default_values(*wren::engine().descriptor);
  v[at("wren.breath.style")] = choice_of("wren.breath.style", "As drawn");
  v[at("wren.tone.space")] = 0.0;
  // Unity overall: the library makeup would push loud takes into the safety
  // knee, which level measurements must stay below.
  v[at("wren.level")] = -wren::headroom_db;
  return v;
}

// Lyrical, dry, without swell or ornaments: Breath's vibrato alone.
std::vector<double> lyrical_dry() {
  auto v = default_values(*wren::engine().descriptor);
  v[at("wren.tone.space")] = 0.0;
  v[at("wren.breath.swell")] = 0.0;
  v[at("wren.breath.ornaments")] = choice_of("wren.breath.ornaments", "Off");
  return v;
}

void install() { (void)wren_test::install(); }

std::size_t frames_of(double seconds) { return static_cast<std::size_t>(seconds * rate); }
std::uint64_t at_beat(double beat) {
  return static_cast<std::uint64_t>(std::llround(beat * seconds_per_beat * rate));
}

// A note from a clip: its length is known when it starts, as Breath needs.
std::vector<Event> clip_note(std::uint64_t start, std::uint64_t length, std::uint8_t key,
                             std::uint64_t id, std::uint8_t velocity = loud) {
  auto events = note(start, length, key, id, velocity);
  events.front().frames = static_cast<std::uint32_t>(length);
  return events;
}

void append(std::vector<Event>& events, const std::vector<Event>& more) {
  events.insert(events.end(), more.begin(), more.end());
}

std::vector<double> mono(const Rendered& r) {
  std::vector<double> out(r.left.size());
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = 0.5 * (r.left[i] + r.right[i]);
  }
  return out;
}

double key_hz(int key) { return wren_test::key_hz(key); }

double magnitude(const std::vector<double>& x, std::size_t from, std::size_t length, double hz) {
  double re = 0.0;
  double im = 0.0;
  for (std::size_t i = 0; i < length && from + i < x.size(); ++i) {
    const double hann =
        oscillate::native_dsp::dsp::hann(static_cast<double>(i), static_cast<double>(length - 1));
    const double t = 2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate;
    re += x[from + i] * hann * std::cos(t);
    im += x[from + i] * hann * std::sin(t);
  }
  return std::sqrt(re * re + im * im);
}

// The frequency near `hz` with the most energy, by golden-section search.
double pitch_near(const std::vector<double>& x, std::size_t from, std::size_t length, double hz) {
  constexpr double span = 0.03; // ±3 % around the guess.
  constexpr int iterations = 60;
  const double golden = (std::sqrt(5.0) - 1.0) / 2.0;
  double lo = hz * (1.0 - span);
  double hi = hz * (1.0 + span);
  for (int i = 0; i < iterations; ++i) {
    const double a = hi - golden * (hi - lo);
    const double b = lo + golden * (hi - lo);
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

double db(double ratio) { return decibels * std::log10(ratio); }

double max_step(const std::vector<double>& x, std::size_t from, std::size_t to) {
  double most = 0.0;
  for (std::size_t i = std::max<std::size_t>(from, 1); i < to && i < x.size(); ++i) {
    most = std::max(most, std::fabs(x[i] - x[i - 1]));
  }
  return most;
}

std::vector<float> telemetry_of(NativeSynthRuntime& runtime) {
  std::vector<float> out;
  REQUIRE_TRUE(runtime.read_telemetry(out), "telemetry arrives");
  REQUIRE_GE(out.size(), wren::telemetry::size, "telemetry arrives");
  return out;
}

// ---- Library ----------------------------------------------------------------------------

void a_missing_library_is_silent() {
  const auto nowhere = wren_test::scratch_directory("wren-nothing-here");
  ::setenv(std::string(library::directory_variable).c_str(), nowhere.c_str(), 1);
  const auto status = library::status();
  REQUIRE_EQ(status.state, library::State::missing, "without the library Wren says why");
  REQUIRE_TRUE(!status.message.empty(), "without the library Wren says why");
  auto runtime = make_runtime(wren::engine(), plain());
  const auto r = mono(render(*runtime, note(0, frames_of(0.2), d5, 1, loud), frames_of(0.3)));
  REQUIRE_EQ(rms(r, 0, r.size()), 0.0, "and is silent");
  REQUIRE_EQ(telemetry_of(*runtime)[wren::telemetry::library],
             static_cast<float>(library::State::missing), "telemetry says the library is missing");
}

// The manifest with one field of its first sample line replaced.
std::string with_field(const std::string& text, std::size_t field, const std::string& value) {
  constexpr std::string_view tag = "sample ";
  const auto start = text.find(tag);
  const auto end = text.find('\n', start);
  std::istringstream fields(text.substr(start + tag.size(), end - start - tag.size()));
  std::vector<std::string> parts{std::istream_iterator<std::string>(fields), {}};
  parts[field] = value;
  std::string line(tag);
  for (std::size_t i = 0; i < parts.size(); ++i) {
    line += (i == 0 ? "" : " ") + parts[i];
  }
  return text.substr(0, start) + line + text.substr(end);
}

void malformed_libraries_are_rejected() {
  const auto base = wren_test::scratch_directory("wren-malformed");
  wren_test::write_library(base);
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
  REQUIRE_EQ(broken("not a wren library\n" + text), library::State::invalid, "format line");
  std::string channels = text;
  const std::string mono_line = "channels 1";
  channels.replace(channels.find(mono_line), mono_line.size(), "channels 2");
  REQUIRE_EQ(broken(channels), library::State::invalid, "stereo packs are not Wren's");
  // Fields of a sample line, after its tag.
  constexpr std::size_t key_field = 1;
  constexpr std::size_t frames_field = 4;
  constexpr std::size_t attack_field = 5;
  constexpr std::size_t loop_start_field = 6;
  constexpr std::size_t loop_end_field = 7;
  constexpr std::size_t cents_field = 8;
  const std::string off_key_cents = "150"; // Over a semitone.
  const std::string no_frames = "0";
  const auto frames = std::to_string(wren_test::note_frames);
  REQUIRE_EQ(broken(with_field(text, key_field, std::to_string(wren::last_key + 1))),
             library::State::invalid, "a key outside B3..D-flat 7");
  REQUIRE_EQ(broken(with_field(text, loop_end_field, std::to_string(wren_test::note_frames + 1))),
             library::State::invalid, "a loop past the sample's end");
  REQUIRE_EQ(broken(with_field(text, loop_start_field, std::to_string(wren_test::loop_end))),
             library::State::invalid, "an empty loop");
  REQUIRE_EQ(broken(with_field(text, attack_field, frames)), library::State::invalid,
             "an attack end past the sample");
  REQUIRE_EQ(broken(with_field(text, frames_field, no_frames)), library::State::invalid,
             "an empty sample");
  REQUIRE_EQ(broken(with_field(text, cents_field, off_key_cents)), library::State::invalid,
             "a recording over a semitone off its key");
  const auto first_sample = text.substr(
      text.find("sample "), text.find('\n', text.find("sample ")) - text.find("sample ") + 1);
  REQUIRE_EQ(broken(text + first_sample), library::State::invalid, "a sample listed twice");
  std::ofstream(manifest) << text;
  REQUIRE_EQ(library::load(base).state, library::State::ready, "the untouched manifest loads");
  std::filesystem::resize_file(base / "wren.pack",
                               std::filesystem::file_size(base / "wren.pack") / 2);
  REQUIRE_EQ(library::load(base).state, library::State::invalid, "a truncated pack");
}

void descriptor_registry_and_presets() {
  constexpr std::size_t parameters = 16;
  constexpr std::size_t presets = 6;
  const auto& descriptor = *wren::engine().descriptor;
  REQUIRE_EQ(find_engine("wren"), &wren::engine(), "Wren is registered");
  REQUIRE_OK(validate_engine(descriptor), "the table validates");
  REQUIRE_EQ(descriptor.parameters.size(), parameters, "the ADR's sixteen parameters");
  REQUIRE_EQ(descriptor.presets.size(), presets, "six presets, Lyrical Air the defaults");
  REQUIRE_EQ(descriptor.presets.front().name, "Lyrical Air",
             "six presets, Lyrical Air the defaults");
  REQUIRE_TRUE(descriptor.presets.front().values.empty(), "six presets, Lyrical Air the defaults");
  for (const auto& entry : descriptor.presets) {
    REQUIRE_OK(preset_values(descriptor, entry), "every preset resolves");
  }
  require(wren::engine().models_pedals, "a flute ignores the pedals");
  const auto settings = wren::breath_settings(default_values(descriptor));
  REQUIRE_EQ(settings.style, breath::Style::lyrical,
             "Breath's defaults are Lyrical, Ornaments Some");
  REQUIRE_EQ(settings.ornaments, breath::Ornaments::some,
             "Breath's defaults are Lyrical, Ornaments Some");
}

// ---- Pitch, layers and fallback --------------------------------------------------------

void keys_are_in_tune() {
  install();
  constexpr double max_cents = 1.0;
  constexpr double note_seconds = 0.5;
  constexpr double from_seconds = 0.1; // Past the attack.
  constexpr double length_seconds = 0.3;
  for (const double session : {44'100.0, 48'000.0, 96'000.0}) {
    for (const auto key : {static_cast<std::uint8_t>(wren::first_key), c_sharp4, a4,
                           static_cast<std::uint8_t>(wren::last_key)}) {
      auto runtime = make_runtime(wren::engine(), plain(), session);
      const auto length = static_cast<std::uint64_t>(note_seconds * session);
      const auto x = mono(render(*runtime, note(0, length, key, 1, mezzo), length, block, session));
      const double want = key_hz(key);
      const auto from = static_cast<std::size_t>(from_seconds * session);
      const auto span = static_cast<std::size_t>(length_seconds * session);
      // magnitude() assumes 48 kHz: scale the measured frequency back.
      const double got = pitch_near(x, from, span, want * rate / session) * session / rate;
      REQUIRE_LT(std::fabs(wren_test::cents_per_octave * std::log2(got / want)), max_cents,
                 "key " + std::to_string(key) + " in tune at " + std::to_string(session) + " Hz");
    }
  }
}

// The steady level of a drawn note (dB).
double level_db(std::uint8_t key, std::uint8_t velocity) {
  constexpr double note_seconds = 0.4;
  constexpr double from_seconds = 0.1;
  constexpr double length_seconds = 0.2;
  auto runtime = make_runtime(wren::engine(), plain());
  const auto x = mono(render(*runtime, note(0, frames_of(note_seconds), key, 1, velocity),
                             frames_of(note_seconds)));
  return db(rms(x, frames_of(from_seconds), frames_of(length_seconds)));
}

void velocity_picks_layers_and_trims() {
  install();
  // The ADR: pp below 60, mf below 100, ff from 100; within a layer the
  // level trims by up to 6 dB at its bottom.
  constexpr std::array<int, 4> floors{0, 60, 100, 127};
  constexpr double trim_db = -6.0;
  constexpr double tolerance_db = 0.05;
  const auto expected_db = [&](int velocity) {
    std::size_t layer = 0;
    while (layer + 1 < wren::layer_count && velocity >= floors[layer + 1]) {
      ++layer;
    }
    const double within = static_cast<double>(velocity - floors[layer]) /
                          static_cast<double>(floors[layer + 1] - floors[layer]);
    return wren_test::layer_db[layer] + trim_db * (1.0 - within);
  };
  constexpr std::uint8_t reference = 127;
  const double top = level_db(a4, reference);
  for (const std::uint8_t velocity : {std::uint8_t{30}, std::uint8_t{59}, std::uint8_t{60},
                                      std::uint8_t{80}, std::uint8_t{99}, std::uint8_t{100}}) {
    const double got = level_db(a4, velocity) - top;
    const double want = expected_db(velocity) - expected_db(reference);
    REQUIRE_LT(std::fabs(got - want), tolerance_db,
               "velocity " + std::to_string(velocity) + " plays " + std::to_string(got) +
                   " dB, expected " + std::to_string(want));
  }
}

void missing_cells_fall_back() {
  install();
  // Plain C#4 mf is missing: C4's take (the lower of two neighbours) plays,
  // repitched, and the third harmonic names it.
  constexpr double note_seconds = 0.5;
  constexpr double from_seconds = 0.1;
  constexpr double mark_tolerance = 0.02;
  auto runtime = make_runtime(wren::engine(), plain());
  const auto x = mono(render(*runtime, note(0, frames_of(note_seconds), c_sharp4, 1, mezzo),
                             frames_of(note_seconds)));
  const auto from = frames_of(from_seconds);
  const double mark = magnitude(x, from, window, key_hz(c_sharp4) * wren_test::key_harmonic) /
                      magnitude(x, from, window, key_hz(c_sharp4));
  REQUIRE_LT(std::fabs(mark - wren_test::key_mark(c4) / wren_test::body_level), mark_tolerance,
             "a missing cell plays the nearest key, the lower on a tie");

  // The vibrato take has no pp: the mf one plays, at its recorded level.
  // Amount 0.2 keeps the velocity in pp and blends a fifth of the way.
  constexpr double amount = 0.2;
  constexpr std::uint8_t soft = 40;
  constexpr double long_seconds = 1.0;
  constexpr double bloomed_seconds = 0.8;
  constexpr double ratio_tolerance = 0.1;
  auto v = lyrical_dry();
  v[at("wren.breath.amount")] = amount;
  auto blended = make_runtime(wren::engine(), v);
  const auto y = mono(render(*blended, clip_note(0, frames_of(long_seconds), d5, 1, soft),
                             frames_of(long_seconds)));
  REQUIRE_LT(telemetry_of(*blended)[wren::telemetry::last_velocity], static_cast<float>(c4),
             "the note plays pp");
  const auto bloomed = frames_of(bloomed_seconds);
  const double ratio = magnitude(y, bloomed, window, key_hz(d5) * wren_test::vibrato_harmonic) /
                       magnitude(y, bloomed, window, key_hz(d5));
  const double share = std::numbers::pi / 2.0 * amount;
  constexpr std::size_t pp = 0;
  constexpr std::size_t mf = 1;
  const double want =
      std::tan(share) *
      std::pow(10.0, (wren_test::layer_db[mf] - wren_test::layer_db[pp]) / decibels);
  REQUIRE_LT(std::fabs(ratio / want - 1.0), ratio_tolerance,
             "a missing layer plays the nearest layer");

  // A library without vibrato takes plays the plain one for both.
  (void)wren_test::install(
      [](wren_test::Kind kind, int, std::uint32_t) { return kind == wren_test::Kind::plain; },
      "wren-plain-only");
  auto lone = make_runtime(wren::engine(), lyrical_dry());
  const auto z = mono(
      render(*lone, clip_note(0, frames_of(long_seconds), d5, 1, loud), frames_of(long_seconds)));
  constexpr double absent = 1e-3;
  REQUIRE_LT(magnitude(z, bloomed, window, key_hz(d5) * wren_test::vibrato_harmonic),
             absent * magnitude(z, bloomed, window, key_hz(d5)),
             "a missing kind plays the other kind");
  install();
}

// ---- Joining notes ------------------------------------------------------------------------

void slurs_skip_the_attack_and_crossfade() {
  install();
  constexpr double first_seconds = 0.5;
  constexpr double overlap_seconds = 0.1;
  constexpr double gap_seconds = 0.05;
  constexpr double second_seconds = 0.5;
  constexpr double chiff_seconds = 0.03;
  constexpr double chiff_ratio = 0.1;
  const auto play = [&](bool slurred) {
    auto runtime = make_runtime(wren::engine(), plain());
    const auto second =
        frames_of(slurred ? first_seconds - overlap_seconds : first_seconds + gap_seconds);
    auto events = clip_note(0, frames_of(first_seconds), d5, 1, mezzo);
    append(events, clip_note(second, frames_of(second_seconds), e5, 2, mezzo));
    const auto x = mono(render(*runtime, events, second + frames_of(second_seconds)));
    const auto tongue = telemetry_of(*runtime)[wren::telemetry::last_tongue];
    return std::tuple{x, second, tongue};
  };
  const auto [slurred, slur_at, slur_tongue] = play(true);
  const auto [tongued, tongue_at, tongue_tongue] = play(false);
  REQUIRE_EQ(slur_tongue, static_cast<float>(breath::Tongue::slur), "overlap slurs, a gap tongues");
  REQUIRE_EQ(tongue_tongue, static_cast<float>(breath::Tongue::t), "overlap slurs, a gap tongues");
  const auto chiff = [&](const std::vector<double>& x, std::size_t from) {
    return magnitude(x, from, frames_of(chiff_seconds), key_hz(e5) * wren_test::attack_harmonic);
  };
  REQUIRE_LT(chiff(slurred, slur_at), chiff_ratio * chiff(tongued, tongue_at),
             "a slurred note starts past its attack; a tongued one plays it");
  // The crossfade never leaves a hole or a click.
  constexpr double slur_seconds = 0.045;
  constexpr double settle_seconds = 0.15;
  constexpr double level_floor = 0.4;
  constexpr double click_margin = 2.0;
  const double steady =
      rms(slurred, slur_at - frames_of(settle_seconds), frames_of(settle_seconds));
  for (std::size_t from = slur_at; from < slur_at + frames_of(slur_seconds); from += short_window) {
    REQUIRE_GT(rms(slurred, from, short_window), level_floor * steady,
               "the crossfade holds the level");
  }
  const auto after_slur = slur_at + frames_of(settle_seconds);
  REQUIRE_LT(max_step(slurred, slur_at, slur_at + frames_of(slur_seconds)),
             click_margin * max_step(slurred, after_slur, after_slur + frames_of(settle_seconds)),
             "and does not click");
  // After it, the new note sounds at its own pitch.
  constexpr double max_cents = 2.0;
  const double got = pitch_near(slurred, after_slur, window, key_hz(e5));
  REQUIRE_LT(std::fabs(wren_test::cents_per_octave * std::log2(got / key_hz(e5))), max_cents,
             "the slur arrives at E5");
}

void voices_mono_and_poly() {
  install();
  constexpr double seconds = 0.6;
  constexpr double second_at = 0.2;
  constexpr double measure_at = 0.4;
  constexpr double gone = 1e-2;
  // Two notes at one level: their fundamentals within a factor of this.
  constexpr double both_band = 2.0;
  const auto both = [&](std::string_view mode) {
    auto v = plain();
    v[at("wren.voices")] = choice_of("wren.voices", mode);
    auto runtime = make_runtime(wren::engine(), v);
    auto events = clip_note(0, frames_of(seconds), a4, 1, mezzo);
    append(events, clip_note(frames_of(second_at), frames_of(seconds - second_at), e5, 2, mezzo));
    const auto x = mono(render(*runtime, events, frames_of(seconds)));
    const auto from = frames_of(measure_at);
    return magnitude(x, from, window, key_hz(a4)) / magnitude(x, from, window, key_hz(e5));
  };
  REQUIRE_LT(both("Mono"), gone, "Mono: the second note takes over");
  const double poly = both("Poly 4");
  REQUIRE_GT(poly, 1.0 / both_band, "Poly 4: both sound");
  REQUIRE_LT(poly, both_band, "Poly 4: both sound");
}

// wren::played, the rules previews share with the kernel: Poly 4 tongues
// what Breath slurs, Staccato holds a third of a beat at most, the Vibrato
// control scales Breath's vibrato and a grace above the top key is dropped.
void played_follows_voices_and_articulation() {
  constexpr double long_beats = 2.0;
  constexpr double breath_vibrato = 0.8;
  constexpr double vibrato_control = 0.5;
  constexpr int whole_tone = 2;
  breath::Decision slurred;
  slurred.tongue = breath::Tongue::slur;
  slurred.vibrato = breath_vibrato;
  slurred.grace = true;
  slurred.grace_step = whole_tone;

  auto v = default_values(*wren::engine().descriptor);
  auto as_is = wren::played(v, a4, long_beats, slurred);
  REQUIRE_EQ(as_is.tongue, breath::Tongue::slur,
             "Mono, Auto: the decision plays as Breath made it");
  REQUIRE_EQ(as_is.length_beats, long_beats, "Mono, Auto: the decision plays as Breath made it");
  REQUIRE_TRUE(as_is.grace, "Mono, Auto: the decision plays as Breath made it");
  REQUIRE_EQ(as_is.vibrato, breath_vibrato * v[at("wren.breath.vibrato")],
             "the Vibrato control scales Breath's share");

  v[at("wren.voices")] = choice_of("wren.voices", "Poly 4");
  v[at("wren.breath.vibrato")] = vibrato_control;
  const auto poly = wren::played(v, a4, long_beats, slurred);
  REQUIRE_EQ(poly.tongue, breath::Tongue::t, "Poly 4 tongues a slur");
  REQUIRE_EQ(poly.vibrato, breath_vibrato * vibrato_control, "half the Vibrato, half the share");

  v[at("wren.articulation")] = choice_of("wren.articulation", "Staccato");
  REQUIRE_EQ(wren::played(v, a4, long_beats, slurred).length_beats, breath::staccato_beats,
             "Staccato holds a long note a third of a beat");
  constexpr double short_beats = breath::staccato_beats / 2.0;
  REQUIRE_EQ(wren::played(v, a4, short_beats, slurred).length_beats, short_beats,
             "a shorter note keeps its length");
  REQUIRE_EQ(wren::played(v, a4, 0.0, slurred).length_beats, breath::staccato_beats,
             "a live note is held a third of a beat");

  require(!wren::played(v, wren::last_key, long_beats, slurred).grace,
          "no grace above Wren's top key");
  const std::vector<double> other_size;
  const auto unchanged = wren::played(other_size, a4, long_beats, slurred);
  REQUIRE_EQ(unchanged.tongue, breath::Tongue::slur,
             "values of another size play the decision as it is");
  REQUIRE_EQ(unchanged.vibrato, breath_vibrato,
             "values of another size play the decision as it is");
}

void vibrato_blooms_after_its_delay() {
  install();
  // Lyrical: 160 ms delay, then a 400 ms bloom (Bloom's default).
  constexpr double delay_seconds = 0.160;
  constexpr double bloom_seconds = 0.400;
  constexpr double note_seconds = 1.2;
  constexpr double before_seconds = 0.08;
  constexpr double absent = 1e-3;
  constexpr double ratio_tolerance = 0.25;
  constexpr double dominant = 20.0;
  const auto play = [&](double vibrato) {
    auto v = lyrical_dry();
    v[at("wren.breath.vibrato")] = vibrato;
    auto runtime = make_runtime(wren::engine(), v);
    auto x = mono(
        render(*runtime, clip_note(0, frames_of(note_seconds), d5, 1), frames_of(note_seconds)));
    // The indicator follows the played vibrato: off when the control is 0.
    REQUIRE_EQ(telemetry_of(*runtime)[wren::telemetry::vibrato], vibrato > 0.0 ? 1.0F : 0.0F,
               "telemetry: vibrato follows the played value");
    return x;
  };
  const auto x = play(1.0);
  const auto ratio = [&](const std::vector<double>& y, std::size_t centre) {
    const auto from = centre - window / 2;
    return magnitude(y, from, window, key_hz(d5) * wren_test::vibrato_harmonic) /
           magnitude(y, from, window, key_hz(d5));
  };
  REQUIRE_LT(ratio(x, frames_of(before_seconds) + window / 2), absent, "straight before the delay");
  // Half way through the bloom the take's share is a quarter.
  constexpr double half = 0.5;
  constexpr double quarter = 0.25;
  const double mid = ratio(x, frames_of(delay_seconds + half * bloom_seconds));
  const double want = std::tan(std::numbers::pi / 2.0 * quarter);
  REQUIRE_LT(std::fabs(mid / want - 1.0), ratio_tolerance, "half way it blends a quarter in");
  REQUIRE_GT(ratio(x, frames_of(delay_seconds + bloom_seconds) + window), dominant,
             "then it is all vibrato");
  const auto none = play(0.0);
  REQUIRE_LT(ratio(none, frames_of(delay_seconds + bloom_seconds) + window), absent,
             "Vibrato 0 keeps the plain take");
}

void loops_hold_a_note_beyond_its_recording() {
  install();
  constexpr double held_seconds = 3.0; // Five times the recording.
  constexpr double steady_from = 0.25;
  constexpr double steady_seconds = 0.1;
  constexpr double piece_seconds = 0.05;
  constexpr double level_tolerance_db = 0.1;
  constexpr double step_margin = 1.01;
  auto runtime = make_runtime(wren::engine(), plain());
  const auto x = mono(
      render(*runtime, note(0, frames_of(held_seconds), d5, 1, mezzo), frames_of(held_seconds)));
  const auto reference_from = frames_of(steady_from);
  const double reference = rms(x, reference_from, frames_of(steady_seconds));
  const double reference_step =
      max_step(x, reference_from, reference_from + frames_of(steady_seconds));
  for (std::size_t from = reference_from; from + frames_of(piece_seconds) < frames_of(held_seconds);
       from += frames_of(piece_seconds)) {
    REQUIRE_LT(std::fabs(db(rms(x, from, frames_of(piece_seconds)) / reference)),
               level_tolerance_db,
               "the held note keeps its level at " +
                   std::to_string(static_cast<double>(from) / rate) + " s");
  }
  REQUIRE_LT(max_step(x, reference_from, frames_of(held_seconds)), step_margin * reference_step,
             "and never jumps at the loop");
}

void releases_end_on_time() {
  install();
  constexpr double release_seconds = 0.090;
  constexpr double staccato_seconds = 0.040;
  // The envelope is evaluated every 16 frames.
  constexpr double envelope_frames = 16.0;
  constexpr double envelope_seconds = envelope_frames / rate;
  constexpr double still_share = 0.6;
  const auto play = [&](double length, double total) {
    auto runtime = make_runtime(wren::engine(), plain());
    return mono(render(*runtime, clip_note(0, frames_of(length), d5, 1, mezzo), frames_of(total)));
  };
  constexpr double held_seconds = 0.4;
  constexpr double total_seconds = 0.7;
  const auto x = play(held_seconds, total_seconds);
  REQUIRE_GT(rms(x, frames_of(held_seconds + still_share * release_seconds), short_window), audible,
             "the release sounds");
  const auto silent_from = frames_of(held_seconds + release_seconds + envelope_seconds);
  REQUIRE_LT(rms(x, silent_from, x.size() - silent_from), silence, "and ends after 90 ms");
  // Under a third of a beat: staccato, a 40 ms release.
  constexpr double short_seconds = 0.1;
  REQUIRE_LT(short_seconds / seconds_per_beat, breath::staccato_beats, "a staccato length");
  const auto s = play(short_seconds, total_seconds);
  REQUIRE_GT(rms(s, frames_of(short_seconds + still_share * staccato_seconds), short_window),
             audible, "a staccato release sounds");
  const auto staccato_from = frames_of(short_seconds + staccato_seconds + envelope_seconds);
  REQUIRE_LT(rms(s, staccato_from, s.size() - staccato_from), silence, "and ends after 40 ms");
}

void graces_use_the_project_key() {
  install();
  // The line's graced note: A4 on beat 12 after C5, a leap of a third.
  constexpr double before_beat = 11.0;
  constexpr double graced_beat = 12.0;
  constexpr double before_beats = 1.0;
  constexpr double graced_beats = 2.5;
  constexpr double grace_seconds = 0.070;
  constexpr double settle_seconds = 0.010;
  constexpr std::size_t grace_window = 2'048;
  constexpr std::uint8_t tonic_d = 2;
  auto v = default_values(*wren::engine().descriptor);
  v[at("wren.tone.space")] = 0.0;
  const auto play = [&](std::optional<project::MusicalKey> key) {
    auto runtime = make_runtime(wren::engine(), v);
    runtime->offer_project_key(key);
    auto events = clip_note(at_beat(before_beat), at_beat(before_beats), c5, 1);
    append(events, clip_note(at_beat(graced_beat), at_beat(graced_beats), a4, 2));
    const auto start = at_beat(graced_beat);
    const auto x = mono(render(*runtime, events, start + frames_of(grace_seconds + grace_seconds)));
    REQUIRE_EQ(telemetry_of(*runtime)[wren::telemetry::last_grace], 1.0F, "telemetry: a grace");
    const auto from = start + frames_of(settle_seconds);
    return magnitude(x, from, grace_window, key_hz(a_sharp4)) /
           magnitude(x, from, grace_window, key_hz(b4));
  };
  REQUIRE_GT(play(project::MusicalKey{tonic_d, project::KeyMode::minor}), 1.0,
             "in D minor the grace is B-flat");
  REQUIRE_LT(play(project::MusicalKey{tonic_d, project::KeyMode::major}), 1.0,
             "in D major it is B");
  REQUIRE_LT(play(std::nullopt), 1.0, "without a key, a whole tone up");
}

// ---- Determinism, real time and the tail --------------------------------------------------

// The audition line's first phrase as clip notes, with Lyrical, Lots of
// ornaments, Humanize and the hall.
std::vector<Event> phrase() {
  struct Drawn {
    double beat;
    std::uint8_t key;
    double length;
  };
  constexpr std::uint8_t f5 = 77;
  constexpr std::array<Drawn, 8> notes{{{0.5, a4, 0.5},
                                        {1.0, d5, 1.6},
                                        {2.75, c5, 0.25},
                                        {3.0, d5, 0.5},
                                        {3.5, f5, 2.1},
                                        {5.5, e5, 0.6},
                                        {6.0, d5, 0.25},
                                        {6.25, d5, 0.25}}};
  std::vector<Event> events;
  std::uint64_t id = 1;
  for (const auto& n : notes) {
    append(events, clip_note(at_beat(n.beat), at_beat(n.length), n.key, id++, mezzo));
  }
  return events;
}

std::vector<double> rich() {
  constexpr double humanize = 3.0;
  auto v = default_values(*wren::engine().descriptor);
  v[at("wren.breath.ornaments")] = choice_of("wren.breath.ornaments", "Lots");
  v[at("wren.breath.humanize")] = humanize;
  return v;
}

constexpr std::uint8_t phrase_tonic = 2;
constexpr double phrase_beats = 7.5;

void block_splits_render_the_same() {
  install();
  const auto frames = at_beat(phrase_beats);
  const auto played = [&](std::size_t size) {
    auto runtime = make_runtime(wren::engine(), rich());
    runtime->offer_project_key(project::MusicalKey{phrase_tonic, project::KeyMode::minor});
    return render(*runtime, phrase(), frames, size);
  };
  const auto expected = played(block);
  for (const std::size_t size :
       {std::size_t{1}, std::size_t{31}, std::size_t{0}, std::size_t{512}}) {
    const auto r = played(size);
    require(r.left == expected.left && r.right == expected.right,
            "block " + std::to_string(size) + " renders identically");
  }
}

void a_reset_plays_the_same_again() {
  install();
  const auto frames = at_beat(phrase_beats);
  auto runtime = make_runtime(wren::engine(), rich());
  runtime->offer_project_key(project::MusicalKey{phrase_tonic, project::KeyMode::minor});
  const auto first = render(*runtime, phrase(), frames);
  constexpr std::uint64_t seek = 2;
  const auto again = render(*runtime, phrase(), frames, block, rate, bpm, {}, seek);
  require(std::equal(again.left.begin() + static_cast<std::ptrdiff_t>(declick_frames),
                     again.left.end(),
                     first.left.begin() + static_cast<std::ptrdiff_t>(declick_frames)),
          "a reset renders the same again, past the reset's declick: export equals playback");
}

void the_callback_does_not_allocate() {
  install();
  auto v = rich();
  v[at("wren.voices")] = choice_of("wren.voices", "Poly 4");
  for (const std::string_view mode : {"Mono", "Poly 4"}) {
    v[at("wren.voices")] = choice_of("wren.voices", mode);
    auto runtime = make_runtime(wren::engine(), v);
    runtime->offer_project_key(project::MusicalKey{phrase_tonic, project::KeyMode::major});
    constexpr std::uint64_t blocks = 800;
    constexpr std::uint64_t every = 16; // A note every 2048 frames, held 1.5x that.
    constexpr std::uint64_t held_blocks = every * 3 / 2;
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
    events[1].frames = static_cast<std::uint32_t>(block * held_blocks);
    // Prime: the first block adopts values and the key.
    (void)runtime->process(buffer, {}, 0, 1, {}, {});
    counted_allocations = 0;
    counting_allocations = true;
    for (std::uint64_t b = 1; b < blocks; ++b) {
      const auto hit = b / every;
      const double beat = static_cast<double>(b * block) / (seconds_per_beat * rate);
      const SynthTransport transport{beat, 1.0 / (seconds_per_beat * rate), beat / 4.0, 4.0};
      events[0].note_id_bits = {0, hit - 1};
      events[0].key = static_cast<std::uint8_t>(a4 + static_cast<int>((hit + keys - 1) % keys));
      events[1].note_id_bits = {0, hit};
      events[1].key = static_cast<std::uint8_t>(a4 + static_cast<int>(hit % keys));
      const auto due = b % every == 0 ? std::span(events) : std::span(events).subspan(0, 0);
      (void)runtime->process(buffer, due, b * block, 1, transport, {});
    }
    counting_allocations = false;
    REQUIRE_EQ(counted_allocations, 0,
               std::string(mode) + ": the callback allocated " +
                   std::to_string(counted_allocations) + " times");
  }
}

void the_tail_bound_covers_a_released_note() {
  install();
  constexpr double tail_silence = 1e-5;
  constexpr double after_seconds = 0.1;
  const auto v = default_values(*wren::engine().descriptor); // Space 30 %.
  const auto tail = wren::engine().tail_frames(v, rate);
  REQUIRE_GT(tail, wren::engine().tail_frames(plain(), rate), "the hall lengthens the tail");
  auto runtime = make_runtime(wren::engine(), v);
  const auto off = frames_of(after_seconds);
  const auto x =
      mono(render(*runtime, clip_note(0, off, d5, 1), off + tail + frames_of(after_seconds)));
  REQUIRE_GT(rms(x, off + frames_of(after_seconds), frames_of(after_seconds)), audible,
             "the hall rings on");
  REQUIRE_LT(rms(x, off + tail, frames_of(after_seconds)), tail_silence,
             "silent after the tail bound");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "wren engine contract passed",
      {
          {"a_missing_library_is_silent", [] { a_missing_library_is_silent(); }},
          {"malformed_libraries_are_rejected", [] { malformed_libraries_are_rejected(); }},
          {"descriptor_registry_and_presets", [] { descriptor_registry_and_presets(); }},
          {"keys_are_in_tune", [] { keys_are_in_tune(); }},
          {"velocity_picks_layers_and_trims", [] { velocity_picks_layers_and_trims(); }},
          {"missing_cells_fall_back", [] { missing_cells_fall_back(); }},
          {"slurs_skip_the_attack_and_crossfade", [] { slurs_skip_the_attack_and_crossfade(); }},
          {"voices_mono_and_poly", [] { voices_mono_and_poly(); }},
          {"played_follows_voices_and_articulation",
           [] { played_follows_voices_and_articulation(); }},
          {"vibrato_blooms_after_its_delay", [] { vibrato_blooms_after_its_delay(); }},
          {"loops_hold_a_note_beyond_its_recording",
           [] { loops_hold_a_note_beyond_its_recording(); }},
          {"releases_end_on_time", [] { releases_end_on_time(); }},
          {"graces_use_the_project_key", [] { graces_use_the_project_key(); }},
          {"block_splits_render_the_same", [] { block_splits_render_the_same(); }},
          {"a_reset_plays_the_same_again", [] { a_reset_plays_the_same_again(); }},
          {"the_callback_does_not_allocate", [] { the_callback_does_not_allocate(); }},
          {"the_tail_bound_covers_a_released_note",
           [] { the_tail_bound_covers_a_released_note(); }},
      });
}
