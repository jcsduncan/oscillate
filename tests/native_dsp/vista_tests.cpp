#include "effect_test_support.hpp"

#include "oscillate/native_dsp/effects/vista.hpp"
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
#include <string>
#include <string_view>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

// Vista (ADR-0040) against exact oracles: the mono sum never depends on any
// width, band, stereoize, mono-bass or safe-width setting; at 100% the
// image is exact (pure mid stays mid, pure side stays side, every tone at
// unity); each band scales its side by its width; mono bass follows the
// Linkwitz-Riley slope; stereoize decorrelates without touching the bass;
// safe width holds the correlation floor and never narrows; rotation,
// balance, swap and polarity; the correlation meter; click-free switches
// and goldens.
namespace {
using namespace effect_test;
namespace vista = oscillate::native_dsp::effects::vista;

const effects::EffectEngine& vista_engine() { return vista::engine(); }
const EngineDescriptor& table() { return *vista_engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }
std::vector<double> natural() { return preset(table(), "Natural"); }
constexpr std::size_t second = 48'000;

Stereo run(const std::vector<double>& values, const Stereo& input) {
  auto runtime = make_effect(vista_engine(), values);
  return process(*runtime, input);
}

Stereo from(const std::vector<double>& left, const std::vector<double>& right) {
  return {left, right};
}

std::vector<double> scaled(const std::vector<double>& x, double k) {
  std::vector<double> out(x.size());
  for (std::size_t i = 0; i < x.size(); ++i) {
    out[i] = k * x[i];
  }
  return out;
}

void the_mono_sum_never_depends_on_width() {
  const auto input = noise(0.3, second);
  const auto reference = run(natural(), input);
  auto extreme = natural();
  extreme[at("vista.bands")] = 3.0;
  extreme[at("vista.width_1")] = 0.0;
  extreme[at("vista.width_2")] = 2.0;
  extreme[at("vista.width_3")] = 0.3;
  extreme[at("vista.width_4")] = 1.7;
  extreme[at("vista.width")] = 1.5;
  extreme[at("vista.stereoize")] = 0.9;
  extreme[at("vista.safe")] = 1.0;
  extreme[at("vista.safe_floor")] = 0.4;
  extreme[at("vista.mono_bass")] = 1.0;
  for (const double mode : {0.0, 1.0}) {
    extreme[at("vista.stereoize_mode")] = mode;
    const auto out = run(extreme, input);
    double worst = 0.0;
    for (std::size_t i = 0; i < input.size(); ++i) {
      worst = std::max(worst, std::fabs((out.left[i] + out.right[i]) -
                                        (reference.left[i] + reference.right[i])));
    }
    REQUIRE_LT(worst, 1e-6,
               "the mono sum is unchanged by any width setting: " + std::to_string(worst));
  }
}

void unity_keeps_the_image_exact() {
  const auto n = noise(0.3, second, true);
  const auto mid = run(natural(), from(n.left, n.left));
  REQUIRE_EQ(mid.left, mid.right, "pure mid stays pure mid");
  const auto side = run(natural(), from(n.left, scaled(n.left, -1.0)));
  for (std::size_t i = 0; i < side.size(); ++i) {
    REQUIRE_EQ(side.left[i], -side.right[i], "pure side stays pure side");
  }
  for (const double hz : {30.0, 1'000.0, 15'000.0}) {
    const auto tone = sine(hz, 0.25, second);
    const auto out = run(natural(), from(tone.left, std::vector<double>(second, 0.0)));
    const double level = db(rms(out.left, second / 2, second) / rms(tone.left, second / 2, second));
    REQUIRE_LT(std::fabs(level), 0.01, "unity is magnitude-flat at " + std::to_string(hz) + " Hz");
    REQUIRE_LT(rms(out.right, second / 2, second), 1e-6, "and does not leak across channels");
  }
}

double side_gain_db(const std::vector<double>& values, double hz) {
  const auto tone = sine(hz, 0.25, second);
  const auto out = run(values, from(tone.left, scaled(tone.left, -1.0)));
  std::vector<double> s(out.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    s[i] = 0.5 * (out.left[i] - out.right[i]);
  }
  return db(rms(s, second / 2, second) / rms(tone.left, second / 2, second));
}

void each_band_scales_its_side() {
  auto values = natural();
  values[at("vista.bands")] = 3.0;
  // Tones at least 1.3 octaves from every crossover, where a fourth-order
  // Linkwitz-Riley band has let go of its neighbours.
  values[at("vista.xover_1")] = 80.0;
  values[at("vista.xover_2")] = 640.0;
  values[at("vista.xover_3")] = 4'000.0;
  const std::array<double, 4> widths{0.5, 1.0, 1.5, 0.75};
  const std::array<double, 4> tones{20.0, 226.0, 1'600.0, 16'000.0};
  for (std::size_t b = 0; b < 4; ++b) {
    values[at("vista.width_1") + b] = widths[b];
  }
  for (std::size_t b = 0; b < 4; ++b) {
    const double gain = side_gain_db(values, tones[b]);
    REQUIRE_LT(std::fabs(gain - db(widths[b])), 0.3,
               "band " + std::to_string(b + 1) + " scales its side: " + std::to_string(gain) +
                   " dB, expected " + std::to_string(db(widths[b])));
  }
  // Fewer bands: the last control covers the rest.
  values[at("vista.bands")] = 1.0;
  REQUIRE_LT(std::fabs(side_gain_db(values, 16'000.0) - db(widths[1])), 0.3,
             "with two bands the second covers the top");
}

void mono_bass_follows_the_linkwitz_riley_slope() {
  auto values = natural();
  values[at("vista.mono_bass")] = 1.0;
  values[at("vista.mono_freq")] = 160.0;
  // Fourth-order Linkwitz-Riley high-pass: |H| = x^4 / (1 + x^4).
  for (const double x : {0.5, 2.0}) {
    const double expected = db(std::pow(x, 4) / (1.0 + std::pow(x, 4)));
    const double gain = side_gain_db(values, 160.0 * x);
    REQUIRE_LT(std::fabs(gain - expected), 0.3,
               "mono bass side gain at " + std::to_string(x) + "x: " + std::to_string(gain) +
                   " dB, expected " + std::to_string(expected));
  }
}

double correlation(const Stereo& s, std::size_t from, std::size_t to) {
  double lr = 0.0;
  double ll = 0.0;
  double rr = 0.0;
  for (std::size_t i = from; i < to; ++i) {
    lr += s.left[i] * s.right[i];
    ll += s.left[i] * s.left[i];
    rr += s.right[i] * s.right[i];
  }
  return lr / std::sqrt(ll * rr);
}

void stereoize_decorrelates_above_the_bass() {
  const auto mono = noise(0.3, second, true);
  double previous = 1.0;
  for (const double amount : {0.25, 0.5, 1.0}) {
    auto values = natural();
    values[at("vista.stereoize")] = amount;
    const double rho = correlation(run(values, mono), second / 4, second);
    REQUIRE_LT(rho, previous - 0.05, "more stereoize, less correlation: " + std::to_string(rho));
    previous = rho;
  }
  // Full stereoize decorrelates everything above 300 Hz: white noise keeps
  // only its bass correlated.
  REQUIRE_LT(previous, 0.15, "full stereoize opens a mono source: " + std::to_string(previous));
  auto values = natural();
  values[at("vista.stereoize")] = 1.0;
  for (const double mode : {0.0, 1.0}) {
    values[at("vista.stereoize_mode")] = mode;
    const auto bass = sine(80.0, 0.3, second);
    const auto out = run(values, bass);
    std::vector<double> s(out.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
      s[i] = 0.5 * (out.left[i] - out.right[i]);
    }
    REQUIRE_LT(db(rms(s, second / 2, second) / rms(out.left, second / 2, second)), -25.0,
               "the bass stays mono");
  }
}

// L = a, R = rho a + sqrt(1 - rho^2) b: correlation rho.
Stereo correlated(double rho, std::size_t frames) {
  const auto a = noise(0.3, frames, true, 11);
  const auto b = noise(0.3, frames, true, 29);
  Stereo out{a.left, std::vector<double>(frames)};
  for (std::size_t i = 0; i < frames; ++i) {
    out.right[i] = rho * a.left[i] + std::sqrt(1.0 - rho * rho) * b.left[i];
  }
  return out;
}

void safe_width_holds_the_floor_and_never_narrows() {
  const auto input = correlated(0.5, 2 * second);
  auto wide = natural();
  wide[at("vista.width")] = 2.0;
  const double unsafe = correlation(run(wide, input), second, 2 * second);
  REQUIRE_LT(unsafe, 0.0, "doubling the width of a rho 0.5 source goes below zero");
  wide[at("vista.safe")] = 1.0;
  wide[at("vista.safe_floor")] = 0.2;
  const double held = correlation(run(wide, input), second, 2 * second);
  require(held > 0.17 && held < 0.3, "safe width holds the floor: " + std::to_string(held));
  // A source already below the floor keeps its own width.
  const auto loose = correlated(0.0, 2 * second);
  const double kept = correlation(run(wide, loose), second, 2 * second);
  REQUIRE_LT(std::fabs(kept), 0.05, "safe width never narrows the input: " + std::to_string(kept));
}

void rotation_and_balance_move_the_field() {
  auto values = natural();
  values[at("vista.rotation")] = 45.0;
  const auto left_only = noise(0.3, second, true);
  const auto out = run(values, from(left_only.left, std::vector<double>(second, 0.0)));
  for (std::size_t i = second / 10; i < second; ++i) {
    REQUIRE_LT(std::fabs(out.left[i] - out.right[i]), tolerance::float_round_trip,
               "45 degrees centres a hard-left source");
  }
  const auto stereo = noise(0.3, second);
  values[at("vista.rotation")] = -30.0;
  const auto rotated = run(values, stereo);
  const double energy_in =
      rms(stereo.left, second / 2, second) * rms(stereo.left, second / 2, second) +
      rms(stereo.right, second / 2, second) * rms(stereo.right, second / 2, second);
  const double energy_out =
      rms(rotated.left, second / 2, second) * rms(rotated.left, second / 2, second) +
      rms(rotated.right, second / 2, second) * rms(rotated.right, second / 2, second);
  REQUIRE_LT(std::fabs(db(energy_out / energy_in)), 0.05, "rotation keeps the energy");
  auto balanced = natural();
  balanced[at("vista.balance")] = 0.5;
  const auto out_b = run(balanced, stereo);
  const auto ref = run(natural(), stereo);
  REQUIRE_LT(std::fabs(db(rms(out_b.left, second / 2, second) / rms(ref.left, second / 2, second)) -
                       db(0.5)),
             0.01, "balance right halves the left");
  REQUIRE_LT(
      std::fabs(db(rms(out_b.right, second / 2, second) / rms(ref.right, second / 2, second))),
      0.01, "balance right halves the left");
}

void swap_and_polarity_are_exact() {
  const auto stereo = noise(0.3, second);
  const auto ref = run(natural(), stereo);
  auto swapped = natural();
  swapped[at("vista.swap")] = 1.0;
  const auto s = run(swapped, stereo);
  auto inverted = natural();
  inverted[at("vista.invert")] = 3.0;
  const auto inv = run(inverted, stereo);
  for (std::size_t i = 0; i < stereo.size(); ++i) {
    REQUIRE_LT(std::fabs(s.left[i] - ref.right[i]), tolerance::float_round_trip,
               "swap exchanges the channels");
    REQUIRE_LT(std::fabs(s.right[i] - ref.left[i]), tolerance::float_round_trip,
               "swap exchanges the channels");
    REQUIRE_LT(std::fabs(inv.left[i] + ref.left[i]), tolerance::float_round_trip,
               "inverting both flips both");
    REQUIRE_LT(std::fabs(inv.right[i] + ref.right[i]), tolerance::float_round_trip,
               "inverting both flips both");
  }
}

void the_correlation_meter_reads_true() {
  for (const double rho : {-0.4, 0.0, 0.6, 1.0}) {
    auto runtime = make_effect(vista_engine(), natural());
    (void)process(*runtime, correlated(rho, 2 * second));
    std::vector<float> t;
    REQUIRE_TRUE(runtime->read_telemetry(t), "telemetry");
    REQUIRE_EQ(t.size(), vista::telemetry::size, "telemetry");
    require(std::fabs(t[vista::telemetry::correlation_in] - rho) < 0.05 &&
                std::fabs(t[vista::telemetry::correlation_out] - rho) < 0.05,
            "the meter reads " + std::to_string(t[vista::telemetry::correlation_out]) +
                " for rho " + std::to_string(rho));
    for (std::size_t b = 0; b < vista::bands; ++b) {
      REQUIRE_LT(std::fabs(t[vista::telemetry::band_correlation + b] - rho), 0.12,
                 "band " + std::to_string(b) + " reads " +
                     std::to_string(t[vista::telemetry::band_correlation + b]));
    }
  }
}

void switches_are_click_free() {
  struct Switch {
    std::string_view id;
    double to;
  };
  const auto input = sine(220.0, 0.3, second);
  auto offset = input;
  for (std::size_t i = 0; i < offset.size(); ++i) {
    offset.right[i] =
        0.3 * std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(i) / 48e3 + 1.0);
  }
  for (const Switch s :
       {Switch{"vista.bands", 0}, Switch{"vista.mono_bass", 1}, Switch{"vista.swap", 1},
        Switch{"vista.invert", 1}, Switch{"vista.stereoize_mode", 1}}) {
    auto values = natural();
    values[at("vista.stereoize")] = 0.5;
    values[at("vista.width_1")] = 0.5;
    auto runtime = make_effect(vista_engine(), values);
    auto next = values;
    next[at(s.id)] = s.to;
    const auto out =
        process(*runtime, offset, {.at = {second / 2}, .before = [&](std::uint64_t) {
                                     require(runtime->offer_values(next, 1), "offered");
                                   }});
    double steady = 0.0;
    double across = 0.0;
    for (std::size_t i = second / 4; i < second / 2; ++i) {
      steady = std::max(steady, std::fabs(out.left[i] - out.left[i - 1]));
    }
    for (std::size_t i = second / 2; i < second / 2 + 2'400; ++i) {
      across = std::max(across, std::fabs(out.left[i] - out.left[i - 1]));
    }
    REQUIRE_LT(across, steady * 1.3 + 1e-3,
               std::string(s.id) + " switches smoothly: " + std::to_string(across) + " vs " +
                   std::to_string(steady));
  }
}

struct Golden {
  std::string_view name;
  double level_db;
  double correlation;
};

void presets_match_golden_fingerprints() {
  const std::array<Golden, 11> golden{{
      {"Natural", -15.21, 0.505382},
      {"Wide Master", -14.5246, 0.286487},
      {"Tight Low End", -15.2191, 0.509016},
      {"Huge Pads", -13.4765, 0.0103642},
      {"Stereoize Vocal", -14.3681, 0.276057},
      {"Haas Guitar", -13.8977, 0.111405},
      {"Narrow Lows, Open Air", -14.608, 0.311272},
      {"Mono Check", -16.4431, 1},
      {"Safe Widen", -14.2602, 0.209647},
      {"Lean Left", -14.3979, 0.471814},
      {"Init", -15.21, 0.505382},
  }};
  REQUIRE_EQ(golden.size(), table().presets.size(), "every preset has a golden");
  const auto input = correlated(0.5, second);
  bool ok = true;
  for (const auto& entry : golden) {
    const auto out = run(preset(table(), entry.name), input);
    const double level = db(rms(out.left, second / 4, second));
    const double rho = correlation(out, second / 4, second);
    if (std::fabs(level - entry.level_db) > 0.05 || std::fabs(rho - entry.correlation) > 0.005) {
      std::cerr << "{\"" << entry.name << "\", " << level << ", " << rho << "},\n";
      ok = false;
    }
  }
  require(ok, "presets match their golden fingerprints");
}

} // namespace

int main() {
  try {
    the_mono_sum_never_depends_on_width();
    unity_keeps_the_image_exact();
    each_band_scales_its_side();
    mono_bass_follows_the_linkwitz_riley_slope();
    stereoize_decorrelates_above_the_bass();
    safe_width_holds_the_floor_and_never_narrows();
    rotation_and_balance_move_the_field();
    swap_and_polarity_are_exact();
    the_correlation_meter_reads_true();
    switches_are_click_free();
    presets_match_golden_fingerprints();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "native vista contract passed\n";
  return 0;
}
