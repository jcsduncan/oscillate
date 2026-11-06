#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/chorus.hpp"
#include "oscillate/native_dsp/synth/filter.hpp"
#include "oscillate/native_dsp/synth/modulation.hpp"
#include "oscillate/native_dsp/synth/oscillator.hpp"
#include "oscillate/native_dsp/synth/oversampler.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/signal.hpp"
#include "oscillate/test/tolerances.hpp"

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

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using namespace oscillate::native_dsp::synth;
constexpr double pi = std::numbers::pi;

using oscillate::test::require;

double db(double gain) { return oscillate::test::gain_to_db(gain, oscillate::test::silence_floor); }

// ---- Independent spectral oracle --------------------------------------------

void fft(std::vector<std::complex<double>>& data) {
  const std::size_t n = data.size();
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1U;
    for (; (j & bit) != 0; bit >>= 1U) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      std::swap(data[i], data[j]);
    }
  }
  for (std::size_t length = 2; length <= n; length <<= 1U) {
    const double angle = -2.0 * pi / static_cast<double>(length);
    const std::complex<double> root(std::cos(angle), std::sin(angle));
    for (std::size_t start = 0; start < n; start += length) {
      std::complex<double> w(1.0, 0.0);
      for (std::size_t k = 0; k < length / 2; ++k) {
        const auto even = data[start + k];
        const auto odd = data[start + k + length / 2] * w;
        data[start + k] = even + odd;
        data[start + k + length / 2] = even - odd;
        w *= root;
      }
    }
  }
}

// Amplitude spectrum with a 4-term Blackman-Harris window (-92 dB sidelobes),
// scaled so a full-scale sine reads 1.0 at its peak bin.
std::vector<double> amplitude_spectrum(const std::vector<double>& signal) {
  const std::size_t n = signal.size();
  std::vector<std::complex<double>> data(n);
  double window_sum = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double x = 2.0 * pi * static_cast<double>(i) / static_cast<double>(n);
    const double w =
        0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) - 0.01168 * std::cos(3 * x);
    window_sum += w;
    data[i] = signal[i] * w;
  }
  fft(data);
  std::vector<double> amplitude(n / 2);
  for (std::size_t i = 0; i < n / 2; ++i) {
    amplitude[i] = 2.0 * std::abs(data[i]) / window_sum;
  }
  return amplitude;
}

// Largest component in [low_hz, high_hz] that is not within `guard` bins of a
// harmonic of the fundamental: the aliasing (and noise) floor.
double inharmonic_peak(const std::vector<double>& spectrum, double rate, double fundamental,
                       double low_hz, double high_hz) {
  const double bin_hz = rate / (2.0 * static_cast<double>(spectrum.size()));
  constexpr double guard = 6.0;
  double peak = 0.0;
  for (std::size_t bin = 0; bin < spectrum.size(); ++bin) {
    const double hz = static_cast<double>(bin) * bin_hz;
    if (hz < low_hz || hz > high_hz) {
      continue;
    }
    const double harmonic = std::round(hz / fundamental);
    if (harmonic >= 1.0 && std::fabs(hz - harmonic * fundamental) <= guard * bin_hz) {
      continue;
    }
    peak = std::max(peak, spectrum[bin]);
  }
  return peak;
}

std::vector<double> render_oscillator(Waveform waveform, double rate, double hz, double width,
                                      std::size_t frames) {
  BlepOscillator osc;
  osc.reset(0.0, waveform);
  std::vector<double> out(frames);
  for (std::size_t i = 0; i < 4096; ++i) {
    (void)osc.next(hz / rate, width, {}, nullptr);
  }
  for (double& sample : out) {
    sample = osc.next(hz / rate, width, {}, nullptr);
  }
  return out;
}

// ---- Oscillator ---------------------------------------------------------------

void blep_tables_are_band_limited_residuals() {
  const auto& tables = BlepTables::instance();
  REQUIRE_LT(std::fabs(tables.step(0.0) + 0.5), tolerance::float_round_trip,
             "step residual is -1/2 at the edge");
  for (double t = 0.05; t < blep_half_width; t += 0.37) {
    REQUIRE_LT(std::fabs(tables.step(t) + tables.step(-t)), tolerance::float_round_trip,
               "step residual is odd");
    REQUIRE_LT(std::fabs(tables.ramp(t) - tables.ramp(-t)), tolerance::float_round_trip,
               "ramp residual is even");
  }
  REQUIRE_LT(std::fabs(tables.step(blep_half_width - 0.01)), tolerance::near_silence,
             "step residual decays");
  REQUIRE_LT(std::fabs(tables.ramp(blep_half_width - 0.01)), tolerance::near_silence,
             "ramp residual decays");
  REQUIRE_EQ(tables.step(blep_half_width + 1.0), 0.0, "step residual is zero outside");
}

void step_lands_at_the_reported_latency() {
  // Starting at phase 0.5 with 1/100 cycle per frame, the saw wraps exactly
  // on the 50th advance (frame index 49). A band-limited step is zero exactly
  // at its edge.
  BlepOscillator osc;
  osc.reset(0.5, Waveform::saw);
  std::vector<double> out(100);
  for (double& sample : out) {
    sample = osc.next(0.01, 0.5, {}, nullptr);
  }
  const std::size_t edge = 49 + BlepOscillator::latency_frames;
  REQUIRE_LT(std::fabs(out[edge]), 0.02, "band-limited edge midpoint lands at the latency");
  REQUIRE_GT(out[edge - 2], 0.8, "saw falls across the edge");
  REQUIRE_LT(out[edge + 2], -0.8, "saw falls across the edge");
}

void saw_aliasing_stays_below_minus_80_db_at_c8() {
  constexpr double rate = oscillate::test::sample_rate_44k_hz;
  constexpr double c8 = 4186.009;
  const auto spectrum = amplitude_spectrum(render_oscillator(Waveform::saw, rate, c8, 0.5, 65'536));
  const double alias = inharmonic_peak(spectrum, rate, c8, 20.0, 18'000.0);
  std::cout << "saw C8 @44.1k alias floor " << db(alias) << " dBFS\n";
  REQUIRE_LT(db(alias), -80.0, "saw C8 aliasing below -80 dBFS in 20 Hz-18 kHz");

  // The oracle must be able to see aliasing: a naive saw fails badly.
  std::vector<double> naive(65'536);
  double phase = 0.0;
  for (double& sample : naive) {
    sample = 2.0 * phase - 1.0;
    phase += c8 / rate;
    phase -= std::floor(phase);
  }
  REQUIRE_GT(db(inharmonic_peak(amplitude_spectrum(naive), rate, c8, 20.0, 18'000.0)), -40.0,
             "naive saw aliasing is visible to the oracle");
}

void pulse_and_triangle_aliasing_stay_low() {
  constexpr double rate = oscillate::test::sample_rate_44k_hz;
  constexpr double hz = 3'520.0;
  const double pulse =
      inharmonic_peak(amplitude_spectrum(render_oscillator(Waveform::pulse, rate, hz, 0.3, 65'536)),
                      rate, hz, 20.0, 18'000.0);
  const double triangle = inharmonic_peak(
      amplitude_spectrum(render_oscillator(Waveform::triangle, rate, hz, 0.5, 65'536)), rate, hz,
      20.0, 18'000.0);
  std::cout << "pulse 30% A7 alias " << db(pulse) << " dBFS, triangle A7 alias " << db(triangle)
            << " dBFS\n";
  REQUIRE_LT(db(pulse), -80.0, "pulse aliasing below -80 dBFS");
  REQUIRE_LT(db(triangle), -90.0, "triangle aliasing below -90 dBFS");
}

void hard_sync_aliasing_stays_low() {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  constexpr double master_hz = 1'046.5; // C6
  for (Waveform waveform : {Waveform::saw, Waveform::sine, Waveform::triangle}) {
    BlepOscillator master;
    BlepOscillator slave;
    master.reset(0.0, Waveform::saw);
    slave.reset(0.0, waveform);
    std::vector<double> out(65'536);
    for (std::size_t i = 0; i < 4096 + out.size(); ++i) {
      SyncPoint wrap;
      (void)master.next(master_hz / rate, 0.5, {}, &wrap);
      const double value = slave.next(2.71 * master_hz / rate, 0.5, wrap, nullptr);
      if (i >= 4096) {
        out[i - 4096] = value;
      }
    }
    const double alias = inharmonic_peak(amplitude_spectrum(out), rate, master_hz, 20.0, 18'000.0);
    std::cout << "hard sync shape " << static_cast<int>(waveform) << " alias " << db(alias)
              << " dBFS\n";
    REQUIRE_LT(db(alias), -70.0, "hard-synced slave aliasing below -70 dBFS");
  }
}

void oscillator_pitch_matches_zero_crossing_oracle() {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  constexpr double hz = 440.0;
  const auto out = render_oscillator(Waveform::sine, rate, hz, 0.5, 96'000);
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
  const double measured = rate * crossings / (last - first);
  const double cents = 1200.0 * std::log2(measured / hz);
  REQUIRE_LT(std::fabs(cents), 0.5, "pitch within 0.5 cent");
}

void pulse_is_dc_free_and_width_sweeps_stay_bounded() {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  const auto narrow = render_oscillator(Waveform::pulse, rate, 220.0, 0.1, 48'000);
  double sum = 0.0;
  for (double sample : narrow) {
    sum += sample;
  }
  REQUIRE_LT(std::fabs(sum / static_cast<double>(narrow.size())), tolerance::coarse,
             "pulse is DC free");

  BlepOscillator osc;
  osc.reset(0.0, Waveform::pulse);
  double peak = 0.0;
  for (std::size_t i = 0; i < 96'000; ++i) {
    // Fast, deep PWM, including sweeps past the phase in both directions.
    const double width = 0.5 + 0.45 * std::sin(2.0 * pi * 37.0 * static_cast<double>(i) / rate);
    peak =
        std::max(peak, std::fabs(static_cast<double>(osc.next(330.0 / rate, width, {}, nullptr))));
  }
  // A static 5% DC-free pulse peaks at 2.07 (1.9 plus Gibbs ringing).
  REQUIRE_LT(peak, 2.1, "pulse width sweeps ring no more than a static pulse");
}

void live_waveform_switch_is_a_band_limited_step() {
  constexpr double increment = 50.0 / 48'000.0;
  constexpr std::size_t switch_frame = 2'000;
  BlepOscillator switched;
  BlepOscillator reference;
  switched.reset(0.3, Waveform::sine);
  reference.reset(0.3, Waveform::sine);
  BlepOscillator steady;
  steady.reset(0.3, Waveform::pulse);
  double steady_jump = 0.0;
  double switched_jump = 0.0;
  double previous_steady = 0.0;
  double previous_switched = 0.0;
  bool pre_ringing = false;
  const std::size_t released = switch_frame + BlepOscillator::latency_frames;
  for (std::size_t i = 0; i < 4'000; ++i) {
    if (i == switch_frame) {
      switched.set_waveform(Waveform::pulse, 0.5);
    }
    const double value = switched.next(increment, 0.5, {}, nullptr);
    const double sine = reference.next(increment, 0.5, {}, nullptr);
    const double pulse = steady.next(increment, 0.5, {}, nullptr);
    if (i + blep_half_width <= released) {
      REQUIRE_EQ(value, sine, "frames released before the kernel reach are untouched");
    } else if (i < released) {
      pre_ringing = pre_ringing || std::fabs(value - sine) > 1e-4;
    }
    if (i > 100) {
      steady_jump = std::max(steady_jump, std::fabs(pulse - previous_steady));
      switched_jump = std::max(switched_jump, std::fabs(value - previous_switched));
    }
    previous_steady = pulse;
    previous_switched = value;
  }
  require(pre_ringing, "switch step is spread symmetrically, not applied raw");
  REQUIRE_LT(switched_jump, steady_jump + 0.05, "switch is no sharper than a band-limited edge");
}

// ---- Filters -----------------------------------------------------------------

// Steady-state gain of a per-sample processor for a sine at hz.
template <typename Process>
double sine_gain(double rate, double hz, Process&& process, double amplitude = 1.0) {
  const auto settle = static_cast<std::size_t>(rate * 0.5);
  const auto measure = static_cast<std::size_t>(rate * 0.5);
  double input_power = 0.0;
  double output_power = 0.0;
  for (std::size_t i = 0; i < settle + measure; ++i) {
    const double x = amplitude * std::sin(2.0 * pi * hz * static_cast<double>(i) / rate);
    const double y = process(x);
    if (i >= settle) {
      input_power += x * x;
      output_power += y * y;
    }
  }
  return std::sqrt(output_power / input_power);
}

// Bilinear-transform oracle: a trapezoidal filter's response at hz equals the
// analog prototype's at the prewarped frequency ratio.
double prewarped_ratio(double rate, double hz, double cutoff) {
  return std::tan(pi * hz / rate) / std::tan(pi * cutoff / rate);
}

void svf_matches_analog_prototypes() {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  constexpr double cutoff = 1'000.0;
  for (double hz : {250.0, 1'000.0, 2'000.0, 8'000.0}) {
    const double w = prewarped_ratio(rate, hz, cutoff);
    const double q = oscillate::native_dsp::dsp::butterworth_q;
    const double denominator = std::sqrt((1 - w * w) * (1 - w * w) + (w / q) * (w / q));
    struct Case {
      FilterMode mode;
      double expected;
    };
    for (const Case c : {Case{FilterMode::lowpass_12, 1.0 / denominator},
                         Case{FilterMode::highpass_12, w * w / denominator},
                         Case{FilterMode::bandpass_12, w / denominator},
                         Case{FilterMode::lowpass_24, 1.0 / std::sqrt(1.0 + std::pow(w, 8.0))}}) {
      MultimodeFilter filter;
      const auto coefficients = make_filter_coefficients(c.mode, cutoff / rate, 0.0, 1.0);
      const double gain =
          sine_gain(rate, hz, [&](double x) { return filter.process(x, coefficients); });
      REQUIRE_LT(std::fabs(db(gain) - db(c.expected)), 0.05, "SVF matches analog prototype");
    }
  }
}

void ladder_small_signal_and_self_oscillation() {
  constexpr double rate = oscillate::test::sample_rate_96k_hz;
  constexpr double cutoff = 1'200.0;
  for (double hz : {300.0, 1'200.0, 4'800.0}) {
    const double w = prewarped_ratio(rate, hz, cutoff);
    MultimodeFilter filter;
    const auto coefficients =
        make_filter_coefficients(FilterMode::ladder_24, cutoff / rate, 0.0, 1.0);
    const double gain =
        sine_gain(rate, hz, [&](double x) { return filter.process(x, coefficients); }, 0.01);
    REQUIRE_LT(std::fabs(db(gain) - db(1.0 / (1.0 + w * w) / (1.0 + w * w) * 1.0)), 0.1,
               "ladder matches four one-pole stages at zero resonance");
  }

  MultimodeFilter filter;
  const auto coefficients =
      make_filter_coefficients(FilterMode::ladder_24, cutoff / rate, 1.0, 1.0);
  std::vector<double> out(static_cast<std::size_t>(rate));
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = filter.process(i == 0 ? 1.0 : 0.0, coefficients);
  }
  double power = 0.0;
  double peak = 0.0;
  int crossings = 0;
  const std::size_t tail = out.size() * 3 / 4;
  for (std::size_t i = tail; i < out.size(); ++i) {
    power += out[i] * out[i];
    peak = std::max(peak, std::fabs(out[i]));
    crossings += out[i - 1] < 0.0 && out[i] >= 0.0 ? 1 : 0;
  }
  const double measured_hz = crossings / (static_cast<double>(out.size() - tail) / rate);
  REQUIRE_GT(std::sqrt(power / static_cast<double>(out.size() - tail)), 0.05,
             "full-resonance ladder self-oscillates");
  REQUIRE_LT(peak, 2.0, "self-oscillation is bounded by saturation");
  REQUIRE_LT(std::fabs(measured_hz / cutoff - 1.0), 0.1, "self-oscillation is near cutoff");
}

void filters_stay_finite_under_audio_rate_modulation() {
  constexpr double rate = oscillate::test::sample_rate_44k_hz;
  NoiseSource noise;
  noise.reset(7);
  for (FilterMode mode : {FilterMode::lowpass_12, FilterMode::lowpass_24, FilterMode::bandpass_24,
                          FilterMode::highpass_24, FilterMode::notch_24, FilterMode::ladder_24}) {
    MultimodeFilter filter;
    double peak = 0.0;
    for (std::size_t i = 0; i < 200'000; ++i) {
      const double cutoff = 20.0 * std::pow(1'000.0, 0.5 + 0.5 * noise.white());
      const auto c = make_filter_coefficients(mode, cutoff / rate, 1.0, 16.0);
      const double y = filter.process(noise.white(), c);
      require(std::isfinite(y), "filter output stays finite");
      peak = std::max(peak, std::fabs(y));
    }
    REQUIRE_LT(peak, 100.0, "modulated resonant filter stays bounded");
  }
}

// ---- Oversampling ------------------------------------------------------------

void halfband_round_trip_is_flat_and_rejects_images() {
  constexpr double rate = oscillate::test::sample_rate_44k_hz;
  for (double hz : {100.0, 1'000.0, 10'000.0, 18'000.0}) {
    Upsampler2x up;
    Downsampler2x down;
    const double gain = sine_gain(rate, hz, [&](double x) {
      double a = 0.0;
      double b = 0.0;
      up.process(x, a, b);
      return down.process(a, b);
    });
    REQUIRE_LT(std::fabs(db(gain)), 0.01, "round trip is flat to 18 kHz");
  }

  // Images of an upsampled tone sit at base_rate - f in the doubled rate.
  constexpr double tone = 15'000.0;
  Upsampler2x up;
  std::vector<double> doubled(131'072);
  for (std::size_t i = 0; i < doubled.size() / 2; ++i) {
    up.process(std::sin(2.0 * pi * tone * static_cast<double>(i) / rate), doubled[2 * i],
               doubled[2 * i + 1]);
  }
  const auto spectrum = amplitude_spectrum(doubled);
  const double bin_hz = 2.0 * rate / static_cast<double>(doubled.size());
  const auto image_bin = static_cast<std::size_t>(std::round((rate - tone) / bin_hz));
  double image = 0.0;
  for (std::size_t bin = image_bin - 8; bin <= image_bin + 8; ++bin) {
    image = std::max(image, spectrum[bin]);
  }
  std::cout << "half-band image of 15 kHz at 44.1k: " << db(image) << " dB\n";
  REQUIRE_LT(db(image), -90.0, "upsampler image rejected by 90 dB");
}

// ---- Envelopes and LFO -------------------------------------------------------

void adsr_segments_land_exactly_for_every_curve() {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  for (double curve : {0.0, 0.3, 1.0}) {
    Adsr env;
    env.prepare(rate);
    env.set({0.010, 0.020, 0.25, 0.050, curve});
    env.gate_on();
    double previous = 0.0;
    for (int i = 1; i <= 480; ++i) {
      const double v = env.next();
      REQUIRE_GE(v, previous - 1e-12, "attack rises monotonically");
      require(i == 480 ? v == 1.0 : v < 1.0, "attack peaks on its final frame");
      previous = v;
    }
    for (int i = 1; i <= 960; ++i) {
      const double v = env.next();
      require(i == 960 ? v == 0.25 : v > 0.25, "decay lands on sustain on its final frame");
    }
    REQUIRE_EQ(env.stage(), Adsr::Stage::sustain, "decay enters sustain");
    env.gate_off();
    for (int i = 1; i <= 2'400; ++i) {
      const double v = env.next();
      require(i == 2'400 ? v == 0.0 : v > 0.0, "release ends on its final frame");
    }
    require(!env.active(), "released envelope is idle");
  }
}

void adsr_retrigger_and_early_release_are_continuous() {
  Adsr env;
  env.prepare(48'000.0);
  env.set({0.050, 0.1, 0.6, 0.2, 0.5});
  env.gate_on();
  double previous = 0.0;
  double largest = 0.0;
  for (int i = 0; i < 20'000; ++i) {
    if (i == 1'000) {
      env.gate_off(); // Mid-attack.
    }
    if (i == 3'000) {
      env.gate_on(); // Mid-release retrigger.
    }
    if (i == 5'000) {
      env.set({0.010, 0.3, 0.2, 0.05, 0.9}); // Live edit mid-decay.
    }
    const double v = env.next();
    if (i > 0) {
      largest = std::max(largest, std::fabs(v - previous));
    }
    previous = v;
  }
  REQUIRE_LT(largest, 1.0 / 400.0, "no envelope steps on retrigger, release or edit");
}

void lfo_rate_and_shapes() {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  Lfo lfo;
  lfo.prepare(rate);
  lfo.reset(0.0, 3);
  lfo.set_shape(LfoShape::triangle);
  int wraps = 0;
  double previous_phase = 0.0;
  for (int i = 0; i < 48'000; ++i) {
    const double v = lfo.next(5.0 / rate);
    REQUIRE_GE(v, -1.0, "LFO is bipolar and bounded");
    REQUIRE_LE(v, 1.0, "LFO is bipolar and bounded");
    wraps += lfo.phase() < previous_phase ? 1 : 0;
    previous_phase = lfo.phase();
  }
  REQUIRE_EQ(wraps, 5, "LFO completes exactly its rate in cycles per second");

  lfo.set_shape(LfoShape::square);
  double previous = lfo.next(5.0 / rate);
  double largest = 0.0;
  for (int i = 0; i < 48'000; ++i) {
    const double v = lfo.next(5.0 / rate);
    largest = std::max(largest, std::fabs(v - previous));
    previous = v;
  }
  REQUIRE_LT(largest, 0.05, "square LFO edges are slewed");
}

// ---- Chorus, noise and shaping -----------------------------------------------

void chorus_off_is_transparent_and_modes_are_stereo() {
  Chorus chorus;
  chorus.prepare(48'000.0);
  chorus.set(ChorusMode::off, 1.0);
  for (int i = 0; i < 1'000; ++i) {
    const double x = std::sin(i * 0.05);
    const auto out = chorus.process(x, x);
    REQUIRE_EQ(out.left, x, "chorus off passes input unchanged");
    REQUIRE_EQ(out.right, x, "chorus off passes input unchanged");
  }
  chorus.set(ChorusMode::one, 1.0);
  double difference = 0.0;
  double previous = 0.0;
  double largest_step = 0.0;
  for (int i = 0; i < 96'000; ++i) {
    if (i == 48'000) {
      chorus.set(ChorusMode::both, 1.0);
    }
    const double x = std::sin(2.0 * pi * 440.0 * i / 48'000.0);
    const auto out = chorus.process(x, x);
    REQUIRE_TRUE(std::isfinite(out.left), "chorus stays finite");
    REQUIRE_TRUE(std::isfinite(out.right), "chorus stays finite");
    difference += std::fabs(out.left - out.right);
    largest_step = std::max(largest_step, std::fabs(out.left - previous));
    previous = out.left;
  }
  REQUIRE_GT(difference / 96'000.0, 0.01, "chorus widens a mono source");
  // A 440 Hz unit sine moves at most 0.058 per frame; allow pitch wobble.
  REQUIRE_LT(largest_step, 0.08, "mode switch glides without clicks");
}

void noise_is_white_or_pink() {
  NoiseSource noise;
  noise.reset(11);
  double sum = 0.0;
  double square = 0.0;
  constexpr int count = 400'000;
  for (int i = 0; i < count; ++i) {
    const double w = noise.white();
    REQUIRE_GE(w, -1.0, "white noise is bounded");
    REQUIRE_LT(w, 1.0, "white noise is bounded");
    sum += w;
    square += w * w;
  }
  REQUIRE_LT(std::fabs(sum / count), 0.01, "white noise is zero mean");
  REQUIRE_LT(std::fabs(square / count - 1.0 / 3.0), 0.01, "white noise is uniform");

  std::vector<double> pink(262'144);
  for (double& sample : pink) {
    sample = noise.next(NoiseColour::pink);
  }
  const auto spectrum = amplitude_spectrum(pink);
  const double bin_hz = 48'000.0 / static_cast<double>(pink.size());
  const auto band_power = [&](double low, double high) {
    double power = 0.0;
    for (auto bin = static_cast<std::size_t>(low / bin_hz);
         bin < static_cast<std::size_t>(high / bin_hz); ++bin) {
      power += spectrum[bin] * spectrum[bin];
    }
    return power;
  };
  // Pink noise has equal power per octave.
  const double ratio = 10.0 * std::log10(band_power(1'600.0, 3'200.0) / band_power(200.0, 400.0));
  REQUIRE_LT(std::fabs(ratio), 1.5, "pink noise has equal power per octave");
}

void soft_clip_is_bounded_and_smooth() {
  double previous = soft_clip(-10.0);
  for (double x = -10.0; x <= 10.0; x += 0.001) {
    const double y = soft_clip(x);
    REQUIRE_GE(y, -1.0, "soft clip is bounded");
    REQUIRE_LE(y, 1.0, "soft clip is bounded");
    REQUIRE_GE(y, previous, "soft clip is monotonic");
    REQUIRE_LT(std::fabs(y - previous), 0.0011, "soft clip is continuous with slope <= 1");
    previous = y;
  }
  REQUIRE_LT(std::fabs(soft_clip(0.01) - 0.01), tolerance::float_processing,
             "soft clip is unity near zero");
}

void dc_blocker_removes_offset() {
  DcBlocker blocker;
  blocker.prepare(48'000.0);
  double y = 0.0;
  for (int i = 0; i < 96'000; ++i) {
    y = blocker.process(0.5 + 0.1 * std::sin(i * 0.1));
  }
  REQUIRE_LT(std::fabs(y), 0.11, "DC offset is removed");
}

void processing_does_not_allocate() {
  BlepOscillator osc;
  osc.reset(0.0, Waveform::saw);
  BlepOscillator slave;
  slave.reset(0.0, Waveform::sine);
  MultimodeFilter filter;
  Adsr env;
  env.prepare(48'000.0);
  Lfo lfo;
  lfo.prepare(48'000.0);
  Chorus chorus;
  chorus.prepare(48'000.0);
  chorus.set(ChorusMode::one, 1.0);
  Upsampler2x up;
  Downsampler2x down;
  NoiseSource noise;
  noise.reset(5);

  counting_allocations = true;
  counted_allocations = 0;
  env.gate_on();
  double sink = 0.0;
  for (int i = 0; i < 4'096; ++i) {
    SyncPoint wrap;
    const double master = osc.next(0.01, 0.5, {}, &wrap);
    const double x = master + slave.next(0.027, 0.5, wrap, nullptr) + noise.next(NoiseColour::pink);
    const auto c =
        make_filter_coefficients(FilterMode::ladder_24, 0.1 + 0.05 * lfo.next(1e-4), 0.7, 4.0);
    double a = 0.0;
    double b = 0.0;
    up.process(x, a, b);
    const double y = down.process(filter.process(a, c), filter.process(b, c)) * env.next();
    sink += chorus.process(y, y).left;
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "synth primitives do not allocate while processing");
  require(std::isfinite(sink), "allocation workload stays finite");
}

} // namespace

int main() {
  try {
    blep_tables_are_band_limited_residuals();
    step_lands_at_the_reported_latency();
    saw_aliasing_stays_below_minus_80_db_at_c8();
    pulse_and_triangle_aliasing_stay_low();
    hard_sync_aliasing_stays_low();
    oscillator_pitch_matches_zero_crossing_oracle();
    pulse_is_dc_free_and_width_sweeps_stay_bounded();
    live_waveform_switch_is_a_band_limited_step();
    svf_matches_analog_prototypes();
    ladder_small_signal_and_self_oscillation();
    filters_stay_finite_under_audio_rate_modulation();
    halfband_round_trip_is_flat_and_rejects_images();
    adsr_segments_land_exactly_for_every_curve();
    adsr_retrigger_and_early_release_are_continuous();
    lfo_rate_and_shapes();
    chorus_off_is_transparent_and_modes_are_stereo();
    noise_is_white_or_pink();
    soft_clip_is_bounded_and_smooth();
    dc_blocker_removes_offset();
    processing_does_not_allocate();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "synth DSP contract passed\n";
  return 0;
}
