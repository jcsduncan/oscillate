#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/fm.hpp"
#include "oscillate/native_dsp/synth/wavetable.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/signal.hpp"

#include <algorithm>
#include <bit>
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
constexpr double pi = std::numbers::pi;

using oscillate::test::require;

double db(double gain) { return oscillate::test::gain_to_db(gain, oscillate::test::silence_floor); }

void waveforms_are_exact() {
  const auto& tables = FmWaveTables::instance();
  for (int i = 0; i < 1'000; ++i) {
    const double phase = i / 1'000.0;
    const double s = std::sin(2.0 * pi * phase);
    REQUIRE_LT(std::fabs(tables.read(FmWave::sine, phase) - s), 2e-6, "sine table");
    REQUIRE_LT(std::fabs(tables.read(FmWave::abs_sine, phase) - std::fabs(s)), 2e-6, "abs sine");
    REQUIRE_LT(std::fabs(tables.read(FmWave::half_sine, phase) - (phase < 0.5 ? s : 0.0)), 2e-3,
               "half sine");
  }
}

void algorithms_are_well_formed() {
  const auto& table = fm_algorithm_table();
  constexpr std::array<int, 32> carrier_counts{2, 2, 2, 2, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1,
                                               1, 1, 3, 3, 4, 4, 4, 5, 5, 3, 3, 3, 4, 4, 5, 6};
  for (std::size_t i = 0; i < table.size(); ++i) {
    const auto& algorithm = table[i];
    REQUIRE_EQ(std::popcount(algorithm.carriers), carrier_counts[i],
               "algorithm carrier count matches the chart");
    REQUIRE_LT(algorithm.feedback, fm_operators, "feedback operator exists");
    for (std::size_t op = 0; op < fm_operators; ++op) {
      // Targets are always lower-numbered: evaluation from 6 down works.
      REQUIRE_EQ((algorithm.modulates[op] >> op), 0, "modulators feed lower operators");
      const bool is_carrier = (algorithm.carriers >> op) & 1U;
      require(is_carrier || algorithm.modulates[op] != 0,
              "every operator is heard or modulates something");
    }
  }
  REQUIRE_EQ(table[31].carriers, 0x3F, "algorithm 32 is six parallel carriers");
  // Only algorithms 4 and 6 close a loop: op 4 and op 5 feed back into op 6,
  // which reaches them through its chain.
  for (std::size_t i = 0; i < table.size(); ++i) {
    REQUIRE_EQ(table[i].loop(), (i == 3 || i == 5), "only algorithms 4 and 6 loop");
  }
  REQUIRE_EQ(table[3].feedback, 5, "algorithm 4 loops op 4 to op 6");
  REQUIRE_EQ(table[3].feedback_from, 3, "algorithm 4 loops op 4 to op 6");
  REQUIRE_EQ(table[5].feedback, 5, "algorithm 6 loops op 5 to op 6");
  REQUIRE_EQ(table[5].feedback_from, 4, "algorithm 6 loops op 5 to op 6");
  for (const std::size_t i : {std::size_t{3}, std::size_t{5}}) {
    // Follow the chain down from the loop's destination to its source.
    std::uint8_t reached = 1U << table[i].feedback;
    for (std::size_t op = fm_operators; op-- > 0;) {
      if (((reached >> op) & 1U) != 0U) {
        reached |= table[i].modulates[op];
      }
    }
    REQUIRE_NE(((static_cast<unsigned>(reached) >> table[i].feedback_from) & 1U), 0U,
               "the loop closes through its chain");
  }
}

// A sine carrier phase-modulated by a sine of index beta has sideband n at
// |J_n(beta)|: the independent closed-form FM oracle.
void sidebands_follow_bessel_functions() {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  constexpr double carrier_hz = 3'000.0;
  constexpr double modulator_hz = 750.0; // 1:4 ratio, clean sideband spacing.
  for (double beta : {0.5, 1.5, 3.0}) {
    FmOperator carrier;
    FmOperator modulator;
    carrier.reset();
    modulator.reset();
    const std::size_t n = 65'536;
    std::vector<std::complex<double>> data(n);
    for (std::size_t i = 0; i < n; ++i) {
      const double m = modulator.next(modulator_hz / rate, 0.0, 0.0, FmWave::sine);
      // Modulation is in cycles: beta radians is beta / 2 pi cycles.
      const double y = carrier.next(carrier_hz / rate, beta / (2.0 * pi) * m, 0.0, FmWave::sine);
      const double hann = oscillate::native_dsp::dsp::hann(static_cast<double>(i), n);
      data[i] = y * hann;
    }
    fft(data, false);
    const double bin_hz = rate / static_cast<double>(n);
    const auto amplitude = [&](double hz) {
      const auto centre = static_cast<std::size_t>(std::round(hz / bin_hz));
      double peak = 0.0;
      for (std::size_t bin = centre - 2; bin <= centre + 2; ++bin) {
        peak = std::max(peak, std::abs(data[bin]));
      }
      return peak / (static_cast<double>(n) / 4.0); // Hann coherent gain 1/2.
    };
    for (int order = 0; order <= 3; ++order) {
      const double expected = std::fabs(std::cyl_bessel_j(static_cast<double>(order), beta));
      if (expected < 0.05) {
        continue;
      }
      const double measured = amplitude(carrier_hz + order * modulator_hz);
      REQUIRE_LT(std::fabs(db(measured / expected)), 0.2, "sideband matches J_n(beta)");
    }
  }
}

void pitch_and_feedback() {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  FmOperator op;
  op.reset();
  std::vector<double> out(96'000);
  for (double& sample : out) {
    sample = op.next(440.0 / rate, 0.0, 0.0, FmWave::sine);
  }
  double first = -1.0;
  double last = -1.0;
  int crossings = 0;
  for (std::size_t i = 1; i < out.size(); ++i) {
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
  REQUIRE_LT(std::fabs(1200.0 * std::log2(rate * crossings / (last - first) / 440.0)), 0.5,
             "operator pitch within 0.5 cent");
  for (double feedback : {0.0, 0.25, 0.5, 1.0}) {
    FmOperator fb;
    fb.reset();
    double peak = 0.0;
    for (int i = 0; i < 48'000; ++i) {
      const double y = fb.next(220.0 / rate, 0.0, feedback, FmWave::sine);
      require(std::isfinite(y), "feedback stays finite");
      peak = std::max(peak, std::fabs(y));
    }
    REQUIRE_LE(peak, 1.0 + 1e-6, "feedback stays bounded");
  }
}

// Switching waveform mid-cycle crossfades at the same phase: no sample step
// is larger than the steady slope of the two waves.
void wave_switches_crossfade() {
  FmOperator op;
  op.reset();
  constexpr double increment = 220.0 / 96'000.0;
  double previous = 0.0;
  double largest = 0.0;
  for (int i = 0; i < 4'000; ++i) {
    const FmWave wave = i < 1'111 ? FmWave::sine : FmWave::abs_sine;
    const double y = op.next(increment, 0.0, 0.0, wave);
    if (i > 0) {
      largest = std::max(largest, std::fabs(y - previous));
    }
    previous = y;
  }
  // Sine and abs-sine both move at most 2 pi * increment per frame.
  REQUIRE_LT(largest, 2.0 * pi * increment * 1.5, "a wave switch does not step");
}

void operators_do_not_allocate() {
  std::array<FmOperator, 6> ops;
  counting_allocations = true;
  counted_allocations = 0;
  double sink = 0.0;
  for (int i = 0; i < 4'096; ++i) {
    double modulation = 0.0;
    for (std::size_t op = 6; op-- > 0;) {
      modulation =
          2.0 * ops[op].next(0.01 * static_cast<double>(op + 1), modulation, 0.3, FmWave::sine);
    }
    sink += modulation;
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "FM operators never allocate");
  require(std::isfinite(sink), "finite output");
}

} // namespace

int main() {
  try {
    waveforms_are_exact();
    algorithms_are_well_formed();
    sidebands_follow_bessel_functions();
    pitch_and_feedback();
    wave_switches_crossfade();
    operators_do_not_allocate();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "FM DSP contract passed\n";
  return 0;
}
