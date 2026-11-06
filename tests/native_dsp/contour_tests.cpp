#include "effect_test_support.hpp"

#include "oscillate/native_dsp/effects/contour.hpp"
#include "oscillate/test/check.hpp"

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

// Contour (ADR-0040) against analytic oracles: Butterworth responses for the
// SVF modes, a notch null, band-pass shape, the character models' slopes, comb
// peaks, a phase-aligned dry/wet mix, click-free live model switches, the
// envelope follower on input and sidechain, the stereo LFO and goldens.
namespace {
using namespace effect_test;
namespace contour = oscillate::native_dsp::effects::contour;

const effects::EffectEngine& contour_engine() { return contour::engine(); }
const EngineDescriptor& table() { return *contour_engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }

// Model choices by their public labels (the table's order is persisted).
int model_named(std::string_view name) {
  const auto& choices = table().parameters[at("contour.model")].choices;
  for (std::size_t i = 0; i < choices.size(); ++i) {
    if (choices[i] == name) {
      return static_cast<int>(i);
    }
  }
  throw std::runtime_error("unknown model " + std::string(name));
}
namespace M {
const int lp12 = model_named("LP 12");
const int lp24 = model_named("LP 24");
const int bp12 = model_named("BP 12");
const int hp12 = model_named("HP 12");
const int hp24 = model_named("HP 24");
const int notch = model_named("Notch");
const int ladder = model_named("Ladder");
const int ota = model_named("OTA");
const int sem = model_named("SEM");
const int comb_plus = model_named("Comb +");
const int comb_minus = model_named("Comb -");
} // namespace M

// Init: the filter only, flat resonance, no modulation, fully wet.
std::vector<double> filter(int model, double cutoff, double resonance = 0.0) {
  auto values = preset(table(), "Init");
  values[at("contour.model")] = model;
  values[at("contour.cutoff")] = cutoff;
  values[at("contour.resonance")] = resonance;
  return values;
}

// Steady-state gain of a sine through `values`, in dB.
double gain_db(const std::vector<double>& values, double hz) {
  auto runtime = make_effect(contour_engine(), values);
  const auto input = sine(hz, 0.25, 48'000);
  const auto out = process(*runtime, input);
  return db(rms(out.left, 24'000, 48'000) / rms(input.left, 24'000, 48'000));
}

double butterworth_low_db(double hz, double cutoff, int order) {
  return -10.0 * std::log10(1.0 + std::pow(hz / cutoff, 2.0 * order));
}

void svf_modes_match_butterworth() {
  const double cutoff = 1'000.0;
  struct Case {
    int model;
    int order;
    bool high;
  };
  for (const Case c : {Case{M::lp12, 2, false}, Case{M::lp24, 4, false}, Case{M::hp12, 2, true},
                       Case{M::hp24, 4, true}}) {
    for (const double ratio : {0.25, 0.5, 1.0, 2.0, 4.0}) {
      const double hz = cutoff * ratio;
      const double expected =
          butterworth_low_db(c.high ? cutoff * cutoff / hz : hz, cutoff, c.order);
      if (expected < -45.0) {
        continue;
      }
      const double actual = gain_db(filter(c.model, cutoff), hz);
      REQUIRE_LT(std::fabs(actual - expected), 0.25,
                 "model " + std::to_string(c.model) + " at " + std::to_string(hz) + " Hz: " +
                     std::to_string(actual) + " dB, expected " + std::to_string(expected));
    }
  }
}

void notch_and_band_pass_have_their_shape() {
  REQUIRE_LT(gain_db(filter(M::notch, 1'000.0), 1'000.0), -40.0, "the notch nulls its centre");
  REQUIRE_LT(std::fabs(gain_db(filter(M::notch, 1'000.0), 100.0)), 0.2,
             "the notch passes a decade below");
  // A second-order band-pass at Q = 0.707: |H| = x / sqrt((1 - x^2)^2 + 2x^2).
  for (const double ratio : {0.25, 1.0, 4.0}) {
    const double expected =
        db(ratio / std::sqrt((1.0 - ratio * ratio) * (1.0 - ratio * ratio) + 2.0 * ratio * ratio));
    const double actual = gain_db(filter(M::bp12, 1'000.0), 1'000.0 * ratio);
    REQUIRE_LT(std::fabs(actual - expected), 0.25,
               "band-pass at " + std::to_string(ratio) + "x: " + std::to_string(actual) +
                   " dB, expected " + std::to_string(expected));
  }
}

void character_models_are_low_passes() {
  for (const int model : {M::ladder, M::ota, M::sem}) {
    auto values = filter(model, 1'000.0);
    values[at("contour.morph")] = 0.0; // SEM: fully low-pass.
    const double pass = gain_db(values, 125.0);
    const double stop = gain_db(values, 8'000.0);
    REQUIRE_LT(std::fabs(pass), 3.0,
               "model " + std::to_string(model) + " passes below cutoff: " + std::to_string(pass));
    REQUIRE_LT(stop - pass, (model == M::sem ? -30.0 : -60.0),
               "model " + std::to_string(model) + " rolls off: " + std::to_string(stop - pass));
  }
}

void combs_peak_where_they_should() {
  const double f0 = 200.0;
  const auto plus = filter(M::comb_plus, f0, 0.9);
  const auto minus = filter(M::comb_minus, f0, 0.9);
  const double plus_peak = gain_db(plus, f0);
  const double plus_trough = gain_db(plus, 1.5 * f0);
  const double minus_peak = gain_db(minus, 1.5 * f0);
  const double minus_trough = gain_db(minus, f0);
  REQUIRE_GT(plus_peak - plus_trough, 12.0,
             "Comb + peaks at its harmonics: " + std::to_string(plus_peak - plus_trough));
  REQUIRE_GT(minus_peak - minus_trough, 12.0,
             "Comb - peaks between them: " + std::to_string(minus_peak - minus_trough));
}

void the_mix_is_phase_aligned() {
  const auto input = sine(1'000.0, 0.25, 48'000);
  std::array<Stereo, 3> out;
  for (std::size_t i = 0; i < 3; ++i) {
    auto values = filter(M::lp12, 20'000.0);
    values[at("contour.mix")] = 0.5 * static_cast<double>(i);
    auto runtime = make_effect(contour_engine(), values);
    out[i] = process(*runtime, input);
  }
  const double reference = rms(input.left, 24'000, 48'000);
  for (std::size_t i = 0; i < 3; ++i) {
    const double level = db(rms(out[i].left, 24'000, 48'000) / reference);
    REQUIRE_LT(std::fabs(level), 0.05,
               "a transparent filter at mix " + std::to_string(i) +
                   "/2 keeps level: " + std::to_string(level));
  }
  double worst = 0.0;
  for (std::size_t n = 0; n < input.size(); ++n) {
    worst = std::max(worst, std::fabs(out[1].left[n] - 0.5 * (out[0].left[n] + out[2].left[n])));
  }
  REQUIRE_LT(worst, 1e-6, "mix is a linear blend of aligned dry and wet");
}

// The largest sample step across a live switch, against the steady signal's.
void live_model_switches_are_click_free() {
  const std::array<std::array<int, 2>, 4> switches{
      {{M::lp24, M::hp12}, {M::lp24, M::comb_plus}, {M::ladder, M::sem}, {M::notch, M::ota}}};
  const auto input = sine(220.0, 0.5, 48'000);
  for (const auto& pair : switches) {
    auto values = filter(pair[0], 1'500.0, 0.3);
    auto runtime = make_effect(contour_engine(), values);
    auto next = values;
    next[at("contour.model")] = pair[1];
    const auto out =
        process(*runtime, input, {.at = {24'000}, .before = [&](std::uint64_t) {
                                    require(runtime->offer_values(next, 1), "switch offered");
                                  }});
    double steady = 0.0;
    for (std::size_t n = 12'001; n < 24'000; ++n) {
      steady = std::max(steady, std::fabs(out.left[n] - out.left[n - 1]));
    }
    double across = 0.0;
    for (std::size_t n = 24'000; n < 26'400; ++n) {
      across = std::max(across, std::fabs(out.left[n] - out.left[n - 1]));
    }
    REQUIRE_LT(across, steady * 1.6 + 1e-3,
               "switch " + std::to_string(pair[0]) + " -> " + std::to_string(pair[1]) +
                   " is smooth: " + std::to_string(across) + " vs " + std::to_string(steady));
  }
}

float telemetry_value(effects::NativeEffectRuntime& runtime, std::size_t slot) {
  std::vector<float> values;
  REQUIRE_TRUE(runtime.read_telemetry(values), "telemetry is published");
  REQUIRE_EQ(values.size(), contour::telemetry::size, "telemetry is published");
  return values[slot];
}

void the_envelope_opens_the_filter() {
  const auto values = preset(table(), "Auto-Wah");
  const double base = values[at("contour.cutoff")];
  auto loud = make_effect(contour_engine(), values);
  (void)process(*loud, sine(300.0, 0.5, 9'600));
  const double opened = telemetry_value(*loud, contour::telemetry::cutoff_hz);
  auto quiet = make_effect(contour_engine(), values);
  (void)process(*quiet, sine(300.0, 0.0005, 9'600));
  const double rested = telemetry_value(*quiet, contour::telemetry::cutoff_hz);
  // A peak follower on a -6 dB sine sits near -6 dB: 54/60 of the 36 st.
  const double predicted = base * std::exp2(36.0 * 54.0 / 60.0 / 12.0);
  REQUIRE_LT(std::fabs(std::log2(opened / predicted)) * 12.0, 1.0,
             "a loud input opens the auto-wah by its amount: " + std::to_string(opened));
  REQUIRE_LT(std::fabs(rested - base), base * 0.02,
             "a -66 dB input leaves it at rest: " + std::to_string(rested));
  // Release: the follower falls back once the input stops.
  const Stereo quiet_tail{std::vector<double>(48'000), std::vector<double>(48'000)};
  (void)process(*loud, quiet_tail, {}, 9'600);
  REQUIRE_LT(telemetry_value(*loud, contour::telemetry::cutoff_hz), base * 1.05,
             "the envelope releases");
}

void the_sidechain_ducks_the_filter() {
  const auto values = preset(table(), "Ducked by Sidechain");
  const double base = values[at("contour.cutoff")];
  const auto input = noise(0.1, 9'600);
  const auto kick = sine(60.0, 0.8, 9'600);
  auto keyed = make_effect(contour_engine(), values);
  (void)process(*keyed, input, {.sidechain = &kick});
  const double ducked = telemetry_value(*keyed, contour::telemetry::cutoff_hz);
  // A 0.8 peak key (-1.9 dB) is 58/60 of the -36 st amount.
  const double predicted = base * std::exp2(-36.0 * (60.0 + db(0.8)) / 60.0 / 12.0);
  REQUIRE_LT(std::fabs(std::log2(ducked / predicted)) * 12.0, 1.0,
             "a loud sidechain closes the filter by its amount: " + std::to_string(ducked));
  auto unrouted = make_effect(contour_engine(), values);
  (void)process(*unrouted, input);
  REQUIRE_LT(std::fabs(telemetry_value(*unrouted, contour::telemetry::cutoff_hz) - base), 1.0,
             "with no sidechain routed the input does not key it");
}

void the_lfo_spreads_in_stereo() {
  const auto input = noise(0.3, 48'000, true);
  for (const double stereo : {0.0, 180.0}) {
    auto values = filter(M::lp24, 1'000.0, 0.4);
    values[at("contour.lfo.depth")] = 24.0;
    values[at("contour.lfo.rate")] = 2.0;
    values[at("contour.lfo.stereo")] = stereo;
    auto runtime = make_effect(contour_engine(), values);
    const auto out = process(*runtime, input);
    if (stereo == 0.0) {
      REQUIRE_EQ(out.left, out.right, "no stereo offset keeps a mono input mono");
    } else {
      std::vector<double> side(out.size());
      for (std::size_t i = 0; i < out.size(); ++i) {
        side[i] = out.left[i] - out.right[i];
      }
      REQUIRE_GT(rms(side, 0, side.size()), 0.3 * rms(out.left, 0, side.size()),
                 "a 180 degree offset spreads the sweep");
    }
  }
}

void tempo_sync_follows_the_transport() {
  auto values = filter(M::lp24, 1'000.0, 0.4);
  values[at("contour.lfo.depth")] = 24.0;
  values[at("contour.lfo.sync")] = 1.0;
  const auto input = noise(0.3, 24'000, true);
  auto slow = make_effect(contour_engine(), values);
  auto fast = make_effect(contour_engine(), values);
  const auto a = process(*slow, input, {.bpm = 90.0});
  const auto b = process(*fast, input, {.bpm = 150.0});
  REQUIRE_NE(a.left, b.left, "a synced LFO follows the tempo");
}

struct Golden {
  std::string_view name;
  double level_db;
  double centroid_hz;
};

void presets_match_golden_fingerprints() {
  const std::array<Golden, 12> golden{{
      {"Warm Low-Pass", -16.6282, 805.446},
      {"Acid Sweep", -15.2014, 639.179},
      {"Auto-Wah", -18.1088, 5331.78},
      {"Juno Pad", -20.9485, 1976.06},
      {"SEM Morph", -15.0466, 11788.6},
      {"Telephone", -22.9283, 6166.25},
      {"DJ High-Pass", -11.5151, 11434.9},
      {"Stereo Notch Sweep", -14.8888, 11780.2},
      {"Metallic Comb", -21.3792, 11519.2},
      {"Ducked by Sidechain", -16.62, 646.498},
      {"Init", -15.0975, 10502},
      {"Dotted Breath", -20.3888, 768.613},
  }};
  REQUIRE_EQ(golden.size(), table().presets.size(), "every preset has a golden");
  auto input = noise(0.2, 48'000);
  const auto chord = sine(220.0, 0.2, 48'000);
  auto pulses = sine(55.0, 0.0, 48'000);
  for (std::size_t i = 0; i < input.size(); ++i) {
    input.left[i] += chord.left[i];
    input.right[i] += chord.right[i];
    // Quarter notes at 120 BPM: 50 ms bursts every 24000 frames.
    if (i % 24'000 < 2'400) {
      pulses.left[i] = pulses.right[i] =
          0.8 * std::sin(2.0 * std::numbers::pi * 55.0 * static_cast<double>(i) / 48e3);
    }
  }
  bool ok = true;
  for (const auto& entry : golden) {
    auto runtime = make_effect(contour_engine(), preset(table(), entry.name));
    const auto out = process(*runtime, input, {.sidechain = &pulses});
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
    svf_modes_match_butterworth();
    notch_and_band_pass_have_their_shape();
    character_models_are_low_passes();
    combs_peak_where_they_should();
    the_mix_is_phase_aligned();
    live_model_switches_are_click_free();
    the_envelope_opens_the_filter();
    the_sidechain_ducks_the_filter();
    the_lfo_spreads_in_stereo();
    tempo_sync_follows_the_transport();
    presets_match_golden_fingerprints();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "native contour contract passed\n";
  return 0;
}
