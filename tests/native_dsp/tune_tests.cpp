#include "pocket_support.hpp"

#include "oscillate/native_dsp/effects/tune.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

// Tune (ADR-0042): audio untouched; steady sines read within 0.5 cent from
// 30 Hz to 4 kHz; harmonic-rich tones with a weak fundamental read their
// note, not an octave off; the reference moves the note and cents; a
// synthetic kick glide is traced; and the Pulse drum synth's kick reads
// exactly an octave up when its tune is raised 12 semitones.
namespace {
using namespace pocket_test;
namespace tune = oscillate::native_dsp::effects::tune;

const EngineDescriptor& table() { return *tune::engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }

std::vector<float> run(const std::vector<double>& values, const Stereo& input,
                       double sample_rate = rate) {
  auto runtime = make_effect(tune::engine(), values, sample_rate);
  (void)process(*runtime, input, {.rate = sample_rate});
  std::vector<float> t;
  REQUIRE_TRUE(runtime->read_telemetry(t), "telemetry");
  REQUIRE_EQ(t.size(), tune::telemetry::size, "telemetry");
  return t;
}

double cents_error(double measured, double truth) { return 1'200.0 * std::log2(measured / truth); }

void audio_passes_untouched() {
  const auto input = sine(110.0, 0.5, second);
  auto runtime = make_effect(tune::engine(), preset(table(), "Instrument"));
  const auto out = process(*runtime, input);
  for (std::size_t i = 0; i < input.size(); ++i) {
    REQUIRE_EQ(out.left[i], static_cast<double>(static_cast<float>(input.left[i])),
               "Tune never touches the audio");
  }
}

void steady_tones_read_within_half_a_cent() {
  double worst = 0.0;
  for (const double sample_rate : {44'100.0, 48'000.0, 96'000.0}) {
    for (const auto& [mode, tones] : std::vector<std::pair<std::string_view, std::vector<double>>>{
             {"Bass", {30.0, 41.2, 55.0, 98.0, 220.0}},
             {"Instrument", {82.4, 110.0, 440.0, 1'000.0, 2'637.0, 4'000.0}}}) {
      for (const double hz : tones) {
        const auto t =
            run(preset(table(), mode),
                sine(hz, 0.3, static_cast<std::size_t>(sample_rate), sample_rate), sample_rate);
        const double error = cents_error(t[tune::telemetry::hz], hz);
        worst = std::max(worst, std::fabs(error));
        REQUIRE_LT(std::fabs(error), 0.5,
                   std::string(mode) + " " + std::to_string(hz) + " Hz at " +
                       std::to_string(sample_rate) + " reads " + std::to_string(error) +
                       " cents off");
      }
    }
  }
  std::cout << "  steady sines within " << worst << " cents, 30 Hz-4 kHz, three rates\n";
}

void harmonic_tones_read_their_note() {
  // A saw-like tone with its fundamental 12 dB under the second harmonic.
  for (const double f0 : {41.2, 65.4, 110.0}) {
    Stereo tone{std::vector<double>(second), std::vector<double>(second)};
    for (std::size_t i = 0; i < second; ++i) {
      double x = 0.0;
      for (int h = 1; h <= 12; ++h) {
        const double gain = (h == 1 ? 0.25 : 1.0) / h;
        x += gain * std::sin(2.0 * std::numbers::pi * f0 * h * static_cast<double>(i) / rate);
      }
      tone.left[i] = tone.right[i] = 0.2 * x;
    }
    const auto t = run(preset(table(), "Bass"), tone);
    const double error = cents_error(t[tune::telemetry::hz], f0);
    REQUIRE_LT(std::fabs(error), 1.0,
               "a " + std::to_string(f0) + " Hz tone with a weak fundamental reads " +
                   std::to_string(error) + " cents off, no octave error");
  }
}

void the_reference_moves_the_note() {
  auto values = preset(table(), "A = 432 Hz");
  const auto t = run(values, sine(432.0, 0.3, second));
  REQUIRE_LT(std::fabs(t[tune::telemetry::note] - 69.0F), 0.005F, "432 Hz is A4 against A = 432");
  REQUIRE_LT(std::fabs(t[tune::telemetry::cents]), 0.5F, "432 Hz is A4 against A = 432");
  values[at("tune.reference")] = 440.0;
  const auto u = run(values, sine(432.0, 0.3, second));
  const double expected = 1'200.0 * std::log2(432.0 / 440.0);
  REQUIRE_LT(std::fabs(u[tune::telemetry::cents] - expected), 0.5F,
             "and 31.8 cents flat against A = 440: " + std::to_string(u[tune::telemetry::cents]));
}

void a_kick_glide_is_traced() {
  // f(t) = 50 + 100 exp(-t / 30 ms), phase-continuous, decaying.
  Stereo kick{std::vector<double>(second), std::vector<double>(second)};
  const std::size_t onset = 12'000;
  double phase = 0.0;
  for (std::size_t i = onset; i < second; ++i) {
    const double t = static_cast<double>(i - onset) / rate;
    const double hz = 50.0 + 100.0 * std::exp(-t / 0.03);
    phase += hz / rate;
    kick.left[i] = kick.right[i] =
        0.8 * std::exp(-t / 0.25) * std::sin(2.0 * std::numbers::pi * phase);
  }
  const auto t = run(preset(table(), "Kick"), kick);
  REQUIRE_EQ(t[tune::telemetry::hits], 1.0F, "one hit is found");
  for (const double ms : {30.0, 60.0, 120.0, 250.0}) {
    const auto k = static_cast<std::size_t>(ms / 5.0);
    const double expected = 50.0 + 100.0 * std::exp(-ms / 30.0);
    const double measured = t[tune::telemetry::trajectory + k];
    REQUIRE_LT(std::fabs(measured / expected - 1.0), 0.06,
               "the glide at " + std::to_string(ms) + " ms reads " + std::to_string(measured) +
                   " Hz, expected " + std::to_string(expected));
  }
  REQUIRE_LT(std::fabs(cents_error(t[tune::telemetry::settled_hz], 50.0)), 15.0,
             "and settles on 50 Hz: " + std::to_string(t[tune::telemetry::settled_hz]));
}

void pulse_kicks_tune_by_octaves() {
  double settled_0 = 0.0;
  for (const double tune_st : {0.0, 12.0}) {
    const auto& engine = oscillate::native_dsp::synth::pulse::engine();
    auto values = preset(*engine.descriptor, "909 House");
    values[index(*engine.descriptor, "pulse.kick.tune")] += tune_st;
    auto runtime = make_runtime(engine, values);
    const auto hit = note(12'000, 2'400, 36, 1, 110);
    const auto rendered = render(*runtime, {hit.begin(), hit.end()}, second);
    const auto t = run(preset(table(), "Kick"), Stereo{rendered.left, rendered.right});
    const double settled = t[tune::telemetry::settled_hz];
    REQUIRE_GT(settled, 20.0, "the Pulse kick settles on a pitch");
    if (tune_st == 0.0) {
      settled_0 = settled;
    } else {
      const double interval = cents_error(settled, settled_0);
      std::cout << "  909 kick settles at " << settled_0 << " Hz; +12 st at " << settled << " Hz ("
                << interval << " cents)\n";
      // Pulse's body ends exactly at 50 x 2^(tune / 12) Hz; 909 House layers
      // a second body (55 Hz) under it, and the two beat, so the settled
      // pitch of the pair wanders by a few cents.
      REQUIRE_LT(std::fabs(interval - 1'200.0), 10.0,
                 "raising the kick 12 semitones raises it an octave");
    }
  }
}

} // namespace

int main() {
  try {
    audio_passes_untouched();
    steady_tones_read_within_half_a_cent();
    harmonic_tones_read_their_note();
    the_reference_moves_the_note();
    a_kick_glide_is_traced();
    pulse_kicks_tune_by_octaves();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "native tune contract passed\n";
  return 0;
}
