#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/drift.hpp"
#include "oscillate/native_dsp/synth/vintage_effects.hpp"
#include "oscillate/native_dsp/synth/vintage_filters.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/signal.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using namespace oscillate::native_dsp::synth;
constexpr double pi = std::numbers::pi;

using oscillate::test::require;

double db(double gain) { return oscillate::test::gain_to_db(gain, oscillate::test::silence_floor); }

template <typename Process>
double sine_gain(double rate, double hz, Process&& process, double amplitude = 1.0) {
  const auto settle = static_cast<std::size_t>(rate * 0.5);
  const auto measure = static_cast<std::size_t>(rate * 0.5);
  double in = 0.0;
  double out = 0.0;
  for (std::size_t i = 0; i < settle + measure; ++i) {
    const double x = amplitude * std::sin(2.0 * pi * hz * static_cast<double>(i) / rate);
    const double y = process(x);
    if (i >= settle) {
      in += x * x;
      out += y * y;
    }
  }
  return std::sqrt(out / in);
}

double warped(double rate, double hz, double cutoff) {
  return std::tan(pi * hz / rate) / std::tan(pi * cutoff / rate);
}

// ---- OTA cascade ---------------------------------------------------------------

void ota_small_signal_is_four_one_poles() {
  constexpr double rate = oscillate::test::sample_rate_96k_hz;
  constexpr double cutoff = 1'000.0;
  for (double hz : {250.0, 1'000.0, 4'000.0}) {
    OtaLadder filter;
    const auto c = OtaCoefficients::make(prewarp(cutoff / rate), 0.0, 1.0);
    const double w = warped(rate, hz, cutoff);
    const double gain = sine_gain(rate, hz, [&](double x) { return filter.process(x, c); }, 0.005);
    REQUIRE_LT(std::fabs(db(gain) - db(1.0 / ((1.0 + w * w) * (1.0 + w * w)))), 0.1,
               "OTA cascade matches four one-pole stages at zero resonance");
  }
}

void ota_resonance_thins_bass_and_self_oscillates() {
  constexpr double rate = oscillate::test::sample_rate_96k_hz;
  constexpr double cutoff = 1'500.0;
  OtaLadder flat;
  OtaLadder resonant;
  const auto open = OtaCoefficients::make(prewarp(cutoff / rate), 0.0, 1.0);
  const auto peaked = OtaCoefficients::make(prewarp(cutoff / rate), 0.8, 1.0);
  const double bass_flat =
      sine_gain(rate, 100.0, [&](double x) { return flat.process(x, open); }, 0.01);
  const double bass_peaked =
      sine_gain(rate, 100.0, [&](double x) { return resonant.process(x, peaked); }, 0.01);
  REQUIRE_LT(db(bass_peaked), db(bass_flat) - 3.0,
             "resonance costs bass like the hardware, but not all of it");
  REQUIRE_GT(db(bass_peaked), db(bass_flat) - 12.0,
             "resonance costs bass like the hardware, but not all of it");

  OtaLadder filter;
  const auto c = OtaCoefficients::make(prewarp(cutoff / rate), 1.0, 1.0);
  std::vector<double> out(static_cast<std::size_t>(rate));
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = filter.process(i == 0 ? 0.5 : 0.0, c);
  }
  double peak = 0.0;
  double power = 0.0;
  int crossings = 0;
  const std::size_t tail = out.size() / 2;
  for (std::size_t i = tail; i < out.size(); ++i) {
    peak = std::max(peak, std::fabs(out[i]));
    power += out[i] * out[i];
    crossings += out[i - 1] < 0.0 && out[i] >= 0.0 ? 1 : 0;
  }
  const double measured = crossings / (static_cast<double>(out.size() - tail) / rate);
  REQUIRE_GT(std::sqrt(power / static_cast<double>(out.size() - tail)), 0.05,
             "full resonance self-oscillates");
  REQUIRE_LT(peak, 2.0, "self-oscillation is bounded");
  REQUIRE_LT(std::fabs(measured / cutoff - 1.0), 0.12, "self-oscillation sits near cutoff");
}

void ota_stays_finite_under_modulation_and_overdrive() {
  NoiseSource noise;
  noise.reset(3);
  OtaLadder filter;
  for (int i = 0; i < 200'000; ++i) {
    const double cutoff = 20.0 * std::pow(1'000.0, 0.5 + 0.5 * noise.white());
    const auto c = OtaCoefficients::make(prewarp(cutoff / 44'100.0), 1.0, 4.0);
    const double y = filter.process(8.0 * noise.white(), c);
    REQUIRE_TRUE(std::isfinite(y), "OTA stays finite and bounded");
    REQUIRE_LT(std::fabs(y), 50.0, "OTA stays finite and bounded");
  }
}

// ---- SEM morph and HPF ---------------------------------------------------------

void sem_morph_matches_prototypes() {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  constexpr double cutoff = 1'000.0;
  const double q = oscillate::native_dsp::dsp::butterworth_q;
  for (double hz : {250.0, 4'000.0}) {
    const double w = warped(rate, hz, cutoff);
    const double d = std::sqrt((1 - w * w) * (1 - w * w) + (w / q) * (w / q));
    SemFilter low;
    SemFilter high;
    const auto lc = SemCoefficients::make(prewarp(cutoff / rate), 0.0, 0.0);
    const auto hc = SemCoefficients::make(prewarp(cutoff / rate), 0.0, 1.0);
    REQUIRE_LT(std::fabs(db(sine_gain(rate, hz, [&](double x) { return low.process(x, lc); })) -
                         db(1.0 / d)),
               0.05, "morph 0 is the low-pass prototype");
    REQUIRE_LT(std::fabs(db(sine_gain(rate, hz, [&](double x) { return high.process(x, hc); })) -
                         db(w * w / d)),
               0.05, "morph 1 is the high-pass prototype");
  }
  SemFilter notch;
  const auto nc = SemCoefficients::make(prewarp(cutoff / rate), 0.0, 0.5);
  REQUIRE_LT(db(sine_gain(rate, cutoff, [&](double x) { return notch.process(x, nc); })), -40.0,
             "morph 0.5 notches the cutoff");
}

void high_pass_positions_and_switching() {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  for (int position = 1; position <= 3; ++position) {
    HighPass4 filter;
    filter.prepare(rate);
    filter.set_position(position);
    filter.prepare(rate); // Start settled at this position.
    const double corner = hpf_position_hz[static_cast<std::size_t>(position)];
    REQUIRE_LT(
        std::fabs(db(sine_gain(rate, corner, [&](double x) { return filter.process(x); })) + 3.01),
        0.1, "each position is -3 dB at its corner");
  }
  HighPass4 bypass;
  bypass.prepare(rate);
  REQUIRE_LT(std::fabs(db(sine_gain(rate, 40.0, [&](double x) { return bypass.process(x); }))),
             tolerance::float_round_trip, "position 0 is transparent");

  HighPass4 moving;
  moving.prepare(rate);
  double previous = 0.0;
  double largest = 0.0;
  for (int i = 0; i < 48'000; ++i) {
    if (i == 24'000) {
      moving.set_position(3);
    }
    const double y = moving.process(std::sin(2.0 * pi * 220.0 * i / rate));
    if (i > 0) {
      largest = std::max(largest, std::fabs(y - previous));
    }
    previous = y;
  }
  REQUIRE_LT(largest, 0.04, "switching position glides instead of stepping");
}

// ---- Drift and tolerances ------------------------------------------------------

void drift_is_seeded_smooth_and_bounded() {
  DriftSource a;
  DriftSource b;
  DriftSource c;
  for (auto* drift : {&a, &b, &c}) {
    drift->prepare(1'500.0, 0.3);
  }
  a.reset(7);
  b.reset(7);
  c.reset(8);
  double previous = a.value();
  double largest_step = 0.0;
  double different = 0.0;
  double low = 1.0;
  double high = -1.0;
  for (int i = 0; i < 1'500 * 60; ++i) {
    const double x = a.next();
    REQUIRE_EQ(x, b.next(), "equal seeds give identical drift");
    different = std::max(different, std::fabs(x - c.next()));
    largest_step = std::max(largest_step, std::fabs(x - previous));
    low = std::min(low, x);
    high = std::max(high, x);
    previous = x;
    REQUIRE_GE(x, -1.0, "drift is bounded");
    REQUIRE_LE(x, 1.0, "drift is bounded");
  }
  REQUIRE_GT(different, 0.1, "different seeds wander differently");
  REQUIRE_LT(largest_step, 0.005, "drift is smooth at control rate");
  REQUIRE_GT(high - low, 0.5, "drift explores its range over a minute");

  for (std::uint32_t slot = 0; slot < 8; ++slot) {
    const double t = voice_tolerance(slot, 0);
    REQUIRE_GE(t, -1.0, "voice tolerances are deterministic and bounded");
    REQUIRE_LE(t, 1.0, "voice tolerances are deterministic and bounded");
    REQUIRE_EQ(t, voice_tolerance(slot, 0), "voice tolerances are deterministic and bounded");
    REQUIRE_NE(t, voice_tolerance(slot, 1), "voice tolerances differ per slot and index");
    REQUIRE_NE(t, voice_tolerance(slot + 1, 0), "voice tolerances differ per slot and index");
  }
}

// ---- Effects -------------------------------------------------------------------

void ensemble_is_transparent_at_zero_and_wide_at_full() {
  Ensemble ensemble;
  ensemble.prepare(48'000.0);
  ensemble.set_amount(0.0, 0);
  for (int i = 0; i < 1'000; ++i) {
    const double x = std::sin(i * 0.03);
    const auto out = ensemble.process(x, x);
    REQUIRE_EQ(out.left, x, "zero ensemble passes input unchanged");
    REQUIRE_EQ(out.right, x, "zero ensemble passes input unchanged");
  }
  ensemble.set_amount(1.0, 0);
  double width = 0.0;
  for (int i = 0; i < 96'000; ++i) {
    const double x = std::sin(2.0 * pi * 330.0 * i / 48'000.0);
    const auto out = ensemble.process(x, x);
    REQUIRE_TRUE(std::isfinite(out.left), "ensemble is finite");
    REQUIRE_TRUE(std::isfinite(out.right), "ensemble is finite");
    width += std::fabs(out.left - out.right);
  }
  REQUIRE_GT(width / 96'000.0, 0.02, "full ensemble widens a mono source");
}

void delay_echoes_land_on_time_and_decay() {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  BbdDelay delay;
  delay.prepare(rate);
  delay.set(0.25, 0.5, 1.0, false, 0);
  delay.reset();
  std::vector<double> out(48'000);
  for (std::size_t i = 0; i < out.size(); ++i) {
    // A short low tone burst survives the darkening path cleanly.
    const double x = i < 480 ? std::sin(2.0 * pi * 200.0 * static_cast<double>(i) / rate) : 0.0;
    out[i] = delay.process(x, x).left;
  }
  const auto energy = [&](std::size_t from, std::size_t to) {
    double e = 0.0;
    for (std::size_t i = from; i < to; ++i) {
      e += out[i] * out[i];
    }
    return e;
  };
  const double first = energy(12'000, 12'960);
  const double second = energy(24'000, 24'960);
  REQUIRE_LT(energy(1'000, 11'500), 1e-9, "nothing arrives before the delay time");
  REQUIRE_GT(first, 1.0, "the first echo arrives on time");
  REQUIRE_LT(second, first * 0.4, "repeats decay by the feedback amount");
  REQUIRE_GT(second, first * 0.1, "repeats decay by the feedback amount");
  REQUIRE_GT(BbdDelay::tail_seconds(0.25, 0.5), 3.0, "the tail bound covers -80 dB of repeats");
  REQUIRE_EQ(BbdDelay::tail_seconds(0.25, 0.0), 0.25, "the tail bound covers -80 dB of repeats");

  BbdDelay ping;
  ping.prepare(rate);
  ping.set(0.1, 0.6, 1.0, true, 0);
  ping.reset();
  double first_left = 0.0;
  double first_right = 0.0;
  double second_left = 0.0;
  double second_right = 0.0;
  for (int i = 0; i < 12'000; ++i) {
    const double x = i < 240 ? std::sin(2.0 * pi * 200.0 * i / rate) : 0.0;
    const auto y = ping.process(x, x);
    if (i >= 4'800 && i < 5'400) {
      first_left += y.left * y.left;
      first_right += y.right * y.right;
    }
    if (i >= 9'600 && i < 10'200) {
      second_left += y.left * y.left;
      second_right += y.right * y.right;
    }
  }
  REQUIRE_GT(first_left, 10.0 * first_right, "ping-pong repeats alternate sides");
  REQUIRE_GT(second_right, 10.0 * second_left, "ping-pong repeats alternate sides");
}

// The first setting applies before audio starts, without a reset: a short
// delay's first repeat lands on time, not bent by a glide from the default.
void delay_first_setting_applies_at_once() {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  constexpr double short_seconds = 0.02;
  constexpr std::size_t echo_at = 960;
  constexpr std::size_t window = 48;
  constexpr double silent_energy = 1e-12;
  BbdDelay delay;
  delay.prepare(rate);
  delay.set(short_seconds, 0.0, 1.0, false, 0);
  std::vector<double> out(2 * echo_at);
  for (std::size_t i = 0; i < out.size(); ++i)
    out[i] = delay.process(i == 0 ? 1.0 : 0.0, 0.0).left;
  double before = 0.0;
  double around = 0.0;
  for (std::size_t i = window; i + window < echo_at; ++i)
    before += out[i] * out[i];
  for (std::size_t i = echo_at - window; i < echo_at + window; ++i)
    around += out[i] * out[i];
  REQUIRE_LT(before, silent_energy, "the first repeat glided in from the default time");
  REQUIRE_GT(around, silent_energy, "the first repeat did not land on the set time");
}

void tape_is_a_fixed_delay_until_driven_or_wobbled() {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  TapeStage tape;
  tape.prepare(rate);
  tape.set(0.0, 0.0, 0);
  const auto latency = static_cast<std::size_t>(TapeStage::latency_seconds * rate);
  std::vector<double> in(4'800);
  std::vector<double> out(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    in[i] = std::sin(0.01 * static_cast<double>(i));
    out[i] = tape.process(in[i], in[i]).left;
  }
  for (std::size_t i = latency; i < in.size(); ++i) {
    REQUIRE_LT(std::fabs(out[i] - in[i - latency]), tolerance::float_round_trip,
               "neutral tape is a pure fixed delay");
  }

  // Drive adds a second harmonic (asymmetry) without changing level much.
  tape.reset();
  tape.set(1.0, 0.0, 0);
  const double hz = 200.0;
  double fundamental_re = 0.0, fundamental_im = 0.0, second_re = 0.0, second_im = 0.0;
  for (int i = 0; i < 96'000; ++i) {
    const double y = tape.process(0.5 * std::sin(2.0 * pi * hz * i / rate), 0.0).left;
    if (i >= 48'000) {
      const double t = 2.0 * pi * hz * i / rate;
      fundamental_re += y * std::cos(t);
      fundamental_im += y * std::sin(t);
      second_re += y * std::cos(2.0 * t);
      second_im += y * std::sin(2.0 * t);
    }
  }
  const double fundamental = std::hypot(fundamental_re, fundamental_im);
  const double second = std::hypot(second_re, second_im);
  REQUIRE_GT(second / fundamental, 0.003, "driven tape adds a gentle even harmonic");
  REQUIRE_LT(second / fundamental, 0.2, "driven tape adds a gentle even harmonic");
}

void effects_and_filters_do_not_allocate() {
  Ensemble ensemble;
  ensemble.prepare(48'000.0);
  ensemble.set_amount(1.0, 64);
  BbdDelay delay;
  delay.prepare(48'000.0);
  delay.set(0.3, 0.6, 0.5, true, 64);
  TapeStage tape;
  tape.prepare(48'000.0);
  tape.set(0.5, 0.5, 64);
  StereoOtaLadder ota;
  StereoSemFilter sem;
  StereoHighPass4 hpf;
  hpf.prepare(48'000.0);
  hpf.set_position(2);
  DriftSource drift;
  drift.prepare(48'000.0, 1.0);
  drift.reset(1);
  const auto oc = OtaCoefficients::make(0.1, 0.7, 1.5);
  const auto sc = SemCoefficients::make(0.1, 0.3, 0.2);
  counting_allocations = true;
  counted_allocations = 0;
  double sink = 0.0;
  for (int i = 0; i < 4'096; ++i) {
    StereoSample x{std::sin(i * 0.05) + drift.next(), std::cos(i * 0.05)};
    x = hpf.process(sem.process(ota.process(x, oc), sc));
    auto a = ensemble.process(x.left, x.right);
    auto b = delay.process(a.left, a.right);
    auto c = tape.process(b.left, b.right);
    sink += c.left + c.right;
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "vintage primitives never allocate while processing");
  require(std::isfinite(sink), "vintage chain stays finite");
}

} // namespace

int main() {
  try {
    ota_small_signal_is_four_one_poles();
    ota_resonance_thins_bass_and_self_oscillates();
    ota_stays_finite_under_modulation_and_overdrive();
    sem_morph_matches_prototypes();
    high_pass_positions_and_switching();
    drift_is_seeded_smooth_and_bounded();
    ensemble_is_transparent_at_zero_and_wide_at_full();
    delay_echoes_land_on_time_and_decay();
    delay_first_setting_applies_at_once();
    tape_is_a_fixed_delay_until_driven_or_wobbled();
    effects_and_filters_do_not_allocate();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "vintage DSP contract passed\n";
  return 0;
}
