#include "oscillate/native_dsp/synth/drift_engine.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"
#include "synth_test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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

const SynthEngine& drift_engine() { return drift::engine(); }
const EngineDescriptor& table() { return *drift_engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }
std::vector<double> patch(std::string_view name) { return preset(table(), name); }

// Init with only the partial bank, steeply tilted so the fundamental leads:
// overlap-2 Hann grains at constant lag reproduce the source exactly.
std::vector<double> clean() {
  auto values = patch("Init");
  values[at("drift.wt.level")] = 0;
  values[at("drift.partials.level")] = 1;
  values[at("drift.partials.tilt")] = 3;
  values[at("drift.partials.stretch")] = 0;
  return values;
}

void descriptor_registry_and_presets() {
  const auto valid = validate_engine(table());
  REQUIRE_OK(valid, valid.has_value() ? "" : valid.error().message);
  REQUIRE_EQ(table().id, "drift", "engine identity is drift v1");
  REQUIRE_EQ(table().version, 1, "engine identity is drift v1");
  REQUIRE_EQ(table().presets.size(), 17, "seventeen factory presets");
  REQUIRE_EQ(find_engine("drift"), &drift_engine(), "Drift registers as the seventh engine");
  REQUIRE_GE(engines().size(), 7, "Drift registers as the seventh engine");
  REQUIRE_EQ(engines()[6], &drift_engine(), "Drift registers as the seventh engine");
  for (const auto& parameter : table().parameters) {
    require(parameter.id.starts_with("drift."), "ids are namespaced");
    require(parameter.automatable, "every parameter is automatable (ADR-0034)");
  }
}

void clean_path_pitch_is_exact() {
  for (double rate : {44'100.0, 48'000.0, 96'000.0}) {
    for (std::uint8_t key : {std::uint8_t{45}, std::uint8_t{69}}) {
      auto runtime = make_runtime(drift_engine(), clean(), rate);
      const auto frames = static_cast<std::size_t>(rate);
      const auto out = render(*runtime, note(0, frames, key, 1), frames, 128, rate);
      const double expected = 440.0 * std::exp2((key - 69.0) / 12.0);
      REQUIRE_LT(std::fabs(cents(measured_hz(out.left, frames / 2, frames, rate), expected)), 0.5,
                 "the clean grain path keeps pitch within 0.5 cent");
    }
  }
}

// Frozen, scan 0, no spray or chaos: every grain reads the same moment, so
// the output repeats exactly with the grain period (rate / density).
void a_frozen_note_repeats_with_the_grain_period() {
  auto values = clean();
  values[at("drift.freeze")] = 1;
  values[at("drift.grain.scan")] = 0;
  auto runtime = make_runtime(drift_engine(), values);
  const auto out = render(*runtime, note(0, 150'000, 57, 1), 150'000);
  const std::size_t period = 2'400; // 48 kHz / 20 grains per second.
  double largest = 0.0;
  double level = 0.0;
  for (std::size_t i = 100'000; i < 140'000; ++i) {
    largest = std::max(largest, std::fabs(out.left[i + period] - out.left[i]));
    level = std::max(level, std::fabs(out.left[i]));
  }
  REQUIRE_GT(level, 0.01, "the frozen note sounds");
  REQUIRE_LT(largest, 1e-5 * level, "a frozen moment repeats with the grain period");
}

void any_block_split_renders_identically() {
  for (std::string_view name :
       {"Nebula", "Stutter Clock", "Ice Cave", "Spectral Smear", "Glass Rain"}) {
    auto a = make_runtime(drift_engine(), patch(name));
    auto b = make_runtime(drift_engine(), patch(name));
    auto events = note(0, 60'000, 48, 1);
    auto more = note(7'000, 60'000, 55, 2);
    events.insert(events.end(), more.begin(), more.end());
    const auto x = render(*a, events, 72'000, 64);
    const auto y = render(*b, events, 72'000, 0);
    require(x.left == y.left && x.right == y.right,
            std::string(name) + ": block splits render identically");
  }
}

// A filter-type change reaches the sounding note (ADR-0038): no deferral,
// and after the crossfade the note sounds through the new filter.
void filter_changes_apply_live_and_polyphony_steals() {
  const auto values = patch("Nebula");
  auto changed = values;
  changed[at("drift.filter.type")] = 3; // HP 12.
  auto kept = make_runtime(drift_engine(), values);
  auto switched = make_runtime(drift_engine(), values);
  std::vector<engine::StereoFrame> a(128);
  std::vector<engine::StereoFrame> b(128);
  (void)render(*kept, note(0, 96'000, 60, 1), 24'000);
  (void)render(*switched, note(0, 96'000, 60, 1), 24'000);
  require(switched->offer_values(changed, 1), "filter change offered");
  bool deferred = false;
  bool differs = false;
  for (std::uint64_t block = 0; block < 40; ++block) {
    a.assign(128, {0.0F, 0.0F});
    b.assign(128, {0.0F, 0.0F});
    (void)kept->process(a, {}, 24'000 + block * 128, 1, {}, {});
    const auto report = switched->process(b, {}, 24'000 + block * 128, 1, {}, {});
    deferred = deferred || report.kernel.deferred_changes;
    if (block > 10) {
      for (std::size_t i = 0; i < a.size(); ++i) {
        differs = differs || a[i].left != b[i].left;
      }
    }
  }
  require(!deferred, "a filter change is never deferred");
  require(differs, "the sounding note now plays through the new filter");

  auto poly = make_runtime(drift_engine(), patch("Init"));
  std::vector<Event> events;
  for (std::uint64_t i = 0; i < 13; ++i) {
    auto n = note(i * 240, 40'000, static_cast<std::uint8_t>(40 + i), i + 1);
    events.insert(events.end(), n.begin(), n.end());
  }
  const auto out = render(*poly, events, 6'000);
  REQUIRE_EQ(out.last.active_voices, drift::voice_limit,
             "twelve voices; the thirteenth note steals");
  REQUIRE_EQ(out.last.voice_steals, 1, "twelve voices; the thirteenth note steals");
}

void tail_bound_covers_release_and_reverb() {
  for (std::string_view name : {"Init", "Stutter Clock", "Glass Rain"}) {
    const auto values = patch(name);
    auto runtime = make_runtime(drift_engine(), values);
    const auto tail = drift_engine().tail_frames(values, 48'000.0);
    const auto out = render(*runtime, note(0, 24'000, 60, 1), 24'000 + tail + 9'600);
    require(peak(out.left, 24'000 + tail) < 1e-3 && peak(out.right, 24'000 + tail) < 1e-3,
            std::string(name) + ": output settles inside the tail bound");
  }
}

void presets_render_sane_audio() {
  for (const auto& entry : table().presets) {
    auto runtime = make_runtime(drift_engine(), patch(entry.name));
    std::vector<Event> chord;
    std::uint64_t id = 1;
    for (std::uint8_t key :
         {std::uint8_t{48}, std::uint8_t{55}, std::uint8_t{60}, std::uint8_t{64}}) {
      auto n = note(0, 72'000, key, id++);
      chord.insert(chord.end(), n.begin(), n.end());
    }
    const auto out = render(*runtime, chord, 96'000);
    double power = 0.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < out.left.size(); ++i) {
      REQUIRE_TRUE(std::isfinite(out.left[i]), "preset is finite");
      REQUIRE_TRUE(std::isfinite(out.right[i]), "preset is finite");
      power += out.left[i] * out.left[i] + out.right[i] * out.right[i];
      sum += out.left[i] + out.right[i];
    }
    const double frames = static_cast<double>(out.left.size());
    const double rms_db = 10.0 * std::log10(power / (2.0 * frames));
    std::cout << "  " << entry.name << ": rms " << rms_db << " dBFS, peak "
              << 20.0 * std::log10(std::max(peak(out.left), peak(out.right))) << " dBFS\n";
    REQUIRE_LE(std::max(peak(out.left), peak(out.right)), 1.0, "preset never exceeds 0 dBFS");
    REQUIRE_GT(rms_db, -40.0, "preset is audible and not slammed");
    REQUIRE_LT(rms_db, -3.0, "preset is audible and not slammed");
    REQUIRE_LT(std::fabs(sum / (2.0 * frames)), 0.01, "preset has no DC offset");
  }
}

void full_load_does_not_allocate() {
  auto values = patch("Spectral Smear");
  values[at("drift.grain.density")] = 200;
  values[at("drift.fx.chorus")] = 3;
  values[at("drift.fx.delay")] = 0.5;
  auto runtime = make_runtime(drift_engine(), values);
  std::vector<engine::StereoFrame> buffer(128);
  std::vector<engine::BlockMidiEvent> on(12);
  for (std::size_t i = 0; i < on.size(); ++i) {
    on[i].clip_id_bits = {1, 1};
    on[i].note_id_bits = {0, i + 1};
    on[i].kind = engine::MidiEventKind::note_on;
    on[i].key = static_cast<std::uint8_t>(40 + 3 * i);
    on[i].velocity = 100;
  }
  std::vector<double> lane(128, 0.5);
  std::vector<double> frozen(128, 1.0);
  const std::vector<ParameterLane> lanes{{static_cast<std::uint32_t>(at("drift.blur")), lane},
                                         {static_cast<std::uint32_t>(at("drift.freeze")), frozen}};
  counting_allocations = true;
  counted_allocations = 0;
  for (std::uint64_t block = 0; block < 600; ++block) {
    (void)runtime->process(buffer, block == 0 ? std::span(on) : std::span(on).subspan(0, 0),
                           block * 128, 1, {}, lanes);
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "the Drift callback never allocates");
}

void presets_match_golden_fingerprints() {
  struct Golden {
    std::string_view name;
    Fingerprint expected;
  };
  static constexpr std::array<Golden, 16> golden{{
      {"Nebula", {-19.769, -21.793, 2677.3}},
      {"Glass Rain", {-18.653, -23.300, 1800.7}},
      {"Frozen Choir", {-24.775, -25.500, 2919.3}},
      {"Shimmer Halo", {-24.744, -24.753, 2729.6}},
      {"Dust Motes", {-27.105, -31.568, 3773.3}},
      {"Tidal Pad", {-23.604, -23.582, 2727.9}},
      {"Stutter Clock", {-24.281, -40.310, 1950.9}},
      {"Reverse Bloom", {-24.321, -26.181, 2208.7}},
      {"Granite Drone", {-22.456, -22.720, 1508.0}},
      {"Aurora", {-28.625, -28.386, 4393.0}},
      {"Spectral Smear", {-22.210, -23.635, 2437.2}},
      {"Ice Cave", {-16.149, -17.227, 2404.4}},
      {"Pulse Grains", {-21.998, -25.497, 2463.3}},
      {"Deep Field", {-20.323, -21.835, 270.4}},
      {"Wind Organ", {-17.278, -19.392, 1466.6}},
      {"Init", {-18.734, -61.399, 2055.9}},
  }};
  const bool rebaseline = std::getenv("DRIFT_REBASELINE") != nullptr;
  for (const auto& entry : golden) {
    const auto actual = fingerprint(drift_engine(), entry.name);
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

int main() {
  try {
    descriptor_registry_and_presets();
    clean_path_pitch_is_exact();
    a_frozen_note_repeats_with_the_grain_period();
    any_block_split_renders_identically();
    filter_changes_apply_live_and_polyphony_steals();
    tail_bound_covers_release_and_reverb();
    presets_render_sane_audio();
    full_load_does_not_allocate();
    presets_match_golden_fingerprints();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "drift engine contract passed\n";
  return 0;
}
