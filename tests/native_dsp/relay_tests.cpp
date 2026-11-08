#include "effect_test_support.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/relay.hpp"
#include "oscillate/test/audio_constants.hpp"
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

// Relay (ADR-0040) against measured oracles: echo times to the sample and
// repeats falling by the feedback, synced and offset times, ping-pong and
// mono routing, digital time changes that keep pitch and tape ones that
// bend it, wow against its formula, bounded self-oscillation, diffusion that
// smears without moving the echo, ducking, the analog line's darkening,
// click-free structural switches and goldens.
namespace {
using namespace effect_test;
namespace relay = oscillate::native_dsp::effects::relay;

const effects::EffectEngine& relay_engine() { return relay::engine(); }
const EngineDescriptor& table() { return *relay_engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }
std::vector<double> init() { return preset(table(), "Init"); }

constexpr double rate = oscillate::test::sample_rate_48k_hz;

// A 10 ms Hann-windowed 1 kHz burst at frame zero, on the chosen channels.
Stereo burst(std::size_t frames, bool left = true, bool right = true) {
  Stereo out{std::vector<double>(frames), std::vector<double>(frames)};
  for (std::size_t i = 0; i < 480; ++i) {
    const double t = static_cast<double>(i);
    const double x = 0.5 * oscillate::native_dsp::dsp::hann(t, 480.0) *
                     std::sin(2.0 * std::numbers::pi * 1'000.0 * t / rate);
    out.left[i] = left ? x : 0.0;
    out.right[i] = right ? x : 0.0;
  }
  return out;
}

// The burst's level at exactly `lag`: its correlation with the output,
// over its own energy.
double gain_at(const std::vector<double>& y, const std::vector<double>& b, std::size_t lag) {
  double energy = 0.0;
  double r = 0.0;
  for (std::size_t i = 0; i < 480; ++i) {
    energy += b[i] * b[i];
    r += b[i] * y[lag + i];
  }
  return r / energy;
}

Stereo impulse(std::size_t frames) {
  Stereo out{std::vector<double>(frames), std::vector<double>(frames)};
  out.left[0] = 1.0;
  out.right[0] = 1.0;
  return out;
}

// The loudest sample within +-100 frames of `expected`.
std::size_t peak_at(const std::vector<double>& y, std::size_t expected) {
  std::size_t best = expected - 100;
  for (std::size_t i = expected - 100; i <= expected + 100; ++i) {
    if (std::fabs(y[i]) > std::fabs(y[best])) {
      best = i;
    }
  }
  return best;
}

Stereo run(const std::vector<double>& values, const Stereo& input, double bpm = 120.0) {
  auto runtime = make_effect(relay_engine(), values);
  return process(*runtime, input, {.bpm = bpm});
}

void echoes_land_on_time_and_decay_by_feedback() {
  auto values = init();
  values[at("relay.feedback")] = 0.5;
  values[at("relay.mix")] = 1.0;
  // The loop's cuts spread an impulse by a sample or so per pass.
  const auto clicks = run(values, impulse(110'000));
  for (std::size_t n = 1; n <= 4; ++n) {
    const auto lag = peak_at(clicks.left, n * 24'000);
    require(lag >= n * 24'000 && lag <= n * 24'000 + n,
            "echo " + std::to_string(n) + " lands at n x 500 ms: " + std::to_string(lag));
  }
  const auto input = burst(110'000);
  const auto out = run(values, input);
  REQUIRE_LT(std::fabs(db(gain_at(out.left, input.left, 24'000))), 0.05,
             "the first repeat is at unity");
  for (std::size_t n = 1; n < 4; ++n) {
    const double ratio = db(gain_at(out.left, input.left, (n + 1) * 24'000) /
                            gain_at(out.left, input.left, n * 24'000));
    REQUIRE_LT(std::fabs(ratio - db(0.5)), 0.05,
               "each repeat falls by the feedback: " + std::to_string(ratio));
  }
}

void sync_and_offset_set_the_times() {
  auto values = init();
  values[at("relay.sync")] = 1.0;
  values[at("relay.division")] = 8.0; // 1/4
  values[at("relay.offset")] = 0.5;
  values[at("relay.feedback")] = 0.0;
  values[at("relay.mix")] = 1.0;
  const auto out = run(values, impulse(60'000), 100.0);
  // A quarter at 100 BPM is 0.6 s; the right channel runs 50% longer.
  REQUIRE_EQ(peak_at(out.left, 28'800), 28'800, "a synced quarter at 100 BPM");
  REQUIRE_EQ(peak_at(out.right, 43'200), 43'200, "the right channel is offset");
}

void ping_pong_alternates_and_mono_spreads() {
  auto values = init();
  values[at("relay.routing")] = 1.0; // Ping-Pong
  values[at("relay.feedback")] = 0.7;
  values[at("relay.mix")] = 1.0;
  const auto input = burst(80'000, true, false);
  const auto out = run(values, input);
  for (std::size_t n = 1; n <= 3; ++n) {
    const bool on_left = n % 2 == 1;
    const auto& hot = on_left ? out.left : out.right;
    const auto& cold = on_left ? out.right : out.left;
    // The mono sum feeds the left line: half the burst.
    const double expected = 0.5 * std::pow(0.7, static_cast<double>(n - 1));
    const double gain = gain_at(hot, input.left, n * 24'000);
    REQUIRE_LT(std::fabs(gain - expected), expected * 0.01,
               "ping-pong repeat " + std::to_string(n) + " on its side: " + std::to_string(gain));
    REQUIRE_LT(std::fabs(gain_at(cold, input.left, n * 24'000)), tolerance::float_round_trip,
               "and silent on the other");
  }
  values[at("relay.routing")] = 2.0; // Mono
  const auto mono = run(values, input);
  REQUIRE_EQ(mono.left, mono.right, "mono routing sends one repeat to both sides");
  REQUIRE_LT(std::fabs(gain_at(mono.right, input.left, 24'000) - 0.5), 0.005,
             "a left-only input repeats on the right too");
}

// Zero-crossing frequency over 10 ms windows.
std::vector<double> frequencies(const std::vector<double>& y, std::size_t begin, std::size_t end) {
  std::vector<double> out;
  for (std::size_t start = begin; start + 480 <= end; start += 480) {
    out.push_back(measured_hz(y, start, start + 480, rate));
  }
  return out;
}

void digital_time_changes_keep_pitch_and_tape_bends_it() {
  for (const bool tape : {false, true}) {
    auto values = init();
    values[at("relay.character")] = tape ? 1.0 : 0.0;
    values[at("relay.time")] = 300.0;
    values[at("relay.feedback")] = 0.0;
    values[at("relay.mix")] = 1.0;
    auto runtime = make_effect(relay_engine(), values);
    auto next = values;
    next[at("relay.time")] = 500.0;
    const auto out = process(*runtime, sine(1'000.0, 0.5, 96'000),
                             {.at = {48'000}, .before = [&](std::uint64_t) {
                                require(runtime->offer_values(next, 1), "time change offered");
                              }});
    const auto hz = frequencies(out.left, 48'480, 72'000);
    const auto [low, high] = std::ranges::minmax_element(hz);
    if (tape) {
      REQUIRE_LT(*low, 900.0, "a tape time change bends the pitch: " + std::to_string(*low));
    } else {
      require(*low > 995.0 && *high < 1'005.0,
              "a digital time change keeps the pitch: " + std::to_string(*low) + " - " +
                  std::to_string(*high));
    }
  }
}

void wow_follows_its_formula() {
  auto values = init();
  values[at("relay.character")] = 1.0;
  values[at("relay.feedback")] = 0.0;
  values[at("relay.mix")] = 1.0;
  values[at("relay.mod_depth")] = 1.0;
  values[at("relay.mod_rate")] = 1.0;
  values[at("relay.high_cut")] = 20'000.0;
  auto runtime = make_effect(relay_engine(), values);
  const auto out = process(*runtime, sine(1'000.0, 0.05, std::size_t{4} * 48'000));
  // Demodulate the delay over one-millisecond windows.
  const double w = 2.0 * std::numbers::pi * 1'000.0 / rate;
  std::vector<double> delay_ms;
  double unwrapped = 0.0;
  double previous = 0.0;
  for (std::size_t start = 48'000; start + 48 < out.left.size(); start += 48) {
    double i = 0.0;
    double q = 0.0;
    for (std::size_t n = start; n < start + 48; ++n) {
      i += out.left[n] * std::cos(w * static_cast<double>(n));
      q += out.left[n] * std::sin(w * static_cast<double>(n));
    }
    const double phase = std::atan2(-i, q);
    double step = phase - previous;
    step -= 2.0 * std::numbers::pi * std::round(step / (2.0 * std::numbers::pi));
    unwrapped += step;
    previous = phase;
    delay_ms.push_back(unwrapped / w / 48.0);
  }
  const auto [low, high] = std::ranges::minmax_element(delay_ms);
  double expected_low = 1e9;
  double expected_high = -1e9;
  for (std::size_t k = 0; k < delay_ms.size(); ++k) {
    const double t = (48'000.0 + 48.0 * static_cast<double>(k) + 24.0) / rate;
    const double d = 500.0 * (1.0 + 0.003 * std::sin(2.0 * std::numbers::pi * t) +
                              0.0006 * std::sin(2.0 * std::numbers::pi * 11.3 * t));
    expected_low = std::min(expected_low, d);
    expected_high = std::max(expected_high, d);
  }
  const double swing = *high - *low;
  const double expected = expected_high - expected_low;
  REQUIRE_LT(std::fabs(swing - expected), expected * 0.05,
             "wow and flutter swing the delay as designed: " + std::to_string(swing) + " vs " +
                 std::to_string(expected));
}

void tape_self_oscillation_is_bounded() {
  auto values = init();
  values[at("relay.character")] = 1.0;
  values[at("relay.time")] = 200.0;
  values[at("relay.feedback")] = 1.1;
  values[at("relay.mix")] = 1.0;
  values[at("relay.low_cut")] = 80.0;
  values[at("relay.high_cut")] = 6'000.0;
  const auto out = run(values, burst(std::size_t{10} * 48'000));
  const double late = peak(out.left, std::size_t{9} * 48'000);
  require(late < 2.0 && late > 0.05,
          "feedback past unity sustains but stays bounded: " + std::to_string(late));
}

double spread_ms(const std::vector<double>& y, std::size_t begin, std::size_t end) {
  // RMS duration of the echo's energy about its centroid.
  double total = 0.0;
  double centroid = 0.0;
  for (std::size_t i = begin; i < end; ++i) {
    total += y[i] * y[i];
    centroid += static_cast<double>(i) * y[i] * y[i];
  }
  centroid /= total;
  double spread = 0.0;
  for (std::size_t i = begin; i < end; ++i) {
    const double d = static_cast<double>(i) - centroid;
    spread += d * d * y[i] * y[i];
  }
  return std::sqrt(spread / total) / 48.0;
}

double centroid_ms(const std::vector<double>& y, std::size_t begin, std::size_t end) {
  double total = 0.0;
  double centroid = 0.0;
  for (std::size_t i = begin; i < end; ++i) {
    total += y[i] * y[i];
    centroid += static_cast<double>(i) * y[i] * y[i];
  }
  return centroid / total / 48.0;
}

void diffusion_smears_without_moving_the_echo() {
  std::array<double, 2> spread{};
  std::array<double, 2> centre{};
  for (std::size_t k = 0; k < 2; ++k) {
    auto values = init();
    values[at("relay.feedback")] = 0.6;
    values[at("relay.diffusion")] = k == 0 ? 0.0 : 1.0;
    values[at("relay.mix")] = 1.0;
    const auto out = run(values, burst(60'000));
    // The second repeat, through the chain twice.
    spread[k] = spread_ms(out.left, 48'000 - 2'400, 48'000 + 4'800);
    centre[k] = centroid_ms(out.left, 48'000 - 2'400, 48'000 + 4'800);
  }
  REQUIRE_GT(spread[1], spread[0] * 3.0,
             "diffusion smears the repeat: " + std::to_string(spread[0]) + " -> " +
                 std::to_string(spread[1]) + " ms");
  REQUIRE_LT(std::fabs(centre[1] - centre[0]), 6.0,
             "but keeps it near its time: " + std::to_string(centre[1] - centre[0]) + " ms");
}

void ducking_tucks_repeats_under_the_input() {
  auto values = init();
  values[at("relay.ducking")] = 1.0;
  values[at("relay.mix")] = 1.0;
  values[at("relay.feedback")] = 0.6;
  auto input = sine(440.0, 0.5, std::size_t{3} * 48'000);
  std::fill(input.left.begin() + 48'000, input.left.end(), 0.0);
  std::fill(input.right.begin() + 48'000, input.right.end(), 0.0);
  auto runtime = make_effect(relay_engine(), values);
  const auto during = process(
      *runtime, Stereo{std::vector<double>(input.left.begin(), input.left.begin() + 48'000),
                       std::vector<double>(input.right.begin(), input.right.begin() + 48'000)});
  std::vector<float> t;
  require(runtime->read_telemetry(t), "telemetry");
  REQUIRE_LT(std::fabs(t[relay::telemetry::duck_db] + 30.0F), 0.5F,
             "a -6 dBFS input ducks the repeats by 30 dB");
  const double ducked = rms(during.left, 36'000, 48'000);
  const auto after =
      process(*runtime,
              Stereo{std::vector<double>(input.left.begin() + 48'000, input.left.end()),
                     std::vector<double>(input.right.begin() + 48'000, input.right.end())},
              {}, 48'000);
  const double released = rms(after.left, 24'000, 48'000);
  REQUIRE_GT(db(released / ducked), 15.0,
             "repeats come back once the input stops: " + std::to_string(db(released / ducked)));
}

void the_analog_line_darkens_with_time() {
  std::array<double, 2> centroid{};
  const std::array<double, 2> times{60.0, 900.0};
  for (std::size_t k = 0; k < 2; ++k) {
    auto values = init();
    values[at("relay.character")] = 2.0;
    values[at("relay.time")] = times[k];
    values[at("relay.feedback")] = 0.0;
    values[at("relay.mix")] = 1.0;
    auto input = noise(0.3, 120'000);
    std::fill(input.left.begin() + 16'384, input.left.end(), 0.0);
    std::fill(input.right.begin() + 16'384, input.right.end(), 0.0);
    const auto out = run(values, input);
    const auto start = static_cast<std::size_t>(times[k] * 48.0);
    centroid[k] = centroid_hz(out.left, start);
  }
  REQUIRE_LT(centroid[1], centroid[0] * 0.6,
             "a longer analog delay is darker: " + std::to_string(centroid[0]) + " -> " +
                 std::to_string(centroid[1]));
}

void structural_switches_are_click_free() {
  struct Switch {
    std::string_view id;
    double from;
    double to;
  };
  for (const Switch s : {Switch{"relay.character", 0, 1}, Switch{"relay.character", 1, 2},
                         Switch{"relay.routing", 0, 1}, Switch{"relay.routing", 1, 2}}) {
    auto values = init();
    values[at("relay.time")] = 50.0;
    values[at("relay.feedback")] = 0.5;
    values[at("relay.mix")] = 1.0;
    values[at(s.id)] = s.from;
    auto runtime = make_effect(relay_engine(), values);
    auto next = values;
    next[at(s.id)] = s.to;
    const auto out = process(*runtime, sine(220.0, 0.3, 48'000),
                             {.at = {24'000}, .before = [&](std::uint64_t) {
                                require(runtime->offer_values(next, 1), "switch offered");
                              }});
    double steady = 0.0;
    for (std::size_t i = 12'001; i < 24'000; ++i) {
      steady = std::max(steady, std::fabs(out.left[i] - out.left[i - 1]));
    }
    double after = 0.0;
    for (std::size_t i = 30'000; i < 48'000; ++i) {
      after = std::max(after, std::fabs(out.left[i] - out.left[i - 1]));
    }
    double across = 0.0;
    for (std::size_t i = 24'000; i < 30'000; ++i) {
      across = std::max(across, std::fabs(out.left[i] - out.left[i - 1]));
    }
    REQUIRE_LT(across, std::max(steady, after) * 1.2 + 1e-3,
               std::string(s.id) + " switch is smooth: " + std::to_string(across) + " vs " +
                   std::to_string(std::max(steady, after)));
  }
}

struct Golden {
  std::string_view name;
  double level_db;
  double centroid_hz;
};

void presets_match_golden_fingerprints() {
  const std::array<Golden, 13> golden{{
      {"Quarter Echo", -33.2155, 5399.78},
      {"Dotted Eighth", -39.1645, 5077.99},
      {"Slapback", -54.7137, 5258.65},
      {"Tape Echo", -38.0463, 2899.04},
      {"Runaway Tape", -35.2148, 2919.75},
      {"Analog Bucket", -36.9768, 2213.2},
      {"Ping-Pong Eighths", -45.7275, 5482.12},
      {"Wide Dotted Ping-Pong", -39.8867, 5013.89},
      {"Ducked Vocal", -45.0857, 4971.02},
      {"Diffuse Ambience", -30.6281, 3069.95},
      {"Dub Siren", -38.3559, 1407.96},
      {"Haas Widener", -60.9881, 18.8248},
      {"Init", -28.4881, 9266.48},
  }};
  REQUIRE_EQ(golden.size(), table().presets.size(), "every preset has a golden");
  // Quarter-note chord stabs with noise, then silence for the repeats.
  auto input = noise(0.05, 96'000);
  for (std::size_t i = 0; i < 48'000; ++i) {
    const double env = (i % 24'000) < 6'000 ? 1.0 : 0.0;
    for (const double hz : {220.0, 277.18, 329.63}) {
      const double x =
          0.15 * env * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate);
      input.left[i] += x;
      input.right[i] += x;
    }
  }
  for (std::size_t i = 48'000; i < input.size(); ++i) {
    input.left[i] = 0.0;
    input.right[i] = 0.0;
  }
  bool ok = true;
  for (const auto& entry : golden) {
    const auto out = run(preset(table(), entry.name), input);
    const double level = db(rms(out.left, 48'000, 96'000));
    const double centroid = centroid_hz(out.left, 50'000);
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
    echoes_land_on_time_and_decay_by_feedback();
    sync_and_offset_set_the_times();
    ping_pong_alternates_and_mono_spreads();
    digital_time_changes_keep_pitch_and_tape_bends_it();
    wow_follows_its_formula();
    tape_self_oscillation_is_bounded();
    diffusion_smears_without_moving_the_echo();
    ducking_tucks_repeats_under_the_input();
    the_analog_line_darkens_with_time();
    structural_switches_are_click_free();
    presets_match_golden_fingerprints();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "native relay contract passed\n";
  return 0;
}
