#include "effect_test_support.hpp"

#include "oscillate/native_dsp/effects/orbit.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

// Orbit (ADR-0040) against analytic oracles: the vibrato's demodulated
// delay sweep, static flanger combs with and without feedback, static phaser
// notches from the allpass phase law, tremolo depth and stereo phase, the
// dimension's anti-phase sweep, tempo-synced periods, click-free live mode
// switches and goldens.
namespace {
using namespace effect_test;
namespace orbit = oscillate::native_dsp::effects::orbit;

const effects::EffectEngine& orbit_engine() { return orbit::engine(); }
const EngineDescriptor& table() { return *orbit_engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }

int mode_named(std::string_view name) {
  const auto& choices = table().parameters[at("orbit.mode")].choices;
  for (std::size_t i = 0; i < choices.size(); ++i) {
    if (choices[i] == name) {
      return static_cast<int>(i);
    }
  }
  throw std::runtime_error("unknown mode " + std::string(name));
}
namespace modes {
// NOLINTBEGIN(bugprone-throwing-static-initialization): a throw fails the test at start-up.
const int chorus = mode_named("Chorus");
const int ensemble = mode_named("Ensemble");
const int dimension = mode_named("Dimension");
const int flanger = mode_named("Flanger");
const int phaser = mode_named("Phaser");
const int vibrato = mode_named("Vibrato");
const int tremolo = mode_named("Tremolo");
// NOLINTEND(bugprone-throwing-static-initialization)
} // namespace modes

std::vector<double> mode(int m) {
  auto values = preset(table(), "Init");
  values[at("orbit.mode")] = m;
  return values;
}

double level_db(const std::vector<double>& values, double hz) {
  auto runtime = make_effect(orbit_engine(), values);
  const auto input = sine(hz, 0.25, 48'000);
  const auto out = process(*runtime, input);
  return db(rms(out.left, 24'000, 48'000) / rms(input.left, 24'000, 48'000));
}

// The delay of a wet-only sine, demodulated: y = sin(w (t - D(t))), so the
// one-period means of y sin(wt) and y cos(wt) give cos(wD) and -sin(wD).
std::vector<double> demodulated_delay_ms(const std::vector<double>& y, double hz) {
  const auto period = static_cast<std::size_t>(std::lround(48'000.0 / hz));
  const double w = 2.0 * std::numbers::pi * hz / 48'000.0;
  std::vector<double> out;
  double unwrapped = 0.0;
  double previous = 0.0;
  for (std::size_t start = 4'800; start + period < y.size(); start += period) {
    double i = 0.0;
    double q = 0.0;
    for (std::size_t n = start; n < start + period; ++n) {
      i += y[n] * std::cos(w * static_cast<double>(n));
      q += y[n] * std::sin(w * static_cast<double>(n));
    }
    const double phase = std::atan2(-i, q);
    double step = phase - previous;
    step -= 2.0 * std::numbers::pi * std::round(step / (2.0 * std::numbers::pi));
    unwrapped += step;
    previous = phase;
    out.push_back(unwrapped / w / 48.0);
  }
  return out;
}

void the_vibrato_sweeps_its_delay() {
  auto values = mode(modes::vibrato);
  values[at("orbit.time")] = 5.0;
  values[at("orbit.depth")] = 0.5;
  values[at("orbit.rate")] = 1.0;
  values[at("orbit.stereo")] = 0.0;
  values[at("orbit.mix")] = 1.0;
  auto runtime = make_effect(orbit_engine(), values);
  const auto out = process(*runtime, sine(1'000.0, 0.5, std::size_t{4} * 48'000));
  const auto delay = demodulated_delay_ms(out.left, 1'000.0);
  const auto [low, high] = std::ranges::minmax_element(delay);
  // D(t) = T (1 + 0.3 depth sin): 5 ms +- 0.75 ms.
  const double swing = *high - *low;
  REQUIRE_LT(std::fabs(swing - 1.5), 0.03, "vibrato swings 1.5 ms: " + std::to_string(swing));
  // One cycle a second: the maximum comes back after 1000 one-ms windows.
  const auto first = std::max_element(delay.begin(), delay.begin() + 1'000) - delay.begin();
  const auto second =
      std::max_element(delay.begin() + first + 500, delay.begin() + first + 1'500) - delay.begin();
  require(second - first >= 998 && second - first <= 1'002,
          "the sweep repeats at the rate: " + std::to_string(second - first));
}

std::vector<double> static_flanger(double feedback) {
  auto values = mode(modes::flanger);
  values[at("orbit.depth")] = 0.0;
  values[at("orbit.time")] = 1.0; // 48 frames: notches at 500 Hz, peaks at 1 kHz.
  values[at("orbit.feedback")] = feedback;
  values[at("orbit.mix")] = 0.5;
  return values;
}

void the_flanger_is_a_comb() {
  REQUIRE_LT(level_db(static_flanger(0.0), 500.0), -60.0, "a static flanger nulls at 1 / 2T");
  REQUIRE_LT(std::fabs(level_db(static_flanger(0.0), 1'000.0)), 0.01, "and passes at 1 / T");
  for (const double g : {0.7, -0.7}) {
    // (1 + z^-T / (1 - g z^-T)) / 2 at z^-T = +-1.
    const double at_peak = 20.0 * std::log10(std::fabs(1.0 + 1.0 / (1.0 - g)) / 2.0);
    const double at_notch = 20.0 * std::log10(std::fabs(1.0 - 1.0 / (1.0 + g)) / 2.0);
    const double peak = level_db(static_flanger(g), 1'000.0);
    const double notch = level_db(static_flanger(g), 500.0);
    require(std::fabs(peak - at_peak) < 0.05 && std::fabs(notch - at_notch) < 0.05,
            "feedback " + std::to_string(g) + ": " + std::to_string(peak) + " / " +
                std::to_string(notch) + " dB, expected " + std::to_string(at_peak) + " / " +
                std::to_string(at_notch));
  }
}

void the_phaser_notches_follow_the_allpass_law() {
  const double centre = 1'000.0;
  const double k = std::tan(std::numbers::pi * centre / 48'000.0);
  const auto notch_hz = [&](double phase_per_stage) {
    return 48'000.0 / std::numbers::pi * std::atan(k * std::tan(phase_per_stage / 2.0));
  };
  struct Case {
    double stages_index;
    std::vector<double> notches;
  };
  // A first-order allpass turns -2 atan(tan(pi f / fs) / k); with the dry
  // signal at mix 0.5 the sum nulls where the chain reaches -180, -540, ...
  for (const Case& c :
       {Case{0, {notch_hz(std::numbers::pi / 2.0)}},
        Case{1, {notch_hz(std::numbers::pi / 4.0), notch_hz(3.0 * std::numbers::pi / 4.0)}}}) {
    auto values = mode(modes::phaser);
    values[at("orbit.depth")] = 0.0;
    values[at("orbit.centre")] = centre;
    values[at("orbit.stages")] = c.stages_index;
    values[at("orbit.mix")] = 0.5;
    for (const double hz : c.notches) {
      const double level = level_db(values, hz);
      REQUIRE_LT(level, -50.0,
                 "phaser notch at " + std::to_string(hz) + " Hz: " + std::to_string(level));
    }
  }
}

// One-millisecond RMS envelope.
std::vector<double> envelope(const std::vector<double>& x) {
  std::vector<double> out;
  for (std::size_t start = 0; start + 48 <= x.size(); start += 48) {
    out.push_back(rms(x, start, start + 48));
  }
  return out;
}

void the_tremolo_modulates_level_in_stereo() {
  auto values = mode(modes::tremolo);
  values[at("orbit.depth")] = 1.0;
  values[at("orbit.rate")] = 2.0;
  values[at("orbit.stereo")] = 180.0;
  values[at("orbit.mix")] = 1.0;
  auto runtime = make_effect(orbit_engine(), values);
  const auto out = process(*runtime, sine(1'000.0, 0.5, std::size_t{2} * 48'000));
  const auto left = envelope(out.left);
  const auto right = envelope(out.right);
  const double full = 0.5 / std::numbers::sqrt2;
  const auto [low, high] = std::ranges::minmax_element(left);
  REQUIRE_LT(*low, full * 0.01, "full depth reaches silence and unity");
  REQUIRE_GT(*high, full * 0.99, "full depth reaches silence and unity");
  for (std::size_t i = 0; i < left.size(); ++i) {
    // Anti-phase: the gains sum to one, g + (1 - g).
    REQUIRE_LT(std::fabs(left[i] + right[i] - full), full * 0.02,
               "180 degrees pans the channels against each other");
  }
}

void the_dimension_sweeps_in_anti_phase() {
  auto values = preset(table(), "Dimension 4");
  auto runtime = make_effect(orbit_engine(), values);
  const double time = values[at("orbit.time")];
  for (int block = 0; block < 20; ++block) {
    (void)process(*runtime, noise(0.2, 2'400), {}, static_cast<std::uint64_t>(block) * 2'400);
    std::vector<float> t;
    require(runtime->read_telemetry(t), "telemetry");
    REQUIRE_LT(
        std::fabs(t[orbit::telemetry::sweep_left] + t[orbit::telemetry::sweep_right] - 2.0 * time),
        tolerance::coarse, "the two delays mirror each other around Time");
  }
}

void sync_follows_the_tempo() {
  // Auto-Pan: one cycle per half note.
  for (const double bpm : {90.0, 140.0}) {
    auto runtime = make_effect(orbit_engine(), preset(table(), "Auto-Pan"));
    const auto out = process(*runtime, sine(1'000.0, 0.5, std::size_t{6} * 48'000), {.bpm = bpm});
    const auto left = envelope(out.left);
    // Minima of the left gain sit one period apart.
    std::vector<std::size_t> minima;
    for (std::size_t i = 1; i + 1 < left.size(); ++i) {
      if (left[i] < left[i - 1] && left[i] <= left[i + 1] && left[i] < 0.02) {
        minima.push_back(i);
      }
    }
    REQUIRE_GE(minima.size(), 3, "the pan cycles");
    const double expected_ms = 2.0 * 60'000.0 / bpm;
    const double measured = static_cast<double>(minima.back() - minima.front()) /
                            static_cast<double>(minima.size() - 1);
    REQUIRE_LT(std::fabs(measured - expected_ms), 2.0,
               "a half-note cycle at " + std::to_string(bpm) + " BPM: " + std::to_string(measured));
  }
}

void live_mode_switches_are_click_free() {
  const std::array<std::array<int, 2>, 5> switches{{{modes::chorus, modes::flanger},
                                                    {modes::phaser, modes::tremolo},
                                                    {modes::ensemble, modes::dimension},
                                                    {modes::vibrato, modes::chorus},
                                                    {modes::flanger, modes::phaser}}};
  const auto input = sine(220.0, 0.5, 48'000);
  for (const auto& pair : switches) {
    auto values = preset(table(), "Classic Chorus");
    values[at("orbit.mode")] = pair[0];
    values[at("orbit.mix")] = 1.0;
    auto runtime = make_effect(orbit_engine(), values);
    auto next = values;
    next[at("orbit.mode")] = pair[1];
    const auto out =
        process(*runtime, input, {.at = {24'000}, .before = [&](std::uint64_t) {
                                    require(runtime->offer_values(next, 1), "switch offered");
                                  }});
    double steady = 0.0;
    for (std::size_t i = 12'001; i < 24'000; ++i) {
      steady = std::max(steady, std::fabs(out.left[i] - out.left[i - 1]));
    }
    double after = 0.0;
    for (std::size_t i = 26'400; i < 48'000; ++i) {
      after = std::max(after, std::fabs(out.left[i] - out.left[i - 1]));
    }
    double across = 0.0;
    for (std::size_t i = 24'000; i < 26'400; ++i) {
      across = std::max(across, std::fabs(out.left[i] - out.left[i - 1]));
    }
    REQUIRE_LT(across, std::max(steady, after) * 1.2 + 1e-3,
               "switch " + std::to_string(pair[0]) + " -> " + std::to_string(pair[1]) +
                   " is smooth: " + std::to_string(across) + " vs " +
                   std::to_string(std::max(steady, after)));
  }
}

void tone_darkens_only_the_wet_path() {
  auto values = mode(modes::vibrato);
  values[at("orbit.depth")] = 0.0;
  values[at("orbit.mix")] = 1.0;
  values[at("orbit.tone")] = -12.0;
  REQUIRE_LT(std::fabs(level_db(values, 100.0)), 0.1, "the shelf leaves lows alone");
  REQUIRE_LT(level_db(values, 15'000.0), -10.0, "and darkens highs");
  values[at("orbit.mix")] = 0.0;
  REQUIRE_LT(std::fabs(level_db(values, 15'000.0)), tolerance::coarse, "the dry path is untouched");
}

struct Golden {
  std::string_view name;
  double level_db;
  double centroid_hz;
};

void presets_match_golden_fingerprints() {
  const std::array<Golden, 15> golden{{
      {"Classic Chorus", -18.8122, 10182.1},
      {"Lush Chorus", -21.0371, 10511.9},
      {"String Ensemble", -19.964, 9741.59},
      {"Dimension 2", -16.9338, 10146.7},
      {"Dimension 4", -17.7107, 9816.95},
      {"Jet Flanger", -15.1545, 9744.16},
      {"Negative Flanger", -16.2192, 9791.33},
      {"Synced Flange", -16.8564, 10111.7},
      {"Phase Ninety", -16.5118, 11945.6},
      {"Deep Phaser", -17.5097, 11517.9},
      {"Tape Wow", -15.0162, 9625},
      {"Rotary Vibrato", -15.0494, 9148.85},
      {"Tremolo", -17.8013, 10533.3},
      {"Auto-Pan", -20.648, 10523.5},
      {"Init", -14.9332, 10648.2},
  }};
  REQUIRE_EQ(golden.size(), table().presets.size(), "every preset has a golden");
  auto input = noise(0.1, 48'000);
  for (const double hz : {110.0, 220.0, 329.63, 880.0}) {
    const auto tone = sine(hz, 0.12, 48'000);
    for (std::size_t i = 0; i < input.size(); ++i) {
      input.left[i] += tone.left[i];
      input.right[i] += tone.right[i];
    }
  }
  bool ok = true;
  for (const auto& entry : golden) {
    auto runtime = make_effect(orbit_engine(), preset(table(), entry.name));
    const auto out = process(*runtime, input);
    const double level = db(rms(out.left, 12'000, 48'000));
    const double centroid = centroid_hz(out.left, 24'000);
    if (std::fabs(level - entry.level_db) > 0.05 ||
        std::fabs(centroid - entry.centroid_hz) > entry.centroid_hz * 0.01 + 1.0) {
      std::cerr << "{\"" << entry.name << "\", " << level << ", " << centroid << "},\n";
      ok = false;
    }
  }
  require(ok, "presets match their golden fingerprints");
}

} // namespace

int main() {
  try {
    the_vibrato_sweeps_its_delay();
    the_flanger_is_a_comb();
    the_phaser_notches_follow_the_allpass_law();
    the_tremolo_modulates_level_in_stereo();
    the_dimension_sweeps_in_anti_phase();
    sync_follows_the_tempo();
    live_mode_switches_are_click_free();
    tone_darkens_only_the_wet_path();
    presets_match_golden_fingerprints();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "native orbit contract passed\n";
  return 0;
}
