#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/pulse.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/signal.hpp"
#include "synth_test_support.hpp"

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using namespace synth_test;

const SynthEngine& pulse_engine() { return pulse::engine(); }
const EngineDescriptor& table() { return *pulse_engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }
std::vector<double> kit(std::string_view name) { return preset(table(), name); }

constexpr std::uint8_t kick_key = 36;
constexpr std::uint8_t snare_key = 38;
constexpr std::uint8_t closed_hat_key = 42;
constexpr std::uint8_t open_hat_key = 46;

// A dry kit: no bus processing, no drives, no click.
std::vector<double> dry_kit() {
  auto values = kit("Init");
  values[at("pulse.kick.character")] = 0;
  values[at("pulse.kick.tone")] = 0;
  return values;
}

using oscillate::test::rms;

double db(double value) {
  return oscillate::test::gain_to_db(value, oscillate::test::silence_floor);
}

// One bar-ish pattern hitting every pad once, staggered.
std::vector<Event> all_pads() {
  std::vector<Event> events;
  for (std::uint8_t pad = 0; pad < pulse::pad_count; ++pad) {
    auto n = note(static_cast<std::uint64_t>(pad) * 3'600, 2'000,
                  static_cast<std::uint8_t>(pulse::first_key + pad), pad + 1U, 110);
    events.insert(events.end(), n.begin(), n.end());
  }
  return events;
}

void descriptor_registry_and_kits() {
  const auto valid = validate_engine(table());
  REQUIRE_OK(valid, valid.has_value() ? "" : valid.error().message);
  REQUIRE_EQ(table().id, "pulse", "pulse v1 with twelve kits");
  REQUIRE_EQ(table().presets.size(), 12, "pulse v1 with twelve kits");
  REQUIRE_EQ(find_engine("pulse"), &pulse_engine(), "Pulse is the fourth registered engine");
  REQUIRE_GE(engines().size(), 4, "Pulse is the fourth registered engine");
  REQUIRE_EQ(engines()[3], &pulse_engine(), "Pulse is the fourth registered engine");
}

void pads_map_from_c1_and_other_keys_are_silent() {
  auto runtime = make_runtime(pulse_engine(), kit("808 Classic"));
  auto out = render(*runtime, note(0, 100, kick_key, 1), 2'000);
  REQUIRE_EQ(out.last.active_voices, 1, "C1 plays the kick");
  REQUIRE_GT(peak(out.left), 0.05, "C1 plays the kick");
  auto silent = make_runtime(pulse_engine(), kit("808 Classic"));
  auto events = note(0, 100, 35, 1);
  auto above = note(0, 100, 48, 2);
  events.insert(events.end(), above.begin(), above.end());
  out = render(*silent, events, 2'000);
  REQUIRE_EQ(out.last.active_voices, 0, "unmapped keys are silent");
  REQUIRE_EQ(peak(out.left), 0.0, "unmapped keys are silent");
}

void kick_pitch_and_decay_are_exact() {
  for (double tune : {0.0, 12.0}) {
    auto values = dry_kit();
    values[at("pulse.kick.tune")] = tune;
    values[at("pulse.kick.decay")] = 2.0;
    auto runtime = make_runtime(pulse_engine(), values);
    const auto out = render(*runtime, note(0, 100, kick_key, 1), 48'000);
    const double expected = 50.0 * std::exp2(tune / 12.0);
    REQUIRE_LT(std::fabs(cents(measured_hz(out.left, 4'800, 48'000, 48'000.0), expected)), 17.0,
               "kick body sits on its tuned pitch (within 1%)");
  }
  auto values = dry_kit();
  values[at("pulse.kick.decay")] = 0.5;
  auto runtime = make_runtime(pulse_engine(), values);
  const auto out = render(*runtime, note(0, 100, kick_key, 1), 48'000);
  // 40 ms windows (two periods of 50 Hz) around the start and the decay time.
  const double start = rms(out.left, 960, 2'880);
  const double later = rms(out.left, 24'000 - 960, 24'000 + 960);
  REQUIRE_LT(std::fabs(db(later / start) + 60.0 * (24'000.0 - 1'920.0) / 24'000.0), 3.0,
             "kick decays at -60 dB per decay time");
}

// Tune follows a ringing kick without restarting it: the body moves to the
// new pitch and keeps sounding.
void kick_retunes_while_it_rings() {
  constexpr double rate = 48'000.0;
  constexpr std::size_t block = 128;
  constexpr std::uint64_t change_at = 4'864;
  constexpr std::uint64_t total = 48'000;
  constexpr double octave_up = 12.0;
  constexpr double kick_hz = 50.0;
  constexpr double pitch_tolerance_cents = 17.0;
  auto values = dry_kit();
  values[at("pulse.kick.decay")] = 2.0;
  auto runtime = make_runtime(pulse_engine(), values);
  (void)render(*runtime, note(0, 100, kick_key, 1), change_at);
  auto raised = values;
  raised[at("pulse.kick.tune")] = octave_up;
  require(runtime->offer_values(raised, 1), "tune change offered");
  std::vector<double> out;
  std::vector<engine::StereoFrame> buffer(block);
  for (std::uint64_t frame = change_at; frame < total; frame += block) {
    buffer.assign(block, {0.0F, 0.0F});
    (void)runtime->process(buffer, {}, frame, 1, {}, {});
    for (const auto& sample : buffer)
      out.push_back(sample.left);
  }
  const double expected = kick_hz * std::exp2(octave_up / 12.0);
  REQUIRE_LT(std::fabs(cents(measured_hz(out, 4'800, out.size(), rate), expected)),
             pitch_tolerance_cents, "a ringing kick did not follow its Tune");
}

void hats_have_no_low_end_and_the_closed_hat_chokes() {
  auto runtime = make_runtime(pulse_engine(), dry_kit());
  const auto out = render(*runtime, note(0, 100, open_hat_key, 1), 16'384);
  std::vector<std::complex<double>> data(16'384);
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = out.left[i];
  }
  fft(data);
  double low = 0.0;
  double high = 0.0;
  const double bin_hz = 48'000.0 / static_cast<double>(data.size());
  for (std::size_t bin = 1; bin < data.size() / 2; ++bin) {
    const double hz = static_cast<double>(bin) * bin_hz;
    if (hz < 1'000.0) {
      low += std::norm(data[bin]);
    } else if (hz > 6'000.0) {
      high += std::norm(data[bin]);
    }
  }
  REQUIRE_LT(10.0 * std::log10(low / high), -30.0, "hats live above 6 kHz, not below 1 kHz");

  auto alone = make_runtime(pulse_engine(), dry_kit());
  auto ringing = render(*alone, note(0, 100, open_hat_key, 1), 9'600);
  REQUIRE_EQ(ringing.last.active_voices, 1, "an open hat rings on its own");
  auto choked = make_runtime(pulse_engine(), dry_kit());
  auto events = note(0, 100, open_hat_key, 1);
  auto closed = note(2'400, 100, closed_hat_key, 2);
  events.insert(events.end(), closed.begin(), closed.end());
  const auto both = render(*choked, events, 9'600);
  REQUIRE_EQ(both.last.active_voices, 0, "the closed hat chokes the open hat");
}

void retriggers_never_click() {
  auto values = dry_kit();
  values[at("pulse.kick.decay")] = 2.0;
  auto runtime = make_runtime(pulse_engine(), values);
  std::vector<Event> events;
  for (std::uint64_t hit = 0; hit < 6; ++hit) {
    auto n = note(hit * 4'801, 100, kick_key, hit + 1);
    events.insert(events.end(), n.begin(), n.end());
  }
  const auto out = render(*runtime, events, 28'800);
  double steady = 0.0;
  for (std::size_t i = 2'000; i < 4'700; ++i) {
    steady = std::max(steady, std::fabs(out.left[i] - out.left[i - 1]));
  }
  double around = 0.0;
  for (std::size_t hit = 1; hit < 6; ++hit) {
    const std::size_t at_frame = hit * 4'801;
    for (std::size_t i = at_frame; i < at_frame + 100; ++i) {
      around = std::max(around, std::fabs(out.left[i] - out.left[i - 1]));
    }
  }
  REQUIRE_LT(around, 2.0 * steady, "a retriggered pad declicks");
}

void block_splits_and_tails() {
  for (std::string_view name : {"808 Classic", "909 Techno", "Dub", "Breaks"}) {
    auto a = make_runtime(pulse_engine(), kit(name));
    auto b = make_runtime(pulse_engine(), kit(name));
    const auto x = render(*a, all_pads(), 48'000, 64);
    const auto y = render(*b, all_pads(), 48'000, 0);
    require(x.left == y.left && x.right == y.right,
            std::string(name) + ": block splits render identically");
    const auto values = kit(name);
    const auto tail = pulse_engine().tail_frames(values, 48'000.0);
    auto c = make_runtime(pulse_engine(), values);
    const auto out = render(*c, note(0, 100, 47, 1), tail + 9'600);
    require(peak(out.left, tail) < 1e-3 && peak(out.right, tail) < 1e-3,
            std::string(name) + ": a cymbal settles inside the tail bound");
  }
}

void kits_render_sane_audio() {
  for (const auto& entry : table().presets) {
    auto runtime = make_runtime(pulse_engine(), kit(entry.name));
    const auto out = render(*runtime, all_pads(), 48'000);
    double power = 0.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < out.left.size(); ++i) {
      REQUIRE_TRUE(std::isfinite(out.left[i]), "kit is finite");
      REQUIRE_TRUE(std::isfinite(out.right[i]), "kit is finite");
      power += out.left[i] * out.left[i] + out.right[i] * out.right[i];
      sum += out.left[i] + out.right[i];
    }
    const double rms_db = 10.0 * std::log10(power / (2.0 * 48'000.0));
    std::cout << "  " << entry.name << ": rms " << rms_db << " dBFS, peak "
              << db(std::max(peak(out.left), peak(out.right))) << " dBFS\n";
    REQUIRE_LE(std::max(peak(out.left), peak(out.right)), 1.0, "kit never exceeds 0 dBFS");
    REQUIRE_GT(rms_db, -40.0, "kit is audible and not slammed");
    REQUIRE_LT(rms_db, -6.0, "kit is audible and not slammed");
    REQUIRE_LT(std::fabs(sum / (2.0 * 48'000.0)), 0.01, "kit has no DC offset");
  }
}

void every_pad_at_once_does_not_allocate() {
  auto runtime = make_runtime(pulse_engine(), kit("Industrial"));
  std::vector<engine::StereoFrame> buffer(128);
  std::vector<engine::BlockMidiEvent> hits(pulse::pad_count);
  for (std::uint8_t pad = 0; pad < pulse::pad_count; ++pad) {
    hits[pad].clip_id_bits = {1, 1};
    hits[pad].note_id_bits = {0, pad + 1U};
    hits[pad].kind = engine::MidiEventKind::note_on;
    hits[pad].key = static_cast<std::uint8_t>(pulse::first_key + pad);
    hits[pad].velocity = 120;
  }
  counting_allocations = true;
  counted_allocations = 0;
  KernelStatus status{};
  for (std::uint64_t block = 0; block < 32; ++block) {
    status = runtime
                 ->process(buffer, block == 0 ? std::span(hits) : std::span(hits).subspan(0, 0),
                           block * 128, 1, {}, {})
                 .kernel;
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "the Pulse callback never allocates");
  REQUIRE_GE(status.active_voices, 10, "every pad rang at once");
}

Fingerprint pattern_fingerprint(std::string_view name) {
  auto runtime = make_runtime(pulse_engine(), kit(name));
  const auto out = render(*runtime, all_pads(), 96'000);
  std::vector<double> mono(out.left.size());
  for (std::size_t i = 0; i < mono.size(); ++i) {
    mono[i] = 0.5 * (out.left[i] + out.right[i]);
  }
  std::vector<std::complex<double>> window(32'768);
  for (std::size_t i = 0; i < window.size(); ++i) {
    window[i] = mono[i];
  }
  fft(window);
  double weighted = 0.0;
  double total = 0.0;
  for (std::size_t bin = 1; bin < window.size() / 2; ++bin) {
    const double magnitude = std::abs(window[bin]);
    weighted += magnitude * static_cast<double>(bin) * 48'000.0 / 32'768.0;
    total += magnitude;
  }
  return {db(rms(mono, 0, 48'000)), db(rms(mono, 60'000, 72'000)), weighted / total};
}

void kits_match_golden_fingerprints() {
  struct Golden {
    std::string_view name;
    Fingerprint expected;
  };
  static constexpr std::array<Golden, 12> golden{{
      {"808 Classic", {-14.510, -44.868, 2993.5}},
      {"808 Boom", {-12.051, -41.767, 2382.7}},
      {"909 House", {-13.925, -43.536, 3767.0}},
      {"909 Techno", {-11.907, -38.586, 3155.2}},
      {"Electro", {-12.963, -42.949, 3520.2}},
      {"Lo-Fi", {-13.694, -40.009, 2722.9}},
      {"Trap", {-10.968, -32.428, 2880.4}},
      {"Industrial", {-13.207, -36.183, 3581.2}},
      {"Minimal", {-16.174, -45.595, 2724.6}},
      {"Dub", {-13.983, -44.449, 2904.4}},
      {"Breaks", {-11.897, -39.516, 3714.0}},
      {"Init", {-19.665, -48.673, 2885.1}},
  }};
  const bool rebaseline = std::getenv("PULSE_REBASELINE") != nullptr;
  for (const auto& entry : golden) {
    const auto actual = pattern_fingerprint(entry.name);
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
            std::string(entry.name) + ": tail changed");
    REQUIRE_LT(std::fabs(actual.centroid_hz / entry.expected.centroid_hz - 1.0), 0.02,
               std::string(entry.name) + ": tone changed");
  }
}

} // namespace

int main() {
  try {
    descriptor_registry_and_kits();
    pads_map_from_c1_and_other_keys_are_silent();
    kick_pitch_and_decay_are_exact();
    kick_retunes_while_it_rings();
    hats_have_no_low_end_and_the_closed_hat_chokes();
    retriggers_never_click();
    block_splits_and_tails();
    kits_render_sane_audio();
    every_pad_at_once_does_not_allocate();
    kits_match_golden_fingerprints();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "pulse engine contract passed\n";
  return 0;
}
