#include "effect_test_support.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/signal.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

// Signal (ADR-0042): Off is bit-exact; the sine and the sweep follow their
// analytic phase; levels are exact (peak for tones, RMS for noise); white
// is flat; pink falls 3 dB per octave so octave bands read level; impulses
// land on their period; the five outputs and Add route exactly; switching
// fades without a click.
namespace {
using namespace effect_test;
namespace signal = oscillate::native_dsp::effects::signal;

constexpr double rate = oscillate::test::sample_rate_48k_hz;

const EngineDescriptor& table() { return *signal::engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }

std::vector<double> with(std::initializer_list<std::pair<std::string_view, double>> set) {
  auto values = default_values(table());
  for (const auto& [id, value] : set) {
    values[at(id)] = value;
  }
  return values;
}

Stereo quiet(std::size_t frames) {
  return {std::vector<double>(frames), std::vector<double>(frames)};
}

Stereo run(const std::vector<double>& values, const Stereo& input, double sample_rate = rate) {
  auto runtime = make_effect(signal::engine(), values, sample_rate);
  return process(*runtime, input, {.rate = sample_rate});
}

double as_float(double x) { return static_cast<double>(static_cast<float>(x)); }

double gain(double db_value) { return oscillate::native_dsp::dsp::db_to_gain(db_value); }

double max_error(const std::vector<double>& x, const std::vector<double>& y) {
  double error = 0.0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    error = std::max(error, std::fabs(x[i] - y[i]));
  }
  return error;
}

void off_is_bit_exact() {
  const auto input = noise(0.5, 24'000);
  const auto out = run(with({{"signal.on", 0.0}}), input);
  for (std::size_t i = 0; i < input.size(); ++i) {
    REQUIRE_EQ(out.left[i], as_float(input.left[i]), "Off is bit-exact");
    REQUIRE_EQ(out.right[i], as_float(input.right[i]), "Off is bit-exact");
  }
  // On, then off: exact again once the 10 ms fade has passed.
  auto runtime = make_effect(signal::engine(), default_values(table()));
  (void)process(*runtime, quiet(4'800));
  require(runtime->offer_values(with({{"signal.on", 0.0}}), 1), "off offered");
  const auto later = process(*runtime, input, {}, 4'800);
  const std::size_t fade = signal::engine().tail_frames({}, rate) + 32;
  double during = 0.0;
  for (std::size_t i = 0; i < later.size(); ++i) {
    if (i >= fade) {
      REQUIRE_EQ(later.left[i], as_float(input.left[i]), "off after the fade is bit-exact");
    } else {
      during = std::max(during, std::fabs(later.left[i] - input.left[i]));
    }
  }
  REQUIRE_GT(during, 0.0, "the fade crossed over");
}

void sine_is_exact() {
  for (const double sample_rate : {44'100.0, 48'000.0, 96'000.0}) {
    for (const double hz : {40.0, 997.0, 15'000.0}) {
      const auto out = run(with({{"signal.frequency", hz}, {"signal.level", -6.0}}),
                           quiet(static_cast<std::size_t>(sample_rate)), sample_rate);
      std::vector<double> expected(out.size());
      for (std::size_t i = 0; i < out.size(); ++i) {
        expected[i] = gain(-6.0) *
                      std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / sample_rate);
      }
      require(max_error(out.left, expected) < 1e-6 && out.left == out.right,
              "the sine follows its analytic phase at its exact peak: " + std::to_string(hz) +
                  " Hz at " + std::to_string(sample_rate));
    }
  }
  // A frequency change keeps the phase continuous: no step larger than the
  // tone's own slope allows.
  auto runtime = make_effect(signal::engine(), with({{"signal.level", 0.0}}));
  auto out = process(*runtime, quiet(4'800));
  require(runtime->offer_values(with({{"signal.level", 0.0}, {"signal.frequency", 3'000.0}}), 1),
          "frequency offered");
  const auto after = process(*runtime, quiet(4'800), {}, 4'800);
  out.left.insert(out.left.end(), after.left.begin(), after.left.end());
  double step = 0.0;
  for (std::size_t i = 1; i < out.left.size(); ++i) {
    step = std::max(step, std::fabs(out.left[i] - out.left[i - 1]));
  }
  REQUIRE_LE(step, 2.0 * std::sin(std::numbers::pi * 3'000.0 / rate) + 1e-6,
             "a frequency change is phase-continuous");
}

void the_sweep_follows_the_exponential_law() {
  const double seconds = 4.0;
  const auto frames = static_cast<std::size_t>(seconds * rate);
  auto runtime = make_effect(
      signal::engine(),
      with({{"signal.wave", 1.0}, {"signal.sweep_seconds", seconds}, {"signal.level", 0.0}}));
  const auto out = process(*runtime, quiet(frames + 4'800));
  // Instantaneous frequency from interpolated zero crossings, averaged over
  // ten half-periods, against f(t) = 20 * 1000^(t / T)
  // averaged over the same span.
  std::vector<double> crossings;
  for (std::size_t i = 1; i < frames; ++i) {
    if ((out.left[i - 1] < 0.0) != (out.left[i] < 0.0)) {
      const double fraction = out.left[i - 1] / (out.left[i - 1] - out.left[i]);
      crossings.push_back(static_cast<double>(i - 1) + fraction);
    }
  }
  // The law's mean frequency between two times: its cycles over the span.
  const double l = seconds / std::log(1'000.0);
  const auto cycles = [&](double t) { return signal::sweep_low_hz * l * std::exp(t / l); };
  double worst = 0.0;
  for (std::size_t c = 10; c < crossings.size(); c += 10) {
    const double t0 = crossings[c - 10] / rate;
    const double t1 = crossings[c] / rate;
    const double measured = 5.0 / (t1 - t0);
    const double expected = (cycles(t1) - cycles(t0)) / (t1 - t0);
    if (expected > 5'000.0) {
      break; // Crossings at a few samples per period stop resolving it.
    }
    worst = std::max(worst, std::fabs(measured / expected - 1.0));
  }
  REQUIRE_LT(worst, 0.005,
             "the sweep's frequency follows the law within 0.5%: " + std::to_string(worst * 100.0) +
                 "%");
  REQUIRE_LE(peak(out.left, 0, frames), 1.0 + 1e-6, "the sweep holds its peak level");
  REQUIRE_GT(peak(out.left, 0, frames), 0.999, "the sweep holds its peak level");
  // It repeats from the bottom, phase zero.
  require(std::fabs(out.left[frames]) < tolerance::float_round_trip && out.left[frames + 1] > 0.0,
          "the sweep restarts at zero phase");
  std::vector<float> t;
  REQUIRE_TRUE(runtime->read_telemetry(t), "telemetry");
  REQUIRE_EQ(t.size(), signal::telemetry::size, "telemetry");
  const double since = 4'800.0 / rate;
  REQUIRE_LT(std::fabs(t[signal::telemetry::progress] - (since - 1.0 / rate) / seconds),
             tolerance::near_silence, "progress reports the sweep's position");
}

double band_power(const std::vector<double>& x, double low, double high) {
  // Welch: Hann, 65536 points, half overlap; power summed between edges.
  constexpr std::size_t n = 65'536;
  std::vector<std::complex<double>> window(n);
  double total = 0.0;
  for (std::size_t start = 0; start + n <= x.size(); start += n / 2) {
    for (std::size_t i = 0; i < n; ++i) {
      const double w =
          oscillate::native_dsp::dsp::hann(static_cast<double>(i), static_cast<double>(n));
      window[i] = x[start + i] * w;
    }
    fft(window);
    for (std::size_t k = 1; k < n / 2; ++k) {
      const double hz = static_cast<double>(k) * rate / static_cast<double>(n);
      if (hz >= low && hz < high) {
        total += std::norm(window[k]);
      }
    }
  }
  return total;
}

void white_is_flat_at_its_rms() {
  const auto out = run(with({{"signal.wave", 3.0}, {"signal.level", -20.0}}), quiet(480'000));
  REQUIRE_LT(std::fabs(db(rms(out.left, 0, out.size())) + 20.0), 0.05,
             "white noise reads its RMS level within 0.05 dB");
  const double low = band_power(out.left, 100.0, 1'000.0) / 900.0;
  const double high = band_power(out.left, 10'000.0, 19'000.0) / 9'000.0;
  REQUIRE_LT(std::fabs(10.0 * std::log10(high / low)), 0.25, "white noise is flat");
}

// The cross-spectrum of pink against white from the same seed: the shaping
// filter itself, free of the noise's own scatter.
std::vector<double> octave_shape(const std::vector<double>& pink, const std::vector<double>& white,
                                 double first, double last) {
  constexpr std::size_t n = 65'536;
  std::vector<std::complex<double>> p(n);
  std::vector<std::complex<double>> w(n);
  std::vector<std::complex<double>> cross(n / 2);
  std::vector<double> auto_power(n / 2);
  for (std::size_t start = 0; start + n <= pink.size(); start += n / 2) {
    for (std::size_t i = 0; i < n; ++i) {
      const double hann =
          oscillate::native_dsp::dsp::hann(static_cast<double>(i), static_cast<double>(n));
      p[i] = pink[start + i] * hann;
      w[i] = white[start + i] * hann;
    }
    fft(p);
    fft(w);
    for (std::size_t k = 1; k < n / 2; ++k) {
      cross[k] += p[k] * std::conj(w[k]);
      auto_power[k] += std::norm(w[k]);
    }
  }
  // Each octave's power through the filter, over what exact 1/f gives it.
  std::vector<double> octaves;
  for (double centre = first; centre <= last; centre *= 2.0) {
    double through = 0.0;
    double ideal = 0.0;
    for (std::size_t k = 1; k < n / 2; ++k) {
      const double hz = static_cast<double>(k) * rate / static_cast<double>(n);
      if (hz >= centre / std::numbers::sqrt2 && hz < centre * std::numbers::sqrt2) {
        through += std::norm(cross[k] / auto_power[k]);
        ideal += 1.0 / hz;
      }
    }
    octaves.push_back(10.0 * std::log10(through / ideal));
  }
  double mean = 0.0;
  for (const double level : octaves) {
    mean += level / static_cast<double>(octaves.size());
  }
  for (double& level : octaves) {
    level -= mean;
  }
  return octaves;
}

void pink_reads_level_in_every_octave() {
  const auto pink = run(with({{"signal.wave", 2.0}, {"signal.level", -20.0}}), quiet(30 * 48'000));
  REQUIRE_LT(std::fabs(db(rms(pink.left, 0, pink.size())) + 20.0), 0.1,
             "pink noise reads its RMS level within 0.1 dB: " +
                 std::to_string(db(rms(pink.left, 0, pink.size()))));
  const auto white = run(with({{"signal.wave", 3.0}, {"signal.level", -20.0}}), quiet(pink.size()));
  const auto octaves = octave_shape(pink.left, white.left, 31.25, 16'000.0);
  for (std::size_t o = 0; o < octaves.size(); ++o) {
    REQUIRE_LT(std::fabs(octaves[o]), 0.1,
               "pink falls 3 dB an octave within 0.1 dB, 31.5 Hz to 16 kHz: octave " +
                   std::to_string(o) + " " + std::to_string(octaves[o]));
  }
  // So octave bands of the noise itself read level, within its scatter.
  for (double centre = 125.0; centre <= 16'000.0; centre *= 2.0) {
    const double ratio =
        band_power(pink.left, centre / std::numbers::sqrt2, centre * std::numbers::sqrt2) /
        band_power(pink.left, 1'000.0 / std::numbers::sqrt2, 1'000.0 * std::numbers::sqrt2);
    REQUIRE_LT(std::fabs(10.0 * std::log10(ratio)), 0.3, "pink octave bands read level");
  }
  // Repeatable: seeded.
  const auto again = run(with({{"signal.wave", 2.0}, {"signal.level", -20.0}}), quiet(48'000));
  require(std::equal(again.left.begin(), again.left.end(), pink.left.begin()),
          "noise is seeded, so renders repeat");
}

void impulses_land_on_their_period() {
  const auto out =
      run(with({{"signal.wave", 4.0}, {"signal.impulse_rate", 4.0}, {"signal.level", -3.0}}),
          quiet(48'000));
  for (std::size_t i = 0; i < out.size(); ++i) {
    const double expected = i % 12'000 == 0 ? as_float(gain(-3.0)) : 0.0;
    REQUIRE_EQ(out.left[i], expected, "impulses are single samples at the level, every period");
    REQUIRE_EQ(out.right[i], expected, "impulses are single samples at the level, every period");
  }
}

void outputs_route_exactly() {
  const auto input = noise(0.25, 2 * 48'000 + 4'800);
  // Not a whole number of cycles a second, so the tone is not at a zero
  // crossing on the routing switch at frame 48,000.
  constexpr double off_grid_hz = 440.25;
  const auto base = with({{"signal.frequency", off_grid_hz}, {"signal.level", -12.0}});
  const auto tone = run(base, quiet(input.size()));
  const auto route = [&](double output, double mode) {
    auto values = base;
    values[at("signal.output")] = output;
    values[at("signal.mode")] = mode;
    return run(values, input);
  };
  const auto left = route(1.0, 0.0);
  REQUIRE_EQ(left.left, tone.left, "Left: the tone on the left, silence on the right");
  REQUIRE_EQ(max_error(left.right, std::vector<double>(input.size())), 0.0,
             "Left: the tone on the left, silence on the right");
  const auto right = route(2.0, 0.0);
  REQUIRE_EQ(right.right, tone.right, "Right: the tone on the right, silence on the left");
  REQUIRE_EQ(max_error(right.left, std::vector<double>(input.size())), 0.0,
             "Right: the tone on the right, silence on the left");
  const auto inverted = route(4.0, 0.0);
  for (std::size_t i = 0; i < input.size(); ++i) {
    REQUIRE_EQ(inverted.right[i], -inverted.left[i], "Right Inverted is the exact negative");
  }
  const auto alternate = route(3.0, 0.0);
  for (const std::size_t i : {std::size_t{1'000}, std::size_t{47'999}}) {
    REQUIRE_NE(alternate.left[i], 0.0, "the first second is left");
    REQUIRE_EQ(alternate.right[i], 0.0, "the first second is left");
  }
  for (const std::size_t i : {std::size_t{48'000}, std::size_t{95'000}}) {
    REQUIRE_EQ(alternate.left[i], 0.0, "the next second is right");
    REQUIRE_NE(alternate.right[i], 0.0, "the next second is right");
  }
  REQUIRE_NE(alternate.left[97'000], 0.0, "and back left");
  REQUIRE_EQ(alternate.right[97'000], 0.0, "and back left");
  const auto added = route(0.0, 1.0);
  std::vector<double> sum(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    sum[i] = as_float(as_float(input.left[i]) + tone.left[i]);
  }
  REQUIRE_LT(max_error(added.left, sum), 1e-6, "Add mixes the tone onto the input");
}

void switching_glides() {
  auto runtime =
      make_effect(signal::engine(),
                  with({{"signal.on", 0.0}, {"signal.level", 0.0}, {"signal.frequency", 50.0}}));
  (void)process(*runtime, quiet(4'800));
  require(runtime->offer_values(
              with({{"signal.on", 1.0}, {"signal.level", 0.0}, {"signal.frequency", 50.0}}), 1),
          "on offered");
  const auto out = process(*runtime, quiet(4'800), {}, 4'800);
  double step = 0.0;
  for (std::size_t i = 1; i < out.size(); ++i) {
    step = std::max(step, std::fabs(out.left[i] - out.left[i - 1]));
  }
  // A 50 Hz tone moves 0.0065 a sample; the fade adds at most 1/480.
  REQUIRE_LT(step, 0.0065 + 1.0 / 480.0 + 1e-6, "switching on fades in without a click");
  // Level moves glide too.
  auto level = make_effect(signal::engine(), with({{"signal.wave", 3.0}, {"signal.level", -40.0}}));
  (void)process(*level, quiet(4'800));
  require(level->offer_values(with({{"signal.wave", 3.0}, {"signal.level", 0.0}}), 1),
          "level offered");
  const auto rising = process(*level, quiet(4'800), {}, 4'800);
  REQUIRE_LT(rms(rising.left, 0, 32), gain(-20.0), "a level jump glides rather than steps");
}

} // namespace

int main() {
  try {
    off_is_bit_exact();
    sine_is_exact();
    the_sweep_follows_the_exponential_law();
    white_is_flat_at_its_rms();
    pink_reads_level_in_every_octave();
    impulses_land_on_their_period();
    outputs_route_exactly();
    switching_glides();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "native signal contract passed\n";
  return 0;
}
