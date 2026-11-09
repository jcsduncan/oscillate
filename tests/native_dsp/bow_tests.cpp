#include "bow_test_library.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/bow.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "synth_test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

// Bow's engine contract (ADR-0111), on a synthetic library whose takes are
// pure tones with a chiff in their attack and a level per dynamic layer
// (bow_test_library.hpp): a missing library is silent, says why and plays
// once installed; malformed libraries are rejected; keys play in tune in
// both modes; the wheel crossfades the layers by level; legato joins past
// the attack without a click; keyswitches latch and unlatch; voices stay
// bounded; Tone darkens toward sul tasto and thins toward sul ponticello;
// vibrato blooms; Humanise is seeded; block splits and resets render the
// same; the callback never allocates; the tail bound covers a release.
namespace {

using namespace oscillate;
using namespace oscillate::native_dsp;
using namespace oscillate::native_dsp::synth;
using namespace synth_test;
namespace library = bow::library;

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr double decibels = 20.0;
constexpr std::uint8_t c4 = 60;
constexpr std::uint8_t e4 = 64;
constexpr std::uint8_t g4 = 67;
constexpr std::uint8_t a4 = 69;
constexpr std::uint8_t c3 = 48;
constexpr std::uint8_t g3 = 55;
constexpr std::uint8_t c7 = 96;
constexpr std::uint8_t loud = 110;
constexpr std::uint8_t full = 127;
constexpr std::size_t block = 128;
// A reset fades the last output out over 1.5 ms; compare past it.
constexpr std::size_t declick_frames = 128;
constexpr std::size_t window = 2'048;
// Below this a signal is silence.
constexpr double silence = 1e-6;
constexpr double audible = 1e-3;
constexpr double in_tune_cents = 3.0;
constexpr double ms_per_second = 1'000.0;
constexpr double bpm = 120.0;
constexpr double level_tolerance_db = 0.3;

const ParameterDescriptor& parameter(std::string_view id) {
  for (const auto& p : bow::engine().descriptor->parameters) {
    if (p.id == id) {
      return p;
    }
  }
  require(false, std::string("unknown parameter ") + std::string(id));
  return bow::engine().descriptor->parameters.front();
}

std::size_t at(std::string_view id) {
  const auto& all = bow::engine().descriptor->parameters;
  return static_cast<std::size_t>(&parameter(id) - all.data());
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a parameter id, then one of its choices.
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

// One violas player, as recorded: no added vibrato, no humanising, Tone
// ordinario, dry, at unity.
std::vector<double> dry(std::string_view articulation = "Sustain") {
  auto v = default_values(*bow::engine().descriptor);
  v[at("bow.articulation")] = choice_of("bow.articulation", articulation);
  v[at("bow.players")] = bow::min_players;
  v[at("bow.humanise")] = 0.0;
  v[at("bow.vibrato")] = 0.0;
  v[at("bow.tone")] = 0.5;
  v[at("bow.space")] = 0.0;
  v[at("bow.level")] = 0.0;
  v[at("bow.perf.wheel")] = 1.0;
  return v;
}

// Everything moving: a full section, humanised, with vibrato and the hall.
std::vector<double> rich() {
  auto v = default_values(*bow::engine().descriptor);
  v[at("bow.players")] = bow::max_players;
  v[at("bow.humanise")] = 1.0;
  v[at("bow.vibrato")] = 1.0;
  return v;
}

void install() { (void)bow_test::install(); }

std::size_t frames_of(double seconds) { return static_cast<std::size_t>(seconds * rate); }

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

double max_step(const std::vector<double>& x, std::size_t begin, std::size_t end) {
  double most = 0.0;
  for (std::size_t i = std::max<std::size_t>(begin, 1); i < end && i < x.size(); ++i) {
    most = std::max(most, std::fabs(x[i] - x[i - 1]));
  }
  return most;
}

std::vector<float> telemetry_of(NativeSynthRuntime& runtime) {
  std::vector<float> out;
  REQUIRE_TRUE(runtime.read_telemetry(out), "telemetry arrives");
  REQUIRE_GE(out.size(), bow::telemetry::size, "telemetry arrives");
  return out;
}

// The steady level of one held note at a wheel position.
double steady_level(std::vector<double> values, double wheel, std::uint8_t key = c4) {
  constexpr double hold_seconds = 0.6;
  constexpr double from_seconds = 0.35;
  values[at("bow.perf.wheel")] = wheel;
  auto runtime = make_runtime(bow::engine(), values);
  const auto x = mono(
      render(*runtime, note(0, frames_of(hold_seconds), key, 1, loud), frames_of(hold_seconds)));
  return rms(x, frames_of(from_seconds), window);
}

// ---- Library ----------------------------------------------------------------------------

void a_missing_library_is_silent_then_plays_once_installed() {
  const auto nowhere = wren_test::scratch_directory("bow-nothing-here");
  std::filesystem::create_directories(nowhere);
  ::setenv(std::string(library::directory_variable).c_str(), nowhere.c_str(), 1);
  const auto first = library::status();
  REQUIRE_EQ(first.state, library::State::loading, "the first look reads in the background");
  // A Bow made while the read runs says what it found once it ends.
  auto early = make_runtime(bow::engine(), dry());
  library::wait();
  (void)render(*early, {}, block);
  REQUIRE_EQ(telemetry_of(*early)[bow::telemetry::library],
             static_cast<float>(library::State::missing),
             "a Bow made while loading reports the library missing");
  const auto status = library::status();
  REQUIRE_EQ(status.state, library::State::missing, "without the library Bow says why");
  REQUIRE_TRUE(!status.message.empty(), "without the library Bow says why");
  REQUIRE_EQ(status.directory, nowhere, "and where it looked");
  auto runtime = make_runtime(bow::engine(), dry());
  const auto silent = mono(render(*runtime, note(0, frames_of(0.2), c4, 1), frames_of(0.3)));
  REQUIRE_EQ(rms(silent, 0, silent.size()), 0.0, "and is silent");
  REQUIRE_EQ(telemetry_of(*runtime)[bow::telemetry::library],
             static_cast<float>(library::State::missing), "telemetry says the library is missing");
  // Installed where it looked: the next look loads it, and an idle Bow
  // that was silent plays.
  bow_test::write_library(nowhere);
  REQUIRE_EQ(library::status().state, library::State::ready, "installing is seen");
  const auto heard = mono(render(*runtime, note(0, frames_of(0.2), c4, 2), frames_of(0.3)));
  REQUIRE_GT(rms(heard, 0, heard.size()), audible, "the silent Bow plays once installed");
}

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
  const auto base = wren_test::scratch_directory("bow-malformed");
  bow_test::write_library(base);
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
  REQUIRE_EQ(broken("not a bow library\n" + text), library::State::invalid, "format line");
  // Fields of a sample line, after its tag (the first is a stereo sustain).
  constexpr std::size_t channels_field = 0;
  constexpr std::size_t mode_field = 1;
  constexpr std::size_t key_field = 4;
  constexpr std::size_t layer_field = 5;
  constexpr std::size_t loop_start_field = 10;
  constexpr std::size_t loop_end_field = 11;
  constexpr std::size_t cents_field = 12;
  const std::string off_key_cents = "150"; // Over a semitone.
  const std::string three_channels = "3";
  REQUIRE_EQ(broken(with_field(text, channels_field, three_channels)), library::State::invalid,
             "a three-channel take");
  REQUIRE_EQ(broken(with_field(text, mode_field, "quartet")), library::State::invalid,
             "an unknown mode");
  REQUIRE_EQ(broken(with_field(text, key_field, std::to_string(bow::last_key + 1))),
             library::State::invalid, "a key outside E1..G7");
  REQUIRE_EQ(broken(with_field(text, layer_field, std::to_string(bow::max_layers))),
             library::State::invalid, "a fourth dynamic layer");
  REQUIRE_EQ(broken(with_field(text, loop_end_field, "0")), library::State::invalid,
             "a sustain without its loop");
  REQUIRE_EQ(broken(with_field(text, loop_start_field, std::to_string(bow_test::loop_end))),
             library::State::invalid, "an empty loop");
  REQUIRE_EQ(broken(with_field(text, cents_field, off_key_cents)), library::State::invalid,
             "a recording over a semitone off its key");
  const auto first_sample = text.substr(
      text.find("sample "), text.find('\n', text.find("sample ")) - text.find("sample ") + 1);
  REQUIRE_EQ(broken(text + first_sample), library::State::invalid, "a sample listed twice");
  std::string no_mono = text;
  const auto mono_line = no_mono.find("mono_pack");
  no_mono.erase(mono_line, no_mono.find('\n', mono_line) - mono_line + 1);
  REQUIRE_EQ(broken(no_mono), library::State::invalid, "a manifest without its mono pack");
  std::ofstream(manifest) << text;
  REQUIRE_EQ(library::load(base).state, library::State::ready, "the untouched manifest loads");
  std::filesystem::resize_file(base / "bow-mono.pack",
                               std::filesystem::file_size(base / "bow-mono.pack") / 2);
  REQUIRE_EQ(library::load(base).state, library::State::invalid, "a truncated mono pack");
}

void descriptor_registry_and_presets() {
  constexpr std::size_t parameters = 13;
  constexpr std::array<std::string_view, 6> presets{"Sul Tasto Pad",      "Warm Section",
                                                    "Pizzicato Ostinato", "Tremolo Swell",
                                                    "Solo Cello",         "Ensemble Legato"};
  const auto& descriptor = *bow::engine().descriptor;
  REQUIRE_OK(validate_engine(descriptor), "the table validates");
  REQUIRE_EQ(descriptor.parameters.size(), parameters, "the ADR's thirteen parameters");
  REQUIRE_EQ(descriptor.presets.size(), presets.size(), "six presets");
  for (std::size_t i = 0; i < presets.size(); ++i) {
    REQUIRE_EQ(descriptor.presets[i].name, presets[i], "the presets, in order");
    REQUIRE_OK(preset_values(descriptor, descriptor.presets[i]), "every preset resolves");
  }
  REQUIRE_TRUE(descriptor.presets.front().values.empty(), "Sul Tasto Pad is the defaults");
  require(!bow::engine().models_pedals, "the runtime's pedals hold Bow's notes");
  require(parameter("bow.perf.wheel").automatable, "the mod wheel is an automation lane");
  constexpr double one_player = 1.0;
  constexpr double eight_players = 8.0;
  constexpr double five_players = 5.0;
  REQUIRE_EQ(bow::ensemble_layers(one_player), std::size_t{1}, "one player, one layer");
  REQUIRE_EQ(bow::ensemble_layers(five_players), std::size_t{2}, "every four players a layer");
  REQUIRE_EQ(bow::ensemble_layers(eight_players), std::size_t{2}, "every four players a layer");
  REQUIRE_EQ(bow::ensemble_layers(bow::max_players), bow::max_ensemble_layers, "at most four");
  REQUIRE_EQ(bow::dynamic_name(descriptor.parameters[at("bow.perf.wheel")].default_value),
             std::string_view("mp"), "the default wheel reads mp");
  REQUIRE_EQ(bow::dynamic_name(0.0), std::string_view("ppp"), "the bottom is ppp");
  REQUIRE_EQ(bow::dynamic_name(1.0), std::string_view("ff"), "the top is ff");
  REQUIRE_EQ(bow::instrument_for(bow::Section::ensemble, c3), bow::Instrument::cello,
             "Ensemble splits by register");
  REQUIRE_EQ(bow::instrument_for(bow::Section::ensemble, c7), bow::Instrument::violin,
             "Ensemble splits by register");
  REQUIRE_TRUE(bow::is_keyswitch(bow::first_keyswitch) &&
                   bow::last_keyswitch < bow::instrument_ranges.back().low,
               "the keyswitches sit below every range");
}

// ---- Playing ------------------------------------------------------------------------------

void keys_are_in_tune_in_both_modes() {
  install();
  constexpr double start_seconds = 0.3;
  for (const std::string_view mode : {"Solo", "Section"}) {
    auto v = dry();
    v[at("bow.mode")] = choice_of("bow.mode", mode);
    // Sampled keys and repitched neighbours both ways.
    for (const std::uint8_t key : {c3, g3, c4, e4, a4}) {
      auto runtime = make_runtime(bow::engine(), v);
      const auto x = mono(render(*runtime, note(0, frames_of(0.6), key, 1), frames_of(0.6)));
      const double expected = bow_test::key_hz(key);
      const double measured = pitch_near(x, frames_of(start_seconds), window * 2, expected);
      REQUIRE_LT(std::fabs(cents(measured, expected)), in_tune_cents,
                 std::string(mode) + " key " + std::to_string(key) + " plays in tune");
    }
  }
}

void the_wheel_crossfades_the_layers() {
  install();
  // A section's layers are 12 dB apart: half the wheel plays 6 dB under
  // the top, the dynamics interpolated in dB between the layers.
  const auto section = dry();
  const double top = steady_level(section, 1.0);
  const double middle = steady_level(section, 0.5);
  const double top_db = bow_test::section_layer_db[1];
  const double bottom_db = bow_test::section_layer_db[0];
  REQUIRE_LT(std::fabs(db(top / middle) - (top_db - bottom_db) / 2.0), level_tolerance_db,
             "half the wheel is halfway between the layers in dB");
  double previous = 0.0;
  for (const double wheel : {0.2, 0.4, 0.6, 0.8, 1.0}) {
    const double level = steady_level(section, wheel);
    REQUIRE_GT(level, previous, "the wheel bows louder all the way up");
    previous = level;
  }
  constexpr double niente_below_db = 30.0;
  REQUIRE_LT(db(steady_level(section, 0.0) / top), -niente_below_db,
             "the wheel's bottom lifts the bow toward silence");
  // A solo has three layers: the middle of the wheel is the middle layer.
  auto solo = dry();
  solo[at("bow.mode")] = choice_of("bow.mode", "Solo");
  const double solo_top = steady_level(solo, 1.0);
  const double solo_middle = steady_level(solo, 0.5);
  REQUIRE_LT(std::fabs(db(solo_top / solo_middle) -
                       (bow_test::solo_layer_db[2] - bow_test::solo_layer_db[1])),
             level_tolerance_db, "a solo's middle wheel plays its mf layer");
}

void legato_joins_past_the_attack_without_a_click() {
  install();
  constexpr double switch_seconds = 0.5;
  constexpr double end_seconds = 1.4;
  // The join may sum the two notes' slopes under an equal-power crossfade
  // (at most sqrt 2 of the larger); a cut would jump by up to twice the
  // amplitude.
  constexpr double bounded_step = std::numbers::sqrt2 * 1.05;
  const auto v = dry("Legato");
  const auto join = frames_of(switch_seconds);
  std::vector<Event> events = note(0, frames_of(1.0), c4, 1);
  append(events, note(join, frames_of(0.8), g4, 2));
  auto runtime = make_runtime(bow::engine(), v);
  const auto x = mono(render(*runtime, events, frames_of(end_seconds)));
  const auto legato_frames = frames_of(v[at("bow.legato")] / ms_per_second);
  const double before = max_step(x, join - window, join);
  const double after =
      max_step(x, join + legato_frames + window, join + legato_frames + 2 * window);
  const double across = max_step(x, join, join + legato_frames);
  REQUIRE_LT(across, bounded_step * std::max(before, after), "the join has no click");
  // Past the attack: the new note's chiff is missing, where a fresh G4 has it.
  const double chiff_hz = bow_test::played_hz(g4) * bow_test::attack_harmonic;
  const auto chiff_window = frames_of(bow_test::attack_seconds);
  auto fresh = make_runtime(bow::engine(), dry());
  const auto alone = mono(render(*fresh, note(0, frames_of(0.3), g4, 1), frames_of(0.3)));
  constexpr double attack_dropped_db = -20.0;
  REQUIRE_LT(
      db(magnitude(x, join, chiff_window, chiff_hz) / magnitude(alone, 0, chiff_window, chiff_hz)),
      attack_dropped_db, "a legato note starts past its attack");
  // After the legato time only the new note sounds.
  const auto later = join + legato_frames + window;
  constexpr double gone_db = -40.0;
  REQUIRE_LT(db(magnitude(x, later, window, bow_test::played_hz(c4)) /
                magnitude(x, later, window, bow_test::played_hz(g4))),
             gone_db, "the note before has handed over");
}

void keyswitches_latch_the_articulation() {
  install();
  constexpr std::uint8_t pizzicato_switch = bow::first_keyswitch + 3;
  constexpr double hold_seconds = 1.0;
  constexpr double probe_seconds = 0.8;
  const auto held = [&](NativeSynthRuntime& runtime, std::uint64_t id) {
    std::vector<Event> events = note(0, frames_of(0.05), pizzicato_switch, id);
    append(events, note(frames_of(0.1), frames_of(hold_seconds), c4, id + 1));
    return mono(render(runtime, events, frames_of(hold_seconds)));
  };
  // Sustain on the panel; the keyswitch plays every note pizzicato, which
  // dies away under a held key, and sounds nothing itself.
  auto runtime = make_runtime(bow::engine(), dry());
  const auto plucked = held(*runtime, 1);
  REQUIRE_LT(rms(plucked, frames_of(probe_seconds), window), silence,
             "a latched pizzicato plays out under a held key");
  auto telemetry = telemetry_of(*runtime);
  REQUIRE_EQ(telemetry[bow::telemetry::articulation],
             static_cast<float>(bow::Articulation::pizzicato), "telemetry shows the keyswitch");
  REQUIRE_EQ(telemetry[bow::telemetry::latched], 1.0F, "and that it is latched");
  REQUIRE_EQ(telemetry[bow::telemetry::notes], 1.0F, "a keyswitch is not a note");
  // The panel's next change wins.
  auto tremolo = dry("Tremolo");
  REQUIRE_TRUE(runtime->offer_values(tremolo, 2), "the panel changes");
  (void)render(*runtime, {}, block);
  telemetry = telemetry_of(*runtime);
  REQUIRE_EQ(telemetry[bow::telemetry::articulation],
             static_cast<float>(bow::Articulation::tremolo), "a panel change unlatches");
  REQUIRE_EQ(telemetry[bow::telemetry::latched], 0.0F, "a panel change unlatches");
  // Without the keyswitch the sustain holds.
  auto sustained = make_runtime(bow::engine(), dry());
  const auto bowed = mono(render(*sustained, note(frames_of(0.1), frames_of(hold_seconds), c4, 1),
                                 frames_of(hold_seconds)));
  REQUIRE_GT(rms(bowed, frames_of(probe_seconds), window), audible, "a sustain holds");
  // A reset (a seek) returns to the panel's articulation.
  auto seeking = make_runtime(bow::engine(), dry());
  (void)held(*seeking, 1);
  constexpr std::uint64_t seek = 2;
  (void)render(*seeking, {}, block, block, rate, bpm, {}, seek);
  REQUIRE_EQ(telemetry_of(*seeking)[bow::telemetry::latched], 0.0F, "a seek unlatches");
}

void ensemble_layers_and_voices_are_bounded() {
  install();
  constexpr double hold_seconds = 0.3;
  const auto voices_for = [&](double players, int notes) {
    auto v = dry();
    v[at("bow.players")] = players;
    auto runtime = make_runtime(bow::engine(), v);
    std::vector<Event> events;
    const auto range = bow::playable(bow::Section::violas);
    for (int n = 0; n < notes; ++n) {
      append(events, note(0, frames_of(hold_seconds),
                          static_cast<std::uint8_t>(range.low + n % (range.high - range.low)),
                          static_cast<std::uint64_t>(n + 1)));
    }
    return render(*runtime, events, frames_of(hold_seconds / 2)).last.active_voices;
  };
  constexpr int chord = 3;
  REQUIRE_EQ(voices_for(1.0, chord), static_cast<std::uint32_t>(chord), "one player a note");
  REQUIRE_EQ(voices_for(bow::max_players, chord),
             static_cast<std::uint32_t>(chord * bow::max_ensemble_layers),
             "sixteen players are four layers a note");
  constexpr int cluster = 40;
  constexpr std::uint32_t max_voices = 64;
  REQUIRE_LE(voices_for(bow::max_players, cluster), max_voices, "voices are bounded");
}

void tone_runs_from_tasto_to_ponticello() {
  install();
  const auto level = [&](double tone, std::uint8_t key) {
    auto v = dry();
    v[at("bow.section")] = choice_of("bow.section", "Ensemble");
    v[at("bow.tone")] = tone;
    return steady_level(v, 1.0, key);
  };
  constexpr std::uint8_t high = c7;
  constexpr std::uint8_t low = c3;
  REQUIRE_LT(level(0.0, high), level(0.5, high), "sul tasto darkens the top");
  REQUIRE_LT(level(0.5, high), level(1.0, high), "sul ponticello brightens the top");
  REQUIRE_GT(level(0.5, low), level(1.0, low), "sul ponticello thins the fundamental");
}

void vibrato_blooms_after_its_delay() {
  install();
  constexpr double early_seconds = 0.05;
  constexpr double late_seconds = 1.2;
  constexpr std::size_t probe = 960; // 20 ms.
  constexpr std::size_t probes = 25;
  constexpr double deep_cents = 15.0;
  constexpr double still_cents = 2.0;
  const auto spread = [&](double vibrato, double from_seconds) {
    auto v = dry();
    v[at("bow.vibrato")] = vibrato;
    auto runtime = make_runtime(bow::engine(), v);
    const auto x = mono(render(*runtime, note(0, frames_of(1.8), a4, 1), frames_of(1.8)));
    double low = 1e9;
    double high = -1e9;
    for (std::size_t i = 0; i < probes; ++i) {
      const double hz =
          pitch_near(x, frames_of(from_seconds) + i * probe / 2, probe, bow_test::key_hz(a4));
      const double c = cents(hz, bow_test::key_hz(a4));
      low = std::min(low, c);
      high = std::max(high, c);
    }
    return high - low;
  };
  REQUIRE_GT(spread(1.0, late_seconds), deep_cents, "full vibrato swings the pitch");
  REQUIRE_LT(spread(1.0, early_seconds), deep_cents / 2.0, "after the note settles");
  REQUIRE_LT(spread(0.0, late_seconds), still_cents, "none without it");
}

std::vector<Event> phrase() {
  constexpr std::size_t step = 4'800; // 0.1 s.
  std::vector<Event> events;
  const std::array<std::uint8_t, 6> keys{c4, e4, g4, a4, g4, e4};
  std::uint64_t id = 1;
  for (std::size_t i = 0; i < keys.size(); ++i) {
    append(events, note(i * step * 2, step * 3, keys[i], id++, static_cast<std::uint8_t>(loud)));
  }
  // A keyswitch to Staccato, then shorts with velocity.
  append(events, note(keys.size() * step * 2, step / 2, bow::first_keyswitch + 2, id++));
  for (std::size_t i = 0; i < keys.size(); ++i) {
    append(events, note((keys.size() + i) * step * 2 + step, step, keys[i], id++,
                        static_cast<std::uint8_t>(full - i * 10)));
  }
  return events;
}

void humanise_is_seeded() {
  install();
  const auto frames = frames_of(3.0);
  const auto played = [&](const std::vector<double>& v) {
    auto runtime = make_runtime(bow::engine(), v);
    return render(*runtime, phrase(), frames);
  };
  const auto a = played(rich());
  const auto b = played(rich());
  require(a.left == b.left && a.right == b.right, "the same phrase humanises the same");
  auto plain = rich();
  plain[at("bow.humanise")] = 0.0;
  const auto c = played(plain);
  require(a.left != c.left, "Humanise moves the players");
}

void block_splits_render_the_same() {
  install();
  const auto frames = frames_of(3.0);
  const auto played = [&](std::size_t size) {
    auto runtime = make_runtime(bow::engine(), rich());
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
  const auto frames = frames_of(3.0);
  auto runtime = make_runtime(bow::engine(), rich());
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
  for (const std::string_view articulation : {"Legato", "Sustain", "Pizzicato"}) {
    auto v = rich();
    v[at("bow.articulation")] = choice_of("bow.articulation", articulation);
    auto runtime = make_runtime(bow::engine(), v);
    constexpr std::uint64_t blocks = 800;
    constexpr std::uint64_t every = 16;
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
    // Prime: the first block adopts the values.
    (void)runtime->process(buffer, {}, 0, 1, {}, {});
    counted_allocations = 0;
    counting_allocations = true;
    for (std::uint64_t b = 1; b < blocks; ++b) {
      const auto hit = b / every;
      events[0].note_id_bits = {0, hit - 1};
      events[0].key = static_cast<std::uint8_t>(c4 + static_cast<int>((hit + keys - 1) % keys));
      events[1].note_id_bits = {0, hit};
      events[1].key = static_cast<std::uint8_t>(c4 + static_cast<int>(hit % keys));
      const auto due = b % every == 0 ? std::span(events) : std::span(events).subspan(0, 0);
      (void)runtime->process(buffer, due, b * block, 1, {}, {});
    }
    counting_allocations = false;
    REQUIRE_EQ(counted_allocations, 0,
               std::string(articulation) + ": the callback allocated " +
                   std::to_string(counted_allocations) + " times");
  }
}

void the_tail_bound_covers_a_released_note() {
  install();
  constexpr double tail_silence = 1e-5;
  constexpr double after_seconds = 0.1;
  const auto v = default_values(*bow::engine().descriptor); // Space 25 %.
  const auto tail = bow::engine().tail_frames(v, rate);
  REQUIRE_GT(tail, bow::engine().tail_frames(dry(), rate), "the hall lengthens the tail");
  auto runtime = make_runtime(bow::engine(), v);
  const auto off = frames_of(after_seconds);
  const auto x = mono(render(*runtime, note(0, off, c4, 1), off + tail + frames_of(after_seconds)));
  REQUIRE_GT(rms(x, off + frames_of(after_seconds), frames_of(after_seconds)), audible,
             "the release and the hall ring on");
  REQUIRE_LT(rms(x, off + tail, frames_of(after_seconds)), tail_silence,
             "silent after the tail bound");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "bow engine contract passed",
      {
          {"a_missing_library_is_silent_then_plays_once_installed",
           [] { a_missing_library_is_silent_then_plays_once_installed(); }},
          {"malformed_libraries_are_rejected", [] { malformed_libraries_are_rejected(); }},
          {"descriptor_registry_and_presets", [] { descriptor_registry_and_presets(); }},
          {"keys_are_in_tune_in_both_modes", [] { keys_are_in_tune_in_both_modes(); }},
          {"the_wheel_crossfades_the_layers", [] { the_wheel_crossfades_the_layers(); }},
          {"legato_joins_past_the_attack_without_a_click",
           [] { legato_joins_past_the_attack_without_a_click(); }},
          {"keyswitches_latch_the_articulation", [] { keyswitches_latch_the_articulation(); }},
          {"ensemble_layers_and_voices_are_bounded",
           [] { ensemble_layers_and_voices_are_bounded(); }},
          {"tone_runs_from_tasto_to_ponticello", [] { tone_runs_from_tasto_to_ponticello(); }},
          {"vibrato_blooms_after_its_delay", [] { vibrato_blooms_after_its_delay(); }},
          {"humanise_is_seeded", [] { humanise_is_seeded(); }},
          {"block_splits_render_the_same", [] { block_splits_render_the_same(); }},
          {"a_reset_plays_the_same_again", [] { a_reset_plays_the_same_again(); }},
          {"the_callback_does_not_allocate", [] { the_callback_does_not_allocate(); }},
          {"the_tail_bound_covers_a_released_note",
           [] { the_tail_bound_covers_a_released_note(); }},
      });
}
