#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/drum_dsp.hpp"
#include "oscillate/native_dsp/synth/wavetable.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/signal.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
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

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using namespace oscillate::native_dsp::synth;
constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr double pi = std::numbers::pi;

using oscillate::test::require;

double db(double gain) { return oscillate::test::gain_to_db(gain, oscillate::test::silence_floor); }

void decays_reach_minus_60_db_on_time() {
  for (double seconds : {0.05, 0.4, 2.5}) {
    const double c = decay_coefficient(seconds, rate);
    const auto frames = static_cast<std::size_t>(std::round(seconds * rate));
    double level = 1.0;
    for (std::size_t i = 0; i < frames; ++i) {
      level *= c;
    }
    REQUIRE_LT(std::fabs(db(level) + 60.0), 0.05, "decay reaches -60 dB at its time");
  }
}

void body_pitch_follows_the_analytic_sweep() {
  ResonantBody body;
  body.prepare(rate);
  body.trigger(400.0, 50.0, 0.03);
  std::vector<double> out(48'000);
  for (std::size_t i = 0; i < out.size(); ++i) {
    const double t = static_cast<double>(i) / rate;
    const double expected = 50.0 + 350.0 * std::exp(-t / 0.03);
    REQUIRE_LT(std::fabs(body.frequency() / expected - 1.0), 0.01, "sweep follows its curve");
    out[i] = body.next();
  }
  // Late in the hit the body sits at the end pitch: zero-crossing oracle.
  double first = -1.0;
  double last = -1.0;
  int crossings = 0;
  for (std::size_t i = 24'001; i < out.size(); ++i) {
    if (out[i - 1] < 0.0 && out[i] >= 0.0) {
      const double t = static_cast<double>(i - 1) + out[i - 1] / (out[i - 1] - out[i]);
      if (first < 0.0) {
        first = t;
      } else {
        ++crossings;
      }
      last = t;
    }
  }
  REQUIRE_LT(std::fabs(rate * crossings / (last - first) / 50.0 - 1.0), 0.01,
             "the body settles on its end pitch within 1%");
}

void metal_source_has_the_six_frequencies() {
  MetalSource metal;
  metal.reset();
  std::vector<std::complex<double>> data(65'536);
  for (std::size_t i = 0; i < data.size(); ++i) {
    // Hann window: an unwindowed transform leaks the 205 Hz peak downward.
    const double hann =
        oscillate::native_dsp::dsp::hann(static_cast<double>(i), static_cast<double>(data.size()));
    data[i] = metal.next(1.0, rate) * hann;
  }
  fft(data, false);
  const double bin_hz = rate / static_cast<double>(data.size());
  const auto magnitude = [&](double hz) {
    double peak = 0.0;
    const auto centre = static_cast<std::size_t>(std::round(hz / bin_hz));
    for (std::size_t bin = centre - 2; bin <= centre + 2; ++bin) {
      peak = std::max(peak, std::abs(data[bin]));
    }
    return peak;
  };
  const double reference = magnitude(metal_frequencies[0]);
  for (double hz : metal_frequencies) {
    REQUIRE_LT(std::fabs(db(magnitude(hz) / reference)), 1.0, "each oscillator is present equally");
  }
  REQUIRE_LT(db(magnitude(100.0) / reference), -60.0, "nothing below the lowest oscillator");
}

void clap_bursts_then_tail() {
  BurstEnvelope clap;
  clap.prepare(rate);
  clap.trigger(0.011, 0.004, 0.2);
  std::vector<double> envelope(24'000);
  for (double& value : envelope) {
    value = clap.next();
  }
  const auto spacing = static_cast<std::size_t>(0.011 * rate);
  const auto attack = static_cast<std::size_t>(BurstEnvelope::attack_seconds * rate);
  for (std::size_t burst = 0; burst < BurstEnvelope::max_bursts; ++burst) {
    const double at = envelope[burst * spacing + attack - 1];
    require(std::fabs(at - (1.0 - 0.12 * static_cast<double>(burst))) < 0.01 ||
                burst + 1 == BurstEnvelope::max_bursts,
            "each burst reaches its level after its attack, the first the strongest");
    double step = 0.0;
    for (std::size_t i = burst * spacing; i < burst * spacing + attack; ++i)
      step = std::max(step, envelope[i] - (i == 0 ? 0.0 : envelope[i - 1]));
    REQUIRE_LT(step, 1.0 / static_cast<double>(attack) + 0.05, "each burst rises without a step");
    require(envelope[burst * spacing + spacing / 2] < BurstEnvelope::tail_peak + 0.01 ||
                burst + 1 == BurstEnvelope::max_bursts,
            "bursts fall to the tail between hits");
  }
  REQUIRE_GT(envelope[attack + 48], 0.3, "the tail starts with the first burst");
  // 94 ms in, a 0.2 s tail from the first burst is about 28 dB down.
  REQUIRE_GT(envelope[4 * spacing + 2'400], 0.01, "a tail follows the last burst");
  REQUIRE_LT(envelope.back(), 0.01, "the tail decays");
}

void glue_compressor_follows_its_ratio() {
  const auto output_db = [](double amount, double input_db) {
    GlueCompressor glue;
    glue.prepare(rate);
    glue.set(amount, 0.2);
    const double amplitude = oscillate::native_dsp::dsp::db_to_gain(input_db);
    double peak = 0.0;
    for (int i = 0; i < 96'000; ++i) {
      const auto out = glue.process(amplitude * std::sin(2.0 * pi * 100.0 * i / rate), 0.0);
      if (i > 72'000) {
        peak = std::max(peak, std::fabs(out.left));
      }
    }
    return db(peak);
  };
  REQUIRE_LT(std::fabs(output_db(0.0, -12.0) + 12.0), 0.01, "amount 0 is transparent");
  // Well above a -24 dB threshold at ratio 4, 12 dB more in is 3 dB more out.
  const double change = output_db(1.0, -3.0) - output_db(1.0, -15.0);
  REQUIRE_LT(std::fabs(change - 3.0), 0.6, "full amount compresses at about 4:1");
}

void processing_does_not_allocate() {
  ResonantBody body;
  body.prepare(rate);
  MetalSource metal;
  metal.reset();
  BurstEnvelope clap;
  clap.prepare(rate);
  GlueCompressor glue;
  glue.prepare(rate);
  glue.set(0.6, 0.15);
  counting_allocations = true;
  counted_allocations = 0;
  body.trigger(300.0, 60.0, 0.02);
  clap.trigger(0.01, 0.004, 0.2);
  double sink = 0.0;
  for (int i = 0; i < 4'096; ++i) {
    const double x = body.next() + 0.3 * metal.next(1.0, rate) * clap.next();
    const auto glued = glue.process(x, x);
    sink += glued.right;
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "drum primitives never allocate while processing");
  require(std::isfinite(sink), "finite output");
}

} // namespace

int main() {
  try {
    decays_reach_minus_60_db_on_time();
    body_pitch_follows_the_analytic_sweep();
    metal_source_has_the_six_frequencies();
    clap_bursts_then_tail();
    glue_compressor_follows_its_ratio();
    processing_does_not_allocate();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "drum DSP contract passed\n";
  return 0;
}
