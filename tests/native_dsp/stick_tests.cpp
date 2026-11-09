#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/stick.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "stick_test_library.hpp"
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
#include <string>
#include <string_view>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

// Stick's engine contract (ADR-0114), on a synthetic kit whose mics each
// play their own frequency and whose strokes each have hits at five powers
// (stick_test_library.hpp): a missing kit is silent, says why and plays
// once installed; malformed kits are rejected; the General MIDI map;
// velocity follows the recorded power; round robins are seeded and never
// repeat; the pedal opens the hat and closing chokes it; Bleed, mutes,
// solos, mixes and pans route each hit's mics; Tune and Decay; brushes; the
// stream joins the resident start sample for sample; voices are bounded;
// block splits and resets render the same; the callback never allocates;
// the tail bound covers a hit; a kit chosen loads in the background.
namespace {

using namespace oscillate;
using namespace oscillate::native_dsp;
using namespace oscillate::native_dsp::synth;
using namespace synth_test;
namespace library = stick::library;

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr double decibels = 20.0;
constexpr std::uint8_t kick = 36;
constexpr std::uint8_t sidestick = 37;
constexpr std::uint8_t snare = 38;
constexpr std::uint8_t hat_at_pedal = 42;
constexpr std::uint8_t hat_pedal = 44;
constexpr std::uint8_t hat_open = 46;
constexpr std::uint8_t crash = 49;
constexpr std::uint8_t loud = 110;
constexpr std::uint8_t full = 127;
constexpr std::size_t block = 128;
constexpr std::size_t window = 2'048;
// A reset fades the last output out over 1.5 ms; compare past it.
constexpr std::size_t declick_frames = 128;
// Below this a signal is silence.
constexpr double silence = 1e-6;
constexpr double audible = 1e-3;
// A mic's tone is absent this far under the loudest.
constexpr double absent_db = -50.0;
constexpr double bpm = 120.0;

const ParameterDescriptor& parameter(std::string_view id) {
  for (const auto& p : stick::engine().descriptor->parameters) {
    if (p.id == id) {
      return p;
    }
  }
  require(false, std::string("unknown parameter ") + std::string(id));
  return stick::engine().descriptor->parameters.front();
}

std::size_t at(std::string_view id) {
  const auto& all = stick::engine().descriptor->parameters;
  return static_cast<std::size_t>(&parameter(id) - all.data());
}

constexpr std::array<std::string_view, 12> pieces{"kick",   "snare", "hihat", "tom1",
                                                  "tom2",   "tom3",  "tom4",  "crash1",
                                                  "crash2", "ride",  "ride2", "china"};

// The kit as recorded: every piece natural, nothing humanised, round
// robins as asked, the master at unity.
std::vector<double> dry(bool robins = false) {
  auto v = default_values(*stick::engine().descriptor);
  v[at("stick.humanise")] = 0.0;
  v[at("stick.round_robin")] = robins ? 1.0 : 0.0;
  v[at("stick.level")] = 0.0;
  for (const auto piece : pieces) {
    v[at("stick." + std::string(piece) + ".decay")] = 1.0;
  }
  return v;
}

// Everything moving: humanised, round robins, the kit's own shortening.
std::vector<double> rich() {
  auto v = default_values(*stick::engine().descriptor);
  v[at("stick.humanise")] = 1.0;
  return v;
}

void install() { (void)stick_test::install(); }

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

double rms(const std::vector<double>& x, std::size_t from, std::size_t length) {
  double sum = 0.0;
  for (std::size_t i = from; i < from + length && i < x.size(); ++i) {
    sum += x[i] * x[i];
  }
  return std::sqrt(sum / static_cast<double>(std::max<std::size_t>(1, length)));
}

double peak_of(const std::vector<double>& x, std::size_t from, std::size_t length) {
  double most = 0.0;
  for (std::size_t i = from; i < from + length && i < x.size(); ++i) {
    most = std::max(most, std::fabs(x[i]));
  }
  return most;
}

double db(double ratio) { return decibels * std::log10(std::max(ratio, 1e-30)); }

std::vector<float> telemetry_of(NativeSynthRuntime& runtime) {
  std::vector<float> out;
  REQUIRE_TRUE(runtime.read_telemetry(out), "telemetry arrives");
  REQUIRE_GE(out.size(), stick::telemetry::size, "telemetry arrives");
  return out;
}

// One hit from silence, mono.
std::vector<double> hit(const std::vector<double>& values, std::uint8_t key,
                        std::uint8_t velocity = loud, double seconds = 0.6) {
  auto runtime = make_runtime(stick::engine(), values);
  return mono(render(*runtime, note(0, frames_of(seconds), key, 1, velocity), frames_of(seconds)));
}

// How loud each mic's tone is in a hit's first window, in dB under the
// loudest of them.
std::array<double, stick_test::mics.size()> mic_levels(const std::vector<double>& x,
                                                       std::string_view striker = "stick") {
  std::array<double, stick_test::mics.size()> out{};
  double most = 0.0;
  for (std::size_t m = 0; m < out.size(); ++m) {
    out[m] = magnitude(x, 0, window, stick_test::hz_of(m, striker));
    most = std::max(most, out[m]);
  }
  for (auto& level : out) {
    level = db(level / most);
  }
  return out;
}

// ---- The library ----------------------------------------------------------------------------

void a_missing_kit_is_silent_then_plays_once_installed() {
  const auto nowhere = wren_test::scratch_directory("stick-nothing-here");
  std::filesystem::create_directories(nowhere);
  ::setenv(std::string(library::directory_variable).c_str(), nowhere.c_str(), 1);
  const auto first = library::status(stick::Kit::drs);
  REQUIRE_EQ(first.state, library::State::loading, "the first look reads in the background");
  auto runtime = make_runtime(stick::engine(), dry());
  library::wait();
  const auto silent = mono(render(*runtime, note(0, frames_of(0.2), kick, 1), frames_of(0.3)));
  REQUIRE_EQ(rms(silent, 0, silent.size()), 0.0, "without its kit Stick is silent");
  REQUIRE_EQ(telemetry_of(*runtime)[stick::telemetry::library],
             static_cast<float>(library::State::missing), "and telemetry says it is missing");
  const auto status = library::status(stick::Kit::drs);
  REQUIRE_EQ(status.state, library::State::missing, "a missing kit says why");
  REQUIRE_TRUE(!status.message.empty(), "a missing kit says why");
  REQUIRE_EQ(status.directory, nowhere / library::facts(stick::Kit::drs).folder,
             "and where it looked");
  // Installed where it looked: the next look reads it, and the silent
  // Stick plays.
  stick_test::write_kit(nowhere / library::facts(stick::Kit::drs).folder, "DRS Studio");
  (void)library::status(stick::Kit::drs);
  library::wait();
  REQUIRE_EQ(library::status(stick::Kit::drs).state, library::State::ready, "installing is seen");
  const auto heard = mono(render(*runtime, note(0, frames_of(0.2), kick, 2), frames_of(0.3)));
  REQUIRE_GT(rms(heard, 0, heard.size()), audible, "the silent Stick plays once installed");
  REQUIRE_EQ(library::contents(stick::Kit::drs).toms, std::size_t{1}, "the kit's toms are counted");
}

void kits_chosen_load_in_the_background() {
  const auto base = wren_test::scratch_directory("stick-fetched-kits");
  stick_test::write_kits(base);
  ::setenv(std::string(library::directory_variable).c_str(), base.c_str(), 1);
  auto v = dry();
  v[at("stick.kit")] = static_cast<double>(stick::Kit::garage);
  // Creating the runtime fetches the kit its values choose; an offline
  // render waits for it before its first block.
  auto runtime = make_runtime(stick::engine(), v);
  const auto x = mono(render(*runtime, note(0, frames_of(0.2), kick, 1), frames_of(0.3)));
  REQUIRE_GT(rms(x, 0, x.size()), audible, "the chosen kit plays from the first block");
  const auto telemetry = telemetry_of(*runtime);
  REQUIRE_EQ(telemetry[stick::telemetry::kit], static_cast<float>(stick::Kit::garage),
             "telemetry names the kit");
  REQUIRE_EQ(library::status(stick::Kit::drs).state, library::State::loading,
             "a kit nobody chose is read only when looked at");
  library::wait();
}

std::string with_line(const std::string& text, std::string_view tag, const std::string& line) {
  const auto start = text.find(tag);
  const auto end = text.find('\n', start);
  return text.substr(0, start) + line + text.substr(end);
}

void malformed_kits_are_rejected() {
  const auto base = wren_test::scratch_directory("stick-malformed");
  stick_test::write_kit(base, "broken");
  const auto manifest = base / library::manifest_file;
  std::string text;
  {
    std::ifstream in(manifest);
    text.assign(std::istreambuf_iterator<char>(in), {});
  }
  const auto broken = [&](const std::string& replaced) {
    std::ofstream(manifest) << replaced;
    return library::load(stick::Kit::drs, base).state;
  };
  const auto& invalid = library::State::invalid;
  REQUIRE_EQ(broken("not a stick kit\n" + text), invalid, "format line");
  REQUIRE_EQ(broken(with_line(text, "channels ", "channels 17")), invalid, "seventeen mics");
  REQUIRE_EQ(broken(with_line(text, "mic 0 ", "mic 9 kick_in kick mono")), invalid,
             "a mic past the channels");
  REQUIRE_EQ(broken(with_line(text, "mic 0 ", "mic 0 overheads kick mono")), invalid,
             "an overhead that hears one piece");
  REQUIRE_EQ(broken(with_line(text, "mic 0 ", "mic 0 kick_in kick left")), invalid,
             "a close mic in a pair");
  REQUIRE_EQ(broken(with_line(text, "mic 0 ", "")), invalid, "a channel without its mic");
  REQUIRE_EQ(broken(with_line(text, "hit kick hit", "hit kick bell stick 0 10 1e-4 -6")), invalid,
             "a kick's bell");
  REQUIRE_EQ(broken(with_line(text, "hit kick hit", "hit kick hit stick 0 0 1e-4 -6")), invalid,
             "an empty hit");
  REQUIRE_EQ(broken(with_line(text, "hit kick hit", "hit kick hit stick 0 10 2 -6")), invalid,
             "a scale over one");
  REQUIRE_EQ(broken(with_line(text, "hit kick hit", "hit kick hit chopstick 0 10 1e-4 -6")),
             invalid, "an unknown striker");
  std::ofstream(manifest) << text;
  REQUIRE_EQ(library::load(stick::Kit::drs, base).state, library::State::ready,
             "the untouched manifest loads");
  std::filesystem::resize_file(base / "stick.pack",
                               std::filesystem::file_size(base / "stick.pack") / 2);
  REQUIRE_EQ(library::load(stick::Kit::drs, base).state, invalid, "a truncated pack");
}

void descriptor_registry_and_presets() {
  constexpr std::array<std::string_view, 6> presets{"Tight Funk",   "Dry Break",   "Room Kit",
                                                    "Garage Lo-Fi", "Brushy Soft", "As Recorded"};
  const auto& descriptor = *stick::engine().descriptor;
  REQUIRE_EQ(find_engine("stick"), &stick::engine(), "Stick is registered");
  REQUIRE_OK(validate_engine(descriptor), "the table validates");
  REQUIRE_EQ(descriptor.presets.size(), presets.size(), "the presets");
  for (std::size_t i = 0; i < presets.size(); ++i) {
    REQUIRE_EQ(descriptor.presets[i].name, presets[i], "the presets, in order");
    REQUIRE_OK(preset_values(descriptor, descriptor.presets[i]), "every preset resolves");
  }
  REQUIRE_TRUE(descriptor.presets.front().values.empty(), "Tight Funk is the defaults");
  require(parameter("stick.perf.pedal").automatable, "the hi-hat pedal is an automation lane");
  require(!parameter("stick.snare.tune").automatable, "Tune is read as a hit starts");
  // The General MIDI map, its toms spread over the kit's.
  // A key the map plays: required, and then read.
  const auto mapped = [](int key, std::size_t toms) {
    const auto found = stick::map_key(key, toms);
    REQUIRE_TRUE(found.has_value(), "key " + std::to_string(key) + " is mapped");
    return found.value_or(stick::Mapped{});
  };
  REQUIRE_EQ(mapped(kick, 3).piece, stick::Piece::kick, "C2 (36) is the kick");
  REQUIRE_EQ(mapped(sidestick, 3).articulation, stick::Articulation::sidestick,
             "C#2 is the side-stick");
  const bool hat_follows_pedal = mapped(hat_at_pedal, 3).at_pedal;
  REQUIRE_TRUE(hat_follows_pedal, "F#2 plays the hat at the pedal");
  REQUIRE_EQ(mapped(hat_open, 3).articulation, stick::Articulation::open, "A#2 is open");
  constexpr int high_tom = 50;
  constexpr int low_floor = 41;
  constexpr int low_mid = 47;
  REQUIRE_EQ(mapped(high_tom, 3).piece, stick::Piece::tom1, "D3 the highest tom");
  REQUIRE_EQ(mapped(low_floor, 3).piece, stick::Piece::tom3, "F2 the lowest of three");
  REQUIRE_EQ(mapped(low_floor, 4).piece, stick::Piece::tom4, "or of four");
  REQUIRE_EQ(mapped(low_mid, 2).piece, stick::Piece::tom1, "two toms split the six keys");
  REQUIRE_FALSE(stick::map_key(low_floor, 0).has_value(), "no toms, no tom keys");
  REQUIRE_FALSE(stick::map_key(stick::last_key + 1, 3).has_value(), "nothing above the map");
  require(stick::plays(stick::Piece::ride, stick::Articulation::bell) &&
              !stick::plays(stick::Piece::kick, stick::Articulation::bell),
          "only the rides have bells");
}

// ---- Playing -----------------------------------------------------------------------------------

void velocity_follows_the_recorded_power() {
  install();
  constexpr std::array<std::uint8_t, 9> velocities{8, 24, 40, 56, 72, 88, 104, 118, full};
  constexpr double probe_seconds = 0.05;
  constexpr double spread_db = 24.0;
  for (const bool robins : {false, true}) {
    double previous = 0.0;
    double softest = 0.0;
    for (const auto velocity : velocities) {
      const double level = peak_of(hit(dry(robins), kick, velocity), 0, frames_of(probe_seconds));
      REQUIRE_GE(level, previous, "louder velocities play louder");
      previous = level;
      softest = softest == 0.0 ? level : softest;
    }
    REQUIRE_GT(db(previous / softest), spread_db, "velocity spans the recorded powers");
  }
  // A soft curve lifts a light touch; a hard one holds it down.
  constexpr std::uint8_t light = 40;
  auto soft = dry();
  soft[at("stick.kick.curve")] = -stick::max_curve;
  auto hard = dry();
  hard[at("stick.kick.curve")] = stick::max_curve;
  const auto level = [&](const std::vector<double>& v) {
    return peak_of(hit(v, kick, light), 0, frames_of(probe_seconds));
  };
  REQUIRE_GT(level(soft), level(dry()), "a soft curve lifts a light touch");
  REQUIRE_LT(level(hard), level(dry()), "a hard curve holds it down");
}

// A run of identical notes, each long enough to ring out alone.
std::vector<std::vector<double>> repeated(const std::vector<double>& values, std::size_t notes) {
  constexpr double apart_seconds = 0.7;
  // Velocity 102 asks for the -12 dB hits: their two round robins.
  constexpr std::uint8_t middle = 102;
  const auto apart = frames_of(apart_seconds);
  std::vector<Event> events;
  for (std::size_t n = 0; n < notes; ++n) {
    append(events, note(n * apart, apart / 2, snare, n + 1, middle));
  }
  auto runtime = make_runtime(stick::engine(), values);
  const auto x = mono(render(*runtime, events, notes * apart));
  std::vector<std::vector<double>> hits;
  hits.reserve(notes);
  for (std::size_t n = 0; n < notes; ++n) {
    hits.emplace_back(x.begin() + static_cast<std::ptrdiff_t>(n * apart),
                      x.begin() + static_cast<std::ptrdiff_t>(n * apart + window));
  }
  return hits;
}

void round_robins_are_seeded_and_never_repeat() {
  install();
  constexpr std::size_t notes = 6;
  const auto on = repeated(dry(true), notes);
  for (std::size_t n = 1; n < notes; ++n) {
    require(on[n] != on[n - 1], "a round robin never repeats the hit before");
  }
  require(on == repeated(dry(true), notes), "the same notes play the same hits again");
  const auto off = repeated(dry(false), notes);
  for (std::size_t n = 1; n < notes; ++n) {
    require(off[n] == off[0], "without round robins every note plays the nearest hit");
  }
}

void the_pedal_opens_the_hat_and_closing_chokes_it() {
  install();
  constexpr double late_seconds = 0.4;
  const auto at_pedal = [&](double pedal) {
    auto v = dry();
    v[at("stick.perf.pedal")] = pedal;
    const auto x = hit(v, hat_at_pedal, loud, 0.6);
    return rms(x, frames_of(late_seconds), window);
  };
  const double closed = at_pedal(0.0);
  const double half = at_pedal(0.5);
  const double open = at_pedal(1.0);
  REQUIRE_LT(closed, silence, "a closed hat is short");
  REQUIRE_GT(open, audible, "an open hat rings");
  REQUIRE_GT(half, closed, "half open rings between");
  REQUIRE_LT(half, open, "and shorter than open");
  // A closed stroke or the pedal chokes the ringing open hat.
  constexpr double choke_seconds = 0.3;
  constexpr double after_seconds = 0.5;
  const auto ringing = [&](std::uint8_t closing) {
    std::vector<Event> events = note(0, frames_of(0.1), hat_open, 1, loud);
    if (closing != 0) {
      append(events, note(frames_of(choke_seconds), frames_of(0.05), closing, 2, loud));
    }
    auto runtime = make_runtime(stick::engine(), dry());
    const auto x = mono(render(*runtime, events, frames_of(0.7)));
    return rms(x, frames_of(after_seconds), window);
  };
  constexpr double choked_db = -40.0;
  const double free = ringing(0);
  REQUIRE_LT(db(ringing(hat_at_pedal) / free), choked_db, "a closed stroke chokes it");
  REQUIRE_LT(db(ringing(hat_pedal) / free), choked_db, "the pedal chokes it");
  // Closing the pedal lane chokes it too.
  auto open_pedal = dry();
  open_pedal[at("stick.perf.pedal")] = 1.0;
  auto runtime = make_runtime(stick::engine(), open_pedal);
  (void)render(*runtime, note(0, frames_of(0.1), hat_open, 1, loud), frames_of(choke_seconds));
  auto closed_pedal = open_pedal;
  closed_pedal[at("stick.perf.pedal")] = 0.0;
  REQUIRE_TRUE(runtime->offer_values(closed_pedal, 2), "the pedal closes");
  const auto x = mono(render(*runtime, {}, frames_of(choke_seconds)));
  REQUIRE_LT(db(rms(x, frames_of(0.2), window) / free), choked_db, "closing the pedal chokes it");
  REQUIRE_EQ(telemetry_of(*runtime)[stick::telemetry::pedal], 0.0F, "telemetry follows the pedal");
}

void bleed_mutes_solos_and_mixes_route_the_mics() {
  install();
  using stick_test::kick_in;
  using stick_test::overhead_left;
  using stick_test::room_left;
  using stick_test::snare_top;
  auto v = dry();
  auto levels = mic_levels(hit(v, snare));
  REQUIRE_GT(levels[kick_in], absent_db, "with Bleed the kick mic hears the snare");
  REQUIRE_GT(levels[room_left], absent_db, "and the room does");
  v[at("stick.bleed")] = 0.0;
  levels = mic_levels(hit(v, snare));
  REQUIRE_LT(levels[kick_in], absent_db, "without Bleed the snare keeps to its own mics");
  REQUIRE_GT(levels[overhead_left], absent_db, "and the overheads");
  REQUIRE_GT(levels[room_left], absent_db, "and the room");
  v = dry();
  v[at("stick.mic.overheads.mute")] = 1.0;
  levels = mic_levels(hit(v, snare));
  REQUIRE_LT(levels[overhead_left], absent_db, "a muted pair is silent");
  v = dry();
  v[at("stick.mic.snare_top.solo")] = 1.0;
  levels = mic_levels(hit(v, snare));
  for (std::size_t m = 0; m < levels.size(); ++m) {
    if (m != snare_top) {
      REQUIRE_LT(levels[m], absent_db, "a solo silences the other mics");
    }
  }
  v = dry();
  v[at("stick.mix")] = static_cast<double>(stick::Mix::dry);
  levels = mic_levels(hit(v, snare));
  REQUIRE_LT(levels[room_left], absent_db, "the Dry mix leaves the room out");
}

void pans_place_close_mics_and_balance_cymbals() {
  install();
  auto v = dry();
  v[at("stick.snare.pan")] = -1.0;
  auto runtime = make_runtime(stick::engine(), v);
  auto r = render(*runtime, note(0, frames_of(0.2), snare, 1), frames_of(0.3));
  const double top_hz = stick_test::mics[stick_test::snare_top].hz;
  REQUIRE_LT(db(magnitude(r.right, 0, window, top_hz) / magnitude(r.left, 0, window, top_hz)),
             absent_db, "a snare panned left keeps its own mic left");
  v = dry();
  v[at("stick.crash1.pan")] = 1.0;
  runtime = make_runtime(stick::engine(), v);
  r = render(*runtime, note(0, frames_of(0.2), crash, 1), frames_of(0.3));
  const double left_hz = stick_test::mics[stick_test::overhead_left].hz;
  REQUIRE_LT(db(magnitude(r.left, 0, window, left_hz) /
                magnitude(r.right, 0, window, stick_test::mics[stick_test::overhead_right].hz)),
             absent_db, "a cymbal without its own mic pans by its overheads");
}

void tune_decay_and_brushes() {
  install();
  const double top_hz = stick_test::mics[stick_test::snare_top].hz;
  constexpr double octave = 2.0;
  auto v = dry();
  v[at("stick.snare.tune")] = stick::max_tune_semitones;
  const auto up = hit(v, snare);
  REQUIRE_LT(db(magnitude(up, 0, window, top_hz) / magnitude(up, 0, window, top_hz * octave)),
             absent_db, "Tune up an octave doubles every mic's pitch");
  constexpr double late_seconds = 0.3;
  constexpr double shorter_db = -20.0;
  const double natural = rms(hit(dry(), snare), frames_of(late_seconds), window);
  v = dry();
  v[at("stick.snare.decay")] = stick::min_decay * 2.0;
  REQUIRE_LT(db(rms(hit(v, snare), frames_of(late_seconds), window) / natural), shorter_db,
             "Decay shortens the hit");
  v = dry();
  v[at("stick.striker")] = static_cast<double>(stick::Striker::brush);
  const auto brushed = mic_levels(hit(v, snare), "brush");
  REQUIRE_GT(brushed[stick_test::snare_top], absent_db, "brushes play the brush recordings");
  require(hit(v, kick) == hit(dry(), kick), "a kick has no brushes: its beater plays");
}

// The kick at full velocity, its own mic soloed and centred: every output
// frame is its recording, through the resident start and on into the
// stream.
void the_stream_joins_the_resident_start() {
  install();
  auto v = dry();
  v[at("stick.mic.kick_in.solo")] = 1.0;
  v[at("stick.kick.pan")] = 0.0;
  auto runtime = make_runtime(stick::engine(), v);
  const auto frames = frames_of(stick_test::drum_seconds);
  const auto r = render(*runtime, note(0, frames, kick, 1, full), frames, block);
  const auto& s = stick_test::strokes.front();
  constexpr std::size_t loudest = stick_test::powers_db.size() - 1;
  const double peak = std::pow(10.0, stick_test::powers_db[loudest] / decibels);
  constexpr double tolerance = 1e-6;
  for (std::size_t i = 0; i < frames; ++i) {
    const double x =
        stick_test::hit_value(s, loudest, 0, stick_test::kick_in, static_cast<double>(i)) / peak;
    const double expected =
        static_cast<double>(std::lround(std::clamp(x, -1.0, 1.0) * stick_test::full_scale)) *
        (peak / stick_test::full_scale);
    if (std::fabs(r.left[i] - expected) > tolerance) {
      require(false, "frame " + std::to_string(i) + " is not the recording's");
    }
  }
  REQUIRE_EQ(telemetry_of(*runtime)[stick::telemetry::underruns], 0.0F,
             "an offline render never waits on the disk");
}

void voices_are_bounded() {
  install();
  constexpr std::size_t crashes = 12;
  constexpr std::size_t apart = 2'400;
  constexpr std::uint32_t crash_voices = 3;
  std::vector<Event> events;
  for (std::size_t n = 0; n < crashes; ++n) {
    append(events, note(n * apart, apart, crash, n + 1));
  }
  auto runtime = make_runtime(stick::engine(), dry());
  constexpr std::size_t settle = 4'800;
  REQUIRE_LE(render(*runtime, events, crashes * apart + settle).last.active_voices, crash_voices,
             "a cymbal rings at most three times over");
  events.clear();
  constexpr std::size_t notes = 96;
  constexpr int keys = stick::last_key - stick::first_key + 1;
  for (std::size_t n = 0; n < notes; ++n) {
    append(events,
           note(n * 64, 64,
                static_cast<std::uint8_t>(stick::first_key + static_cast<int>(n) % keys), n + 1));
  }
  constexpr std::uint32_t max_voices = 32;
  runtime = make_runtime(stick::engine(), rich());
  REQUIRE_LE(render(*runtime, events, notes * 64).last.active_voices, max_voices,
             "voices are bounded");
}

void telemetry_lights_the_piece_and_its_mics() {
  install();
  auto runtime = make_runtime(stick::engine(), dry());
  (void)render(*runtime, note(0, frames_of(0.05), snare, 1, full), frames_of(0.05));
  const auto t = telemetry_of(*runtime);
  namespace tm = stick::telemetry;
  REQUIRE_EQ(t[tm::last_piece], static_cast<float>(stick::Piece::snare), "the snare was hit");
  REQUIRE_GT(t[tm::piece_level + static_cast<std::size_t>(stick::Piece::snare)], 0.0F,
             "the snare lights");
  REQUIRE_EQ(t[tm::piece_level + static_cast<std::size_t>(stick::Piece::kick)], 0.0F,
             "the kick does not");
  REQUIRE_GT(t[tm::mic_db + static_cast<std::size_t>(stick::MicGroup::snare_top)],
             t[tm::mic_db + static_cast<std::size_t>(stick::MicGroup::kick_in)],
             "the snare's mic reads louder than the kick's");
  REQUIRE_EQ(t[tm::hits], 1.0F, "one hit");
}

std::vector<Event> groove() {
  constexpr std::size_t sixteenth = 6'000; // 120 BPM.
  constexpr std::array<std::uint8_t, 16> keys{
      kick, hat_at_pedal, hat_at_pedal, snare, hat_at_pedal, kick, hat_open,  snare,
      kick, hat_at_pedal, crash,        snare, hat_pedal,    kick, sidestick, snare};
  std::vector<Event> events;
  for (std::size_t i = 0; i < keys.size(); ++i) {
    append(events, note(i * sixteenth, sixteenth / 2, keys[i], i + 1,
                        static_cast<std::uint8_t>(full - (i % 4) * 20)));
  }
  return events;
}

void humanise_is_seeded() {
  install();
  const auto frames = frames_of(2.5);
  const auto played = [&](const std::vector<double>& v) {
    auto runtime = make_runtime(stick::engine(), v);
    return render(*runtime, groove(), frames);
  };
  const auto a = played(rich());
  require(a.left == played(rich()).left, "the same groove humanises the same");
  auto plain = rich();
  plain[at("stick.humanise")] = 0.0;
  require(a.left != played(plain).left, "Humanise moves the hits");
}

void block_splits_render_the_same() {
  install();
  const auto frames = frames_of(2.5);
  const auto played = [&](std::size_t size) {
    auto runtime = make_runtime(stick::engine(), rich());
    return render(*runtime, groove(), frames, size);
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
  const auto frames = frames_of(2.5);
  auto runtime = make_runtime(stick::engine(), rich());
  const auto first = render(*runtime, groove(), frames);
  constexpr std::uint64_t seek = 2;
  const auto again = render(*runtime, groove(), frames, block, rate, bpm, {}, seek);
  require(std::equal(again.left.begin() + static_cast<std::ptrdiff_t>(declick_frames),
                     again.left.end(),
                     first.left.begin() + static_cast<std::ptrdiff_t>(declick_frames)),
          "a reset renders the same again, past the reset's declick: export equals playback");
}

void the_callback_does_not_allocate() {
  install();
  auto runtime = make_runtime(stick::engine(), rich());
  constexpr std::uint64_t blocks = 800;
  constexpr std::uint64_t every = 6;
  std::vector<engine::StereoFrame> buffer(block);
  std::vector<engine::BlockMidiEvent> events(1);
  events[0].clip_id_bits = {1, 1};
  events[0].velocity = loud;
  events[0].kind = engine::MidiEventKind::note_on;
  events[0].offset = block / 2;
  const auto keys = groove();
  // Prime: the first block adopts the values.
  runtime->await_content();
  (void)runtime->process(buffer, {}, 0, 1, {}, {});
  counted_allocations = 0;
  counting_allocations = true;
  for (std::uint64_t b = 1; b < blocks; ++b) {
    const auto hit_index = b / every;
    events[0].note_id_bits = {0, hit_index};
    events[0].key = keys[(hit_index * 2) % keys.size()].key;
    const auto due = b % every == 0 ? std::span(events) : std::span(events).subspan(0, 0);
    (void)runtime->process(buffer, due, b * block, 1, {}, {});
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0,
             "the callback allocated " + std::to_string(counted_allocations) + " times");
}

void the_tail_bound_covers_a_hit() {
  install();
  constexpr double tail_silence = 1e-5;
  constexpr double after_seconds = 0.1;
  auto v = dry();
  v[at("stick.crash1.tune")] = -stick::max_tune_semitones;
  const auto tail = stick::engine().tail_frames(v, rate);
  REQUIRE_GT(tail, stick::engine().tail_frames(dry(), rate), "a lower Tune lengthens the tail");
  auto runtime = make_runtime(stick::engine(), v);
  const auto off = frames_of(after_seconds);
  const auto x = mono(render(*runtime, note(0, off, crash, 1, full), off + tail + off));
  REQUIRE_GT(rms(x, off, off), audible, "the cymbal rings on");
  REQUIRE_LT(rms(x, off + tail, off), tail_silence, "silent after the tail bound");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "stick engine contract passed",
      {
          {"a_missing_kit_is_silent_then_plays_once_installed",
           [] { a_missing_kit_is_silent_then_plays_once_installed(); }},
          {"kits_chosen_load_in_the_background", [] { kits_chosen_load_in_the_background(); }},
          {"malformed_kits_are_rejected", [] { malformed_kits_are_rejected(); }},
          {"descriptor_registry_and_presets", [] { descriptor_registry_and_presets(); }},
          {"velocity_follows_the_recorded_power", [] { velocity_follows_the_recorded_power(); }},
          {"round_robins_are_seeded_and_never_repeat",
           [] { round_robins_are_seeded_and_never_repeat(); }},
          {"the_pedal_opens_the_hat_and_closing_chokes_it",
           [] { the_pedal_opens_the_hat_and_closing_chokes_it(); }},
          {"bleed_mutes_solos_and_mixes_route_the_mics",
           [] { bleed_mutes_solos_and_mixes_route_the_mics(); }},
          {"pans_place_close_mics_and_balance_cymbals",
           [] { pans_place_close_mics_and_balance_cymbals(); }},
          {"tune_decay_and_brushes", [] { tune_decay_and_brushes(); }},
          {"the_stream_joins_the_resident_start", [] { the_stream_joins_the_resident_start(); }},
          {"voices_are_bounded", [] { voices_are_bounded(); }},
          {"telemetry_lights_the_piece_and_its_mics",
           [] { telemetry_lights_the_piece_and_its_mics(); }},
          {"humanise_is_seeded", [] { humanise_is_seeded(); }},
          {"block_splits_render_the_same", [] { block_splits_render_the_same(); }},
          {"a_reset_plays_the_same_again", [] { a_reset_plays_the_same_again(); }},
          {"the_callback_does_not_allocate", [] { the_callback_does_not_allocate(); }},
          {"the_tail_bound_covers_a_hit", [] { the_tail_bound_covers_a_hit(); }},
      });
}
