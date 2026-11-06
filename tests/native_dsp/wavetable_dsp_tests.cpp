#include "oscillate/native_dsp/synth/modulation.hpp"
#include "oscillate/native_dsp/synth/vintage_effects.hpp"
#include "oscillate/native_dsp/synth/wavetable.hpp"
#include "oscillate/native_dsp/synth/wavetable_oscillator.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/signal.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
#include <chrono>
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

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using namespace oscillate::native_dsp::synth;
using Complex = std::complex<double>;
constexpr double pi = std::numbers::pi;

using oscillate::test::require;

double db(double gain) { return oscillate::test::gain_to_db(gain, oscillate::test::silence_floor); }

std::size_t table_index(std::string_view name) {
  const auto names = wavetable_names();
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (names[i] == name) {
      return i;
    }
  }
  throw std::runtime_error("missing table");
}

// Blackman-Harris amplitude spectrum, a full-scale sine reading 1.0.
std::vector<double> spectrum(const std::vector<double>& signal) {
  std::vector<Complex> data(signal.size());
  double window_sum = 0.0;
  for (std::size_t i = 0; i < signal.size(); ++i) {
    const double x = 2.0 * pi * static_cast<double>(i) / static_cast<double>(signal.size());
    const double w =
        0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) - 0.01168 * std::cos(3 * x);
    window_sum += w;
    data[i] = signal[i] * w;
  }
  fft(data, false);
  std::vector<double> amplitude(signal.size() / 2);
  for (std::size_t i = 0; i < amplitude.size(); ++i) {
    amplitude[i] = 2.0 * std::abs(data[i]) / window_sum;
  }
  return amplitude;
}

double inharmonic_peak(const std::vector<double>& s, double rate, double fundamental,
                       double high_hz) {
  const double bin_hz = rate / (2.0 * static_cast<double>(s.size()));
  double peak = 0.0;
  for (std::size_t bin = 0; bin < s.size(); ++bin) {
    const double hz = static_cast<double>(bin) * bin_hz;
    if (hz < 20.0 || hz > high_hz) {
      continue;
    }
    const double harmonic = std::round(hz / fundamental);
    if (harmonic >= 1.0 && std::fabs(hz - harmonic * fundamental) <= 6.0 * bin_hz) {
      continue;
    }
    peak = std::max(peak, s[bin]);
  }
  return peak;
}

std::vector<double> render(std::size_t table, double rate, double hz, double position,
                           WarpMode warp, double amount, std::size_t frames) {
  WavetableOscillator osc;
  osc.reset(0.0);
  osc.set_band(wavetables()[table], hz / rate, warp, amount);
  std::vector<double> out(frames);
  for (std::size_t i = 0; i < 2'048; ++i) {
    (void)osc.next(hz / rate, position, warp, amount);
  }
  for (double& sample : out) {
    sample = osc.next(hz / rate, position, warp, amount);
  }
  return out;
}

void fft_round_trips() {
  std::vector<Complex> data(1'024);
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = std::sin(0.3 * static_cast<double>(i)) + 0.5 * std::cos(1.7 * static_cast<double>(i));
  }
  const auto original = data;
  fft(data, false);
  fft(data, true);
  double error = 0.0;
  for (std::size_t i = 0; i < data.size(); ++i) {
    error = std::max(error, std::abs(data[i] - original[i]));
  }
  REQUIRE_LT(error, 1e-12, "FFT round trip is exact");
  std::vector<Complex> tone(256);
  for (std::size_t i = 0; i < tone.size(); ++i) {
    tone[i] = std::cos(2.0 * pi * 5.0 * static_cast<double>(i) / 256.0);
  }
  fft(tone, false);
  REQUIRE_LT(std::fabs(std::abs(tone[5]) - 128.0), tolerance::double_accumulation,
             "a pure cosine lands in one bin");
  REQUIRE_LT(std::abs(tone[6]), tolerance::double_accumulation, "a pure cosine lands in one bin");
}

void every_table_is_band_limited_at_every_level() {
  const auto start = std::chrono::steady_clock::now();
  const auto& tables = wavetables();
  const auto built = std::chrono::steady_clock::now() - start;
  std::cout << "table build "
            << std::chrono::duration_cast<std::chrono::milliseconds>(built).count() << " ms\n";
  for (const auto& table : tables) {
    for (std::size_t level = 0; level < wavetable_levels; ++level) {
      const auto& band = table.levels[level];
      REQUIRE_EQ(band.harmonics, std::max(1U, wavetable_level_harmonics(level)), "level layout");
      REQUIRE_GE(band.length, 4 * band.harmonics, "level layout");
      for (std::size_t frame = 0; frame < wavetable_frames; frame += 5) {
        std::vector<Complex> data(band.length);
        const float* samples = band.frame(frame);
        for (std::size_t i = 0; i < band.length; ++i) {
          data[i] = samples[i];
        }
        REQUIRE_EQ(samples[-1], samples[band.length - 1], "guard samples wrap the cycle");
        REQUIRE_EQ(samples[band.length], samples[0], "guard samples wrap the cycle");
        REQUIRE_EQ(samples[band.length + 1], samples[1], "guard samples wrap the cycle");
        fft(data, false);
        double inside = 0.0;
        double outside = 0.0;
        for (std::size_t bin = 1; bin < band.length / 2; ++bin) {
          (bin <= band.harmonics ? inside : outside) += std::norm(data[bin]);
        }
        REQUIRE_LT(std::abs(data[0]) / band.length, 1e-6, "frames have no DC");
        REQUIRE_LE(outside, 1e-10 * std::max(inside, 1e-30),
                   std::string(table.name) + ": no harmonic above the level's limit");
      }
    }
  }
}

void basic_shapes_are_exact() {
  const auto& band = wavetables()[table_index("Basic Shapes")].levels[0];
  const float* sine = band.frame(0);
  for (std::size_t i = 0; i < band.length; ++i) {
    const double expected = 0.95 * std::sin(2.0 * pi * static_cast<double>(i) / band.length);
    REQUIRE_LT(std::fabs(sine[i] - expected), tolerance::float_round_trip,
               "first frame is a pure sine");
  }
  std::vector<Complex> square(band.length);
  const float* last = band.frame(wavetable_frames - 1);
  for (std::size_t i = 0; i < band.length; ++i) {
    square[i] = last[i];
  }
  fft(square, false);
  for (std::size_t h : {3U, 5U, 7U, 101U}) {
    REQUIRE_LT(std::fabs(std::abs(square[h]) / std::abs(square[1]) - 1.0 / static_cast<double>(h)),
               tolerance::float_processing,
               "last frame is a band-limited square (1/n odd harmonics)");
    REQUIRE_LT(std::abs(square[h + 1]), tolerance::float_round_trip * std::abs(square[1]),
               "square has no even harmonics");
  }
}

void aliasing_stays_low_with_and_without_warps() {
  constexpr double rate = oscillate::test::sample_rate_44k_hz;
  const auto bright = table_index("Pluck Decay");
  const double c8 = 4'186.009;
  const double plain = inharmonic_peak(
      spectrum(render(bright, rate, c8, 0.0, WarpMode::off, 0.0, 65'536)), rate, c8, 18'000.0);
  std::cout << "bright table C8 @44.1k alias floor " << db(plain) << " dBFS\n";
  REQUIRE_LT(db(plain), -90.0, "unwarped aliasing below -90 dBFS");

  const double c6 = 1'046.5;
  for (WarpMode mode : {WarpMode::bend_plus, WarpMode::bend_minus, WarpMode::sync, WarpMode::mirror,
                        WarpMode::asym}) {
    const double alias = inharmonic_peak(spectrum(render(bright, rate, c6, 0.0, mode, 0.8, 65'536)),
                                         rate, c6, 18'000.0);
    std::cout << "warp " << static_cast<int>(mode) << " at 0.8, C6: alias " << db(alias)
              << " dBFS\n";
    REQUIRE_LT(db(alias), -60.0, "warped aliasing below -60 dBFS at C6");
  }
}

void position_sweeps_are_smooth() {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  const auto& table = wavetables()[table_index("Basic Shapes")];
  WavetableOscillator steady;
  steady.reset(0.0);
  steady.set_band(table, 220.0 / rate, WarpMode::off, 0.0);
  double steady_step = 0.0;
  double previous = steady.next(220.0 / rate, 1.0, WarpMode::off, 0.0);
  for (int i = 0; i < 9'600; ++i) {
    const double y = steady.next(220.0 / rate, 1.0, WarpMode::off, 0.0);
    steady_step = std::max(steady_step, std::fabs(y - previous));
    previous = y;
  }
  WavetableOscillator sweep;
  sweep.reset(0.0);
  sweep.set_band(table, 220.0 / rate, WarpMode::off, 0.0);
  previous = sweep.next(220.0 / rate, 0.0, WarpMode::off, 0.0);
  double sweep_step = 0.0;
  for (int i = 0; i < 4'800; ++i) {
    const double y = sweep.next(220.0 / rate, i / 4'800.0, WarpMode::off, 0.0);
    sweep_step = std::max(sweep_step, std::fabs(y - previous));
    previous = y;
  }
  REQUIRE_LE(sweep_step, 1.05 * steady_step, "a 100 ms full morph is no sharper than a square");
}

void pitch_matches_zero_crossing_oracle() {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  const auto out =
      render(table_index("Basic Shapes"), rate, 440.0, 0.0, WarpMode::off, 0.0, 96'000);
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
  const double hz = rate * crossings / (last - first);
  REQUIRE_LT(std::fabs(1200.0 * std::log2(hz / 440.0)), 0.5, "pitch within 0.5 cent");
}

void comb_peaks_and_notches() {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  const auto gain = [&](double feedback, double hz) {
    CombFilter comb;
    comb.prepare(rate);
    comb.set(500.0, feedback, 0);
    comb.reset();
    double in = 0.0;
    double out = 0.0;
    for (int i = 0; i < 96'000; ++i) {
      const double x = 0.05 * std::sin(2.0 * pi * hz * i / rate);
      const double y = comb.process(x);
      if (i > 48'000) {
        in += x * x;
        out += y * y;
      }
    }
    return std::sqrt(out / in);
  };
  REQUIRE_GT(db(gain(0.9, 1'000.0)) - db(gain(0.9, 750.0)), 20.0,
             "positive feedback peaks at multiples of the comb frequency");
  REQUIRE_GT(db(gain(-0.9, 750.0)) - db(gain(-0.9, 1'000.0)), 20.0,
             "negative feedback notches at even multiples");
}

void step_sequences_step_and_glide() {
  const std::array<double, 4> steps{-1.0, 0.5, 1.0, 0.0};
  REQUIRE_EQ(step_value(steps.data(), 4, 0.1, 0.0), -1.0, "hard steps hold their value");
  REQUIRE_EQ(step_value(steps.data(), 4, 0.6, 0.0), 1.0, "hard steps hold their value");
  // At 0.99 the last step (0) is 92% through its glide to the first (-1).
  require(step_value(steps.data(), 4, 0.99, 0.5) < -0.9 &&
              step_value(steps.data(), 4, 0.99, 0.5) > -1.0,
          "the last step glides back to the first");
  REQUIRE_LT(std::fabs(step_value(steps.data(), 4, 0.125 + 0.0625, 0.5) - (-0.25)),
             tolerance::double_rounding, "glide is a smooth curve through the midpoint");
}

void playback_does_not_allocate() {
  const auto& table = wavetables()[0];
  WavetableOscillator osc;
  osc.reset(0.25);
  CombFilter comb;
  comb.prepare(48'000.0);
  comb.set(300.0, 0.7, 32);
  counting_allocations = true;
  counted_allocations = 0;
  double sink = 0.0;
  for (int i = 0; i < 4'096; ++i) {
    if (i % 32 == 0) {
      osc.set_band(table, 0.01 + 0.00001 * i, WarpMode::sync, 0.5);
    }
    sink += comb.process(osc.next(0.01, (i % 1'000) / 1'000.0, WarpMode::sync, 0.5));
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "wavetable playback never allocates");
  require(std::isfinite(sink), "finite output");
}

} // namespace

int main() {
  try {
    fft_round_trips();
    every_table_is_band_limited_at_every_level();
    basic_shapes_are_exact();
    aliasing_stays_low_with_and_without_warps();
    position_sweeps_are_smooth();
    pitch_matches_zero_crossing_oracle();
    comb_peaks_and_notches();
    step_sequences_step_and_glide();
    playback_does_not_allocate();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "wavetable DSP contract passed\n";
  return 0;
}
