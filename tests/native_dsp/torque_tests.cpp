#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/torque.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/signal.hpp"
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
#include <utility>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using namespace synth_test;

const SynthEngine& torque_engine() { return torque::engine(); }
const EngineDescriptor& table() { return *torque_engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }
std::vector<double> patch(std::string_view name) { return preset(table(), name); }

// Init on a model with the filter open and nothing colouring the pitch.
std::vector<double> clean(double model) {
  auto values = patch("Init");
  values[at("torque.model")] = model;
  values[at("torque.moog.level2")] = 0;
  values[at("torque.moog.level3")] = 0;
  values[at("torque.reese.detune")] = 0;
  values[at("torque.reese.sub")] = 0;
  values[at("torque.reese.comb")] = 0;
  values[at("torque.sub.drop")] = 0;
  return values;
}

// Upward zero crossings, interpolated, from frame `from`.
std::vector<double> crossings(const std::vector<double>& x, std::size_t begin, std::size_t end) {
  std::vector<double> out;
  for (std::size_t i = std::max<std::size_t>(begin, 1); i < end; ++i) {
    if (x[i - 1] < 0.0 && x[i] >= 0.0) {
      out.push_back(static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]));
    }
  }
  return out;
}

using oscillate::test::rms;

void descriptor_registry_and_presets() {
  const auto valid = validate_engine(table());
  REQUIRE_OK(valid, valid.has_value() ? "" : valid.error().message);
  REQUIRE_EQ(table().id, "torque", "engine identity is torque v1");
  REQUIRE_EQ(table().version, 1, "engine identity is torque v1");
  REQUIRE_EQ(table().presets.size(), 16, "sixteen factory presets");
  REQUIRE_EQ(find_engine("torque"), &torque_engine(), "Torque registers as the sixth engine");
  REQUIRE_GE(engines().size(), 6, "Torque registers as the sixth engine");
  REQUIRE_EQ(engines()[5], &torque_engine(), "Torque registers as the sixth engine");
  for (const auto& parameter : table().parameters) {
    require(parameter.id.starts_with("torque."), "ids are namespaced");
  }
}

void pitch_is_exact_in_every_model() {
  for (double model : {0.0, 1.0, 2.0, 3.0}) {
    for (double rate : {44'100.0, 48'000.0, 96'000.0}) {
      for (std::uint8_t key : {std::uint8_t{36}, std::uint8_t{57}}) {
        const double expected = 440.0 * std::exp2((key - 69.0) / 12.0);
        auto values = clean(model);
        if (model == 2.0) {
          // Phase-spread saws cross zero several times a cycle: filter down to
          // the fundamental so the crossing oracle sees one per period.
          values[at("torque.cutoff")] = 1.3 * expected;
        }
        auto runtime = make_runtime(torque_engine(), values, rate);
        const auto frames = static_cast<std::size_t>(rate);
        const auto out = render(*runtime, note(0, frames, key, 1), frames, 128, rate);
        REQUIRE_LT(std::fabs(cents(measured_hz(out.left, frames / 4, frames, rate), expected)), 0.5,
                   "pitch within 0.5 cent in model " + std::to_string(model));
      }
    }
  }
}

// Overlapping notes slide: pitch(t) = target - interval * exp(-t / tau),
// tau = slide / 3. Measured period by period at a high register, where a
// period is short against the slide.
void slide_follows_the_exponential_law() {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  auto values = clean(0);
  values[at("torque.slide")] = 0.6;
  auto runtime = make_runtime(torque_engine(), values, rate);
  auto events = note(0, 48'000, 72, 1);
  auto upper = note(12'000, 30'000, 84, 2);
  events.insert(events.end(), upper.begin(), upper.end());
  const auto out = render(*runtime, events, 48'000, 64, rate);
  const auto times = crossings(out.left, 12'000, 40'000);
  const double tau = 0.6 / 3.0;
  // BLEP latency plus the oversampler's delay, in frames.
  constexpr double latency = 11.0 + 4.0;
  int checked = 0;
  for (std::size_t k = 1; k < times.size(); ++k) {
    const double middle = 0.5 * (times[k] + times[k - 1]);
    const double t = (middle - 12'000.0 - latency) / rate;
    if (t < 0.02) {
      continue; // The first tick has not moved the pitch yet.
    }
    const double measured = 69.0 + 12.0 * std::log2(rate / (times[k] - times[k - 1]) / 440.0);
    const double expected = 84.0 - 12.0 * std::exp(-t / tau);
    REQUIRE_LT(std::fabs(measured - expected), 0.05, "slide within 5 cents of the law");
    ++checked;
  }
  REQUIRE_GT(checked, 100, "the slide was measured over its course");
}

// The 303 accent sweep: consecutive accented notes get brighter; the same
// line unaccented does not.
void accents_climb_through_the_sweep() {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  const auto brightness = [&](std::uint8_t velocity) {
    auto values = patch("Acid Classic");
    values[at("torque.fx.dist_mix")] = 0;
    auto runtime = make_runtime(torque_engine(), values, rate);
    std::vector<Event> events;
    for (std::uint64_t n = 0; n < 4; ++n) {
      auto e = note(n * 6'000, 3'000, 45, n + 1, velocity);
      events.insert(events.end(), e.begin(), e.end());
    }
    const auto out = render(*runtime, events, 24'000, 128, rate);
    std::vector<double> ratio;
    for (std::size_t n = 0; n < 4; ++n) {
      double edge = 0.0;
      double power = 0.0;
      for (std::size_t i = n * 6'000 + 200; i < n * 6'000 + 1'400; ++i) {
        edge += (out.left[i] - out.left[i - 1]) * (out.left[i] - out.left[i - 1]);
        power += out.left[i] * out.left[i];
      }
      ratio.push_back(edge / power);
    }
    return ratio;
  };
  const auto accented = brightness(127);
  const auto plain = brightness(64);
  for (std::size_t n = 1; n < 4; ++n) {
    REQUIRE_GT(accented[n], accented[n - 1] * 1.02, "accented notes climb");
    REQUIRE_LT(std::fabs(plain[n] / plain[1] - 1.0), 0.02, "unaccented notes stay put");
  }
}

// The 808 drop falls in Hz like ResonantBody: f(t) = f0 + df * exp(-t / tau).
// Integrated, phase(t) = f0 t + df tau (1 - exp(-t / tau)); each upward
// crossing lands where the phase reaches a whole cycle.
void sub_drop_follows_the_analytic_sweep() {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  auto values = clean(3);
  values[at("torque.sub.drop")] = 12;
  values[at("torque.sub.drop_time")] = 0.2;
  auto runtime = make_runtime(torque_engine(), values, rate);
  const auto out = render(*runtime, note(0, 24'000, 48, 1), 24'000, 128, rate);
  const auto times = crossings(out.left, 2, 24'000);
  const double f0 = 440.0 * std::exp2((48 - 69.0) / 12.0);
  const double df = f0;
  const double tau = 0.2;
  const auto phase = [&](double frames) {
    const double t = frames / rate;
    return f0 * t + df * tau * (1.0 - std::exp(-t / tau));
  };
  const double origin = phase(times[0]);
  for (std::size_t k = 1; k < 40 && k < times.size(); ++k) {
    REQUIRE_LT(std::fabs(phase(times[k]) - origin - static_cast<double>(k)), 0.01,
               "808 drop cycles land on the analytic phase");
  }
}

void sequencer_runs_while_a_note_is_held() {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  auto values = clean(0);
  values[at("torque.seq.on")] = 1;
  values[at("torque.fx.delay")] = 0;
  // 120 BPM sixteenths: 6000 frames a step. Step 6 is a rest; step 5's gate
  // closes at 27000 and the release ends well before the rest.
  auto runtime = make_runtime(torque_engine(), values, rate);
  const auto out = render(*runtime, note(0, 60'000, 48, 1), 72'000, 128, rate);
  REQUIRE_GT(rms(out.left, 24'500, 26'500), 0.05, "step 5 sounds");
  REQUIRE_LT(rms(out.left, 31'000, 35'500), 1e-4, "step 6 is a rest");
  REQUIRE_LT(rms(out.left, 64'000, 72'000), 1e-4, "releasing the note stops the pattern");
  // Step 1 is the root: holding G2 instead of C2 plays it seven semitones up.
  for (std::uint8_t key : {std::uint8_t{48}, std::uint8_t{55}}) {
    auto held = make_runtime(torque_engine(), values, rate);
    const auto line = render(*held, note(0, 6'000, key, 1), 6'000, 128, rate);
    const double expected = 440.0 * std::exp2((key - 69.0) / 12.0);
    REQUIRE_LT(std::fabs(cents(measured_hz(line.left, 600, 2'800, rate), expected)), 3.0,
               "the pattern is rooted on the held note");
  }
}

// Switching the sequencer while a note is held takes effect at once: on,
// the pattern starts (its rests are heard); off, the held note plays on its
// own and its release still ends it.
void sequencer_toggles_while_a_note_is_held() {
  constexpr std::size_t block = 128;
  constexpr std::uint64_t held_frames = 96'000;
  constexpr std::uint64_t toggle_at = 12'032;
  constexpr double silent_rms = 1e-4;
  constexpr double sounding_rms = 0.01;
  auto plain = clean(0);
  plain[at("torque.fx.delay")] = 0;
  auto sequenced = plain;
  sequenced[at("torque.seq.on")] = 1;
  const auto run = [&](const std::vector<double>& before, const std::vector<double>& after,
                       std::uint64_t total) {
    auto runtime = make_runtime(torque_engine(), before);
    (void)render(*runtime, note(0, held_frames, 48, 1), toggle_at);
    require(runtime->offer_values(after, 1), "sequencer switch offered");
    std::vector<double> out;
    std::vector<engine::StereoFrame> buffer(block);
    for (std::uint64_t at_frame = toggle_at; at_frame < total; at_frame += block) {
      buffer.assign(block, {0.0F, 0.0F});
      (void)runtime->process(buffer, {}, at_frame, 1, {}, {});
      for (const auto& frame : buffer)
        out.push_back(frame.left);
    }
    return out;
  };
  // Off to on: the pattern starts at the switch, so its sixth step (a rest,
  // 31'000..35'500 frames in) is silent, which a plainly held note never is.
  constexpr std::size_t rest_from = 31'000;
  constexpr std::size_t rest_to = 35'500;
  constexpr std::uint64_t pattern_frames = 36'000;
  const auto on = run(plain, sequenced, toggle_at + pattern_frames);
  REQUIRE_LT(rms(on, rest_from, rest_to), silent_rms,
             "switching the sequencer on mid-note did not start the pattern");
  // On to off: the held note keeps sounding through that rest.
  const auto off = run(sequenced, plain, toggle_at + pattern_frames);
  REQUIRE_GT(rms(off, rest_from, rest_to), sounding_rms,
             "switching the sequencer off mid-note silenced the held note");
}

void any_block_split_renders_identically() {
  for (std::string_view name :
       {"Acid Classic", "Seq Acid Line", "Moog Bass", "Neuro Growl", "808 Glide"}) {
    auto a = make_runtime(torque_engine(), patch(name));
    auto b = make_runtime(torque_engine(), patch(name));
    auto events = note(0, 20'000, 36, 1);
    auto more = note(7'000, 20'000, 43, 2, 120);
    auto later = note(30'000, 6'000, 41, 3);
    events.insert(events.end(), more.begin(), more.end());
    events.insert(events.end(), later.begin(), later.end());
    const auto x = render(*a, events, 48'000, 64);
    const auto y = render(*b, events, 48'000, 0);
    require(x.left == y.left && x.right == y.right,
            std::string(name) + ": block splits render identically");
  }
}

// The voice is mono, and a model or Reese voice-count change reaches the
// sounding note (ADR-0038): no deferral, the note changes, and the switch
// allocates nothing (the fading state reuses its prepared storage).
void model_changes_apply_live_and_the_voice_is_mono() {
  auto values = patch("Acid Classic");
  auto runtime = make_runtime(torque_engine(), values);
  auto events = note(0, 48'000, 36, 1);
  auto more = note(100, 48'000, 40, 2);
  events.insert(events.end(), more.begin(), more.end());
  const auto out = render(*runtime, events, 256);
  REQUIRE_EQ(out.last.active_voices, 1, "Torque is mono");

  for (const auto& [id, value] : {std::pair{"torque.model", 2.0}, std::pair{"torque.model", 3.0}}) {
    auto changed = values;
    changed[at(id)] = value;
    auto kept = make_runtime(torque_engine(), values);
    auto switched = make_runtime(torque_engine(), values);
    (void)render(*kept, note(0, 96'000, 36, 1), 12'000);
    (void)render(*switched, note(0, 96'000, 36, 1), 12'000);
    require(switched->offer_values(changed, 1), "model change offered");
    std::vector<engine::StereoFrame> a(128);
    std::vector<engine::StereoFrame> b(128);
    bool deferred = false;
    bool differs = false;
    counting_allocations = true;
    counted_allocations = 0;
    for (std::uint64_t block = 0; block < 40; ++block) {
      a.assign(128, {0.0F, 0.0F});
      b.assign(128, {0.0F, 0.0F});
      (void)kept->process(a, {}, 12'000 + block * 128, 1, {}, {});
      const auto report = switched->process(b, {}, 12'000 + block * 128, 1, {}, {});
      deferred = deferred || report.kernel.deferred_changes;
      if (block > 10) {
        for (std::size_t i = 0; i < a.size(); ++i) {
          differs = differs || a[i].left != b[i].left;
        }
      }
    }
    counting_allocations = false;
    REQUIRE_EQ(counted_allocations, 0, "a live model switch never allocates");
    require(!deferred, "a model change is never deferred");
    require(differs, "the sounding note now plays the new model");
  }

  auto reese = patch("Neuro Growl");
  auto wider = reese;
  wider[at("torque.reese.voices")] = 3;
  auto switched = make_runtime(torque_engine(), reese);
  (void)render(*switched, note(0, 96'000, 36, 1), 12'000);
  require(switched->offer_values(wider, 1), "voice count change offered");
  std::vector<engine::StereoFrame> buffer(128);
  const auto report = switched->process(buffer, {}, 12'000, 1, {}, {});
  require(!report.kernel.deferred_changes, "a Reese voice-count change is never deferred");
}

void tail_bound_covers_release_and_delay() {
  for (std::string_view name : {"Acid Squelch", "Taurus Pedal", "Dub Sub"}) {
    const auto values = patch(name);
    auto runtime = make_runtime(torque_engine(), values);
    const auto tail = torque_engine().tail_frames(values, 48'000.0);
    const auto out = render(*runtime, note(0, 4'800, 36, 1), 4'800 + tail + 9'600);
    require(peak(out.left, 4'800 + tail) < 1e-3 && peak(out.right, 4'800 + tail) < 1e-3,
            std::string(name) + ": output settles inside the tail bound");
  }
}

void presets_render_sane_audio() {
  for (const auto& entry : table().presets) {
    auto runtime = make_runtime(torque_engine(), patch(entry.name));
    std::vector<Event> line;
    std::uint64_t id = 1;
    for (std::uint8_t key :
         {std::uint8_t{36}, std::uint8_t{43}, std::uint8_t{39}, std::uint8_t{36}}) {
      auto n = note((id - 1) * 12'000, 10'000, key, id, id % 2 == 0 ? 120 : 90);
      line.insert(line.end(), n.begin(), n.end());
      ++id;
    }
    const auto out = render(*runtime, line, 48'000);
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
              << 20.0 * std::log10(std::max(peak(out.left), peak(out.right))) << " dBFS\n";
    REQUIRE_LE(std::max(peak(out.left), peak(out.right)), 1.0, "preset never exceeds 0 dBFS");
    REQUIRE_GT(rms_db, -40.0, "preset is audible and not slammed");
    REQUIRE_LT(rms_db, -3.0, "preset is audible and not slammed");
    REQUIRE_LT(std::fabs(sum / (2.0 * 48'000.0)), 0.01, "preset has no DC offset");
  }
}

void full_load_does_not_allocate() {
  auto values = patch("Neuro Growl");
  values[at("torque.seq.on")] = 1;
  values[at("torque.fx.delay")] = 0.5;
  values[at("torque.fx.chorus")] = 3;
  auto runtime = make_runtime(torque_engine(), values);
  std::vector<engine::StereoFrame> buffer(128);
  std::vector<engine::BlockMidiEvent> on(1);
  on[0].clip_id_bits = {1, 1};
  on[0].note_id_bits = {0, 1};
  on[0].kind = engine::MidiEventKind::note_on;
  on[0].key = 36;
  on[0].velocity = 120;
  std::vector<double> lane(128, 900.0);
  const std::vector<ParameterLane> lanes{{static_cast<std::uint32_t>(at("torque.cutoff")), lane}};
  counting_allocations = true;
  counted_allocations = 0;
  for (std::uint64_t block = 0; block < 128; ++block) {
    (void)runtime->process(buffer, block == 0 ? std::span(on) : std::span(on).subspan(0, 0),
                           block * 128, 1, {}, lanes);
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "the Torque callback never allocates");
}

void presets_match_golden_fingerprints() {
  struct Golden {
    std::string_view name;
    Fingerprint expected;
  };
  static constexpr std::array<Golden, 16> golden{{
      {"Acid Classic", {-21.581, -117.501, 368.1}},
      {"Acid Squelch", {-24.593, -44.074, 421.5}},
      {"Rubber Acid", {-20.546, -159.356, 495.8}},
      {"Seq Acid Line", {-23.641, -45.675, 1142.6}},
      {"Moog Bass", {-10.629, -117.399, 696.2}},
      {"Funk Pluck", {-32.058, -147.553, 374.2}},
      {"Taurus Pedal", {-13.976, -34.036, 853.2}},
      {"Synthwave Bass", {-20.203, -44.663, 598.0}},
      {"Reese", {-10.046, -86.479, 1802.6}},
      {"Neuro Growl", {-11.690, -105.454, 5079.3}},
      {"Wobble Reese", {-10.237, -88.966, 424.9}},
      {"Deep Sub", {-7.431, -154.729, 343.9}},
      {"808 Glide", {-11.875, -60.192, 548.3}},
      {"Distorted 808", {-10.842, -118.782, 2982.5}},
      {"Dub Sub", {-6.194, -20.834, 410.4}},
      {"Init", {-20.533, -152.800, 1470.4}},
  }};
  const bool rebaseline = std::getenv("TORQUE_REBASELINE") != nullptr;
  for (const auto& entry : golden) {
    const auto actual = fingerprint(torque_engine(), entry.name);
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
    pitch_is_exact_in_every_model();
    slide_follows_the_exponential_law();
    accents_climb_through_the_sweep();
    sub_drop_follows_the_analytic_sweep();
    sequencer_runs_while_a_note_is_held();
    sequencer_toggles_while_a_note_is_held();
    any_block_split_renders_identically();
    model_changes_apply_live_and_the_voice_is_mono();
    tail_bound_covers_release_and_delay();
    presets_render_sane_audio();
    full_load_does_not_allocate();
    presets_match_golden_fingerprints();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "torque engine contract passed\n";
  return 0;
}
