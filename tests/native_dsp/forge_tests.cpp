#include "effect_test_support.hpp"

#include "oscillate/native_dsp/effects/forge.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Forge (ADR-0040) against measured oracles: audio-band aliasing below
// -90 dBFS, auto gain matching a -12 dBFS sine, colour transparent at low
// level, the harmonic signature of each curve and of bias, the rectifier's
// octave, a phase-aligned mix, click-free live model switches, the panel
// curve and goldens.
namespace {
using namespace effect_test;
namespace forge = oscillate::native_dsp::effects::forge;

const effects::EffectEngine& forge_engine() { return forge::engine(); }
const EngineDescriptor& table() { return *forge_engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }

int model_named(std::string_view name) {
  const auto& choices = table().parameters[at("forge.model")].choices;
  for (std::size_t i = 0; i < choices.size(); ++i) {
    if (choices[i] == name) {
      return static_cast<int>(i);
    }
  }
  throw std::runtime_error("unknown model " + std::string(name));
}
namespace models {
// NOLINTBEGIN(bugprone-throwing-static-initialization): a throw fails the test at start-up.
const int tape = model_named("Tape");
const int tube = model_named("Tube");
const int transistor = model_named("Transistor");
const int diode = model_named("Diode");
const int clip = model_named("Clip");
const int fold = model_named("Fold");
const int sine_fold = model_named("Sine Fold");
const int rectify = model_named("Rectify");
// NOLINTEND(bugprone-throwing-static-initialization)
} // namespace models

std::vector<double> shaper(int model, double drive, double bias = 0.0) {
  auto values = preset(table(), "Init");
  values[at("forge.model")] = model;
  values[at("forge.drive")] = drive;
  values[at("forge.bias")] = bias;
  return values;
}

// Bin-exact sines through a rectangular window: every harmonic and alias
// lands on a bin, so nothing leaks. One second of warm-up lets the DC
// blocker settle.
constexpr std::size_t n = 65'536;
constexpr std::size_t warmup = 48'000;
constexpr double rate = oscillate::test::sample_rate_48k_hz;

std::vector<double> spectrum(const std::vector<double>& values, std::size_t bin, double amp) {
  auto runtime = make_effect(forge_engine(), values);
  const auto out = process(*runtime, sine(rate * static_cast<double>(bin) / n, amp, n + warmup));
  std::vector<std::complex<double>> x(n);
  for (std::size_t i = 0; i < n; ++i) {
    x[i] = out.left[warmup + i];
  }
  fft(x);
  std::vector<double> amplitude(n / 2);
  for (std::size_t k = 0; k < n / 2; ++k) {
    amplitude[k] = 2.0 * std::abs(x[k]) / n;
  }
  return amplitude;
}

// The loudest non-harmonic bin up to 20 kHz, in dBFS.
double worst_alias_db(const std::vector<double>& amplitude, std::size_t bin) {
  const std::size_t top = n * 20'000 / 48'000;
  double worst = 0.0;
  for (std::size_t k = 1; k < top; ++k) {
    if (k % bin != 0) {
      worst = std::max(worst, amplitude[k]);
    }
  }
  return db(worst);
}

constexpr std::size_t bin_1k = 1'367; // 1001.1 Hz
constexpr std::size_t bin_5k = 6'833; // 5004.6 Hz

void aliases_stay_below_minus_90_dbfs() {
  struct Range {
    int model;
    double max_drive;
  };
  // Smooth curves at any drive. A fold driven past 24 dB folds its input
  // dozens of times a cycle: a spectrum no oversampler reaches.
  for (const Range r :
       {Range{models::tape, 48}, Range{models::tube, 48}, Range{models::transistor, 48},
        Range{models::diode, 48}, Range{models::clip, 48}, Range{models::fold, 24},
        Range{models::sine_fold, 24}, Range{models::rectify, 24}}) {
    // The sweep grid is the test input.
    // NOLINTNEXTLINE(bugprone-float-loop-counter,clang-analyzer-security.FloatLoopCounter)
    for (double drive = 0.0; drive <= r.max_drive; drive += 12.0) {
      for (const std::size_t bin : {bin_1k, bin_5k}) {
        const double alias = worst_alias_db(spectrum(shaper(r.model, drive), bin, 0.5), bin);
        REQUIRE_LT(alias, -90.0,
                   "model " + std::to_string(r.model) + " at " + std::to_string(drive) +
                       " dB, bin " + std::to_string(bin) + ": aliases at " + std::to_string(alias) +
                       " dBFS");
      }
    }
  }
}

double level_db(const std::vector<double>& values, double hz, double amp) {
  auto runtime = make_effect(forge_engine(), values);
  const auto input = sine(hz, amp, 48'000);
  const auto out = process(*runtime, input);
  return db(rms(out.left, 24'000, 48'000) / rms(input.left, 24'000, 48'000));
}

void auto_gain_matches_level() {
  for (const int model : {models::tape, models::tube, models::transistor, models::diode,
                          models::clip, models::fold, models::sine_fold, models::rectify}) {
    const bool folds =
        model == models::fold || model == models::sine_fold || model == models::rectify;
    // The sweep grid is the test input.
    // NOLINTNEXTLINE(bugprone-float-loop-counter,clang-analyzer-security.FloatLoopCounter)
    for (double drive = 0.0; drive <= (folds ? 24.0 : 48.0); drive += 6.0) {
      for (const double bias : {0.0, 0.5}) {
        const double level = level_db(shaper(model, drive, bias), 1'000.0, 0.25);
        REQUIRE_LT(std::fabs(level), 0.25,
                   "model " + std::to_string(model) + " at " + std::to_string(drive) +
                       " dB keeps level: " + std::to_string(level));
      }
    }
  }
  auto loud = shaper(models::tape, 24.0);
  loud[at("forge.auto_gain")] = 0.0;
  REQUIRE_GT(level_db(loud, 1'000.0, 0.25), 6.0, "without auto gain drive is louder");
}

void colour_is_transparent_at_low_level() {
  for (const double hz : {100.0, 1'000.0, 10'000.0}) {
    const double flat = level_db(shaper(models::tape, 0.0), hz, 0.01);
    for (const double colour : {-12.0, 12.0}) {
      auto values = shaper(models::tape, 0.0);
      values[at("forge.colour")] = colour;
      const double coloured = level_db(values, hz, 0.01);
      REQUIRE_LT(std::fabs(coloured - flat), 0.05,
                 "emphasis and de-emphasis cancel at " + std::to_string(hz) +
                     " Hz: " + std::to_string(coloured - flat));
    }
  }
}

double harmonic_db(const std::vector<double>& amplitude, std::size_t harmonic) {
  return db(amplitude[bin_1k * harmonic]);
}

void curves_have_their_signature() {
  for (const int model : {models::tape, models::transistor, models::diode, models::clip}) {
    const auto s = spectrum(shaper(model, 12.0), bin_1k, 0.5);
    REQUIRE_GT(harmonic_db(s, 3), -40.0, "a driven symmetric curve has odd harmonics");
    require(harmonic_db(s, 2) < -110.0 && harmonic_db(s, 4) < -110.0,
            "a symmetric curve without bias has no even harmonics: model " + std::to_string(model) +
                " " + std::to_string(harmonic_db(s, 2)));
  }
  const auto tube = spectrum(shaper(models::tube, 12.0), bin_1k, 0.5);
  REQUIRE_GT(harmonic_db(tube, 2), -50.0, "the tube's asymmetry makes a second harmonic");
  const auto biased = spectrum(shaper(models::tape, 12.0, 0.5), bin_1k, 0.5);
  REQUIRE_GT(harmonic_db(biased, 2), -40.0, "bias makes even harmonics");
  const auto octave = spectrum(shaper(models::rectify, 12.0), bin_1k, 0.5);
  REQUIRE_GT(harmonic_db(octave, 2), harmonic_db(octave, 1) + 20.0,
             "the rectifier's output is an octave up");
}

void the_mix_is_phase_aligned() {
  const auto input = sine(10'000.0, 0.01, 48'000);
  std::array<Stereo, 3> out;
  for (std::size_t i = 0; i < 3; ++i) {
    auto values = shaper(models::tape, 0.0);
    values[at("forge.auto_gain")] = 0.0;
    values[at("forge.mix")] = 0.5 * static_cast<double>(i);
    auto runtime = make_effect(forge_engine(), values);
    out[i] = process(*runtime, input);
  }
  const double dry = rms(out[0].left, 24'000, 48'000);
  const double half = rms(out[1].left, 24'000, 48'000);
  const double wet = rms(out[2].left, 24'000, 48'000);
  require(std::fabs(db(half / dry)) < 0.01 && std::fabs(db(wet / dry)) < 0.01,
          "an unsaturated wet path lines up with the dry one at 10 kHz: " +
              std::to_string(db(half / dry)));
  double worst = 0.0;
  for (std::size_t i = 0; i < input.size(); ++i) {
    worst = std::max(worst, std::fabs(out[1].left[i] - 0.5 * (out[0].left[i] + out[2].left[i])));
  }
  REQUIRE_LT(worst, 1e-6, "the mix is a linear blend");
}

void live_model_switches_are_click_free() {
  const std::array<std::array<int, 2>, 4> switches{{{models::tape, models::clip},
                                                    {models::tube, models::fold},
                                                    {models::diode, models::rectify},
                                                    {models::sine_fold, models::tape}}};
  const auto input = sine(220.0, 0.5, 48'000);
  for (const auto& pair : switches) {
    auto values = shaper(pair[0], 12.0);
    auto runtime = make_effect(forge_engine(), values);
    auto next = values;
    next[at("forge.model")] = pair[1];
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
    REQUIRE_LT(across, std::max(steady, after) * 1.1 + 1e-3,
               "switch " + std::to_string(pair[0]) + " -> " + std::to_string(pair[1]) +
                   " is smooth: " + std::to_string(across) + " vs " +
                   std::to_string(std::max(steady, after)));
  }
}

void telemetry_draws_the_curve() {
  for (const int model :
       {models::tape, models::tube, models::transistor, models::diode, models::clip}) {
    auto runtime = make_effect(forge_engine(), shaper(model, 18.0, 0.2));
    (void)process(*runtime, sine(1'000.0, 0.25, 48'000));
    std::vector<float> t;
    REQUIRE_TRUE(runtime->read_telemetry(t), "telemetry");
    REQUIRE_EQ(t.size(), forge::telemetry::size, "telemetry");
    const auto curve = std::span(t).subspan(forge::telemetry::curve, forge::curve_points);
    REQUIRE_EQ(curve[forge::curve_points / 2], 0.0F, "the curve passes through the origin");
    for (std::size_t i = 1; i < curve.size(); ++i) {
      REQUIRE_GE(curve[i], curve[i - 1], "the curve is monotonic: model " + std::to_string(model));
    }
    REQUIRE_GT(t[forge::telemetry::input_rms], 0.1F, "the meters read");
    REQUIRE_GT(t[forge::telemetry::output_rms], 0.1F, "the meters read");
    REQUIRE_LT(t[forge::telemetry::makeup_db], 0.0F, "a driven curve's makeup cuts");
  }
}

struct Golden {
  std::string_view name;
  double level_db;
  double centroid_hz;
};

void presets_match_golden_fingerprints() {
  const std::array<Golden, 12> golden{{
      {"Warm Tape", -13.5986, 7424.75},
      {"Tube Warmth", -13.9466, 7581.98},
      {"Hot Tube", -15.0511, 3749.22},
      {"Transistor Crunch", -15.708, 3190.35},
      {"Diode Grit", -15.1312, 2636.19},
      {"Hard Clipper", -12.3238, 7601.38},
      {"West Coast Fold", -15.711, 4199.67},
      {"Sine Shimmer", -12.9204, 7558.52},
      {"Octave Rectifier", -15.2776, 7677.26},
      {"Parallel Smash", -13.3979, 4105.01},
      {"Broken Speaker", -17.9654, 1606.25},
      {"Init", -12.4657, 7647.61},
  }};
  REQUIRE_EQ(golden.size(), table().presets.size(), "every preset has a golden");
  auto input = noise(0.05, 48'000);
  for (const double hz : {110.0, 220.0, 329.63}) {
    const auto tone = sine(hz, 0.2, 48'000);
    for (std::size_t i = 0; i < input.size(); ++i) {
      input.left[i] += tone.left[i];
      input.right[i] += tone.right[i];
    }
  }
  bool ok = true;
  for (const auto& entry : golden) {
    auto runtime = make_effect(forge_engine(), preset(table(), entry.name));
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

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "native forge contract passed",
      {
          {"aliases_stay_below_minus_90_dbfs", [] { aliases_stay_below_minus_90_dbfs(); }},
          {"auto_gain_matches_level", [] { auto_gain_matches_level(); }},
          {"colour_is_transparent_at_low_level", [] { colour_is_transparent_at_low_level(); }},
          {"curves_have_their_signature", [] { curves_have_their_signature(); }},
          {"the_mix_is_phase_aligned", [] { the_mix_is_phase_aligned(); }},
          {"live_model_switches_are_click_free", [] { live_model_switches_are_click_free(); }},
          {"telemetry_draws_the_curve", [] { telemetry_draws_the_curve(); }},
          {"presets_match_golden_fingerprints", [] { presets_match_golden_fingerprints(); }},
      });
}
