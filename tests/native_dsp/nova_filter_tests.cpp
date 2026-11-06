#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/nova/filter.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/signal.hpp"

#include <algorithm>
#include <array>
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

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using namespace oscillate::native_dsp::synth::nova;
constexpr double pi = std::numbers::pi;

using oscillate::test::require;

std::string describe(FilterModel model) { return std::string(filter_model_name(model)); }

double db(double gain) { return oscillate::test::gain_to_db(gain, oscillate::test::silence_floor); }

FilterSettings settings(FilterModel model, double cutoff, double resonance, double drive = 0.0,
                        double fat = 0.0) {
  FilterSettings s;
  s.model = model;
  s.cutoff_hz = cutoff;
  s.resonance = resonance;
  s.drive_db = drive;
  s.fat = fat;
  return s;
}

// Deterministic noise for inputs and fuzzing.
class Random {
public:
  explicit Random(std::uint32_t seed) : state_(seed) {}
  double uniform() {
    state_ = state_ * 1'664'525U + 1'013'904'223U;
    return static_cast<double>(state_ >> 8U) / 16'777'216.0;
  }
  double bipolar() { return 2.0 * uniform() - 1.0; }

private:
  std::uint32_t state_;
};

// The steady-state sine gain (RMS out over RMS in) of a filter on the left
// channel, over a whole number of cycles.
double sine_gain(const FilterSettings& s, double rate, double hz, double amplitude,
                 double settle_seconds = 0.15) {
  VoiceFilter filter;
  filter.prepare(rate);
  filter.set(s, 0);
  const auto settle = static_cast<std::size_t>(rate * settle_seconds);
  const double period = rate / hz;
  const double cycles = std::max(4.0, std::floor(0.1 * rate / period));
  const auto measure = static_cast<std::size_t>(std::round(cycles * period));
  double in = 0.0;
  double out = 0.0;
  for (std::size_t i = 0; i < settle + measure; ++i) {
    const double x = amplitude * std::sin(2.0 * pi * hz * static_cast<double>(i) / rate);
    float left = static_cast<float>(x);
    float right = 0.0F;
    filter.process(left, right);
    if (i >= settle) {
      in += x * x;
      out += static_cast<double>(left) * left;
    }
  }
  return std::sqrt(out / in);
}

// tan(pi f / fs) / tan(pi fc / fs): the analog frequency a bilinear
// prototype sees.
double warped(double rate, double hz, double cutoff) {
  return std::tan(pi * hz / rate) / std::tan(pi * cutoff / rate);
}

// ---- Responses ----------------------------------------------------------------------

void responses_match_analytic_curves() {
  const std::array models{FilterModel::ladder24, FilterModel::ladder12,  FilterModel::diode,
                          FilterModel::mg_low,   FilterModel::ota,       FilterModel::lp12,
                          FilterModel::lp24,     FilterModel::bp12,      FilterModel::bp24,
                          FilterModel::hp12,     FilterModel::hp24,      FilterModel::notch,
                          FilterModel::peak,     FilterModel::morph_lbh, FilterModel::dual_peak};
  for (const double rate : {48'000.0, 96'000.0}) {
    const std::array<double, 8> probes{50.0,    200.0,      700.0,      2'000.0,
                                       5'000.0, 0.2 * rate, 0.3 * rate, 0.4 * rate};
    for (const auto model : models) {
      for (const double cutoff : {400.0, 3'000.0}) {
        for (const double resonance : {0.0, 0.5}) {
          const auto s =
              settings(model, cutoff, resonance, 0.0, model == FilterModel::morph_lbh ? 0.3 : 0.0);
          for (const double hz : probes) {
            const double expected = filter_response_db(s, hz, rate);
            if (expected < -70.0) {
              continue; // Below the measurement floor.
            }
            const double measured = db(sine_gain(s, rate, hz, 0.01));
            REQUIRE_LT(std::fabs(measured - expected), 0.5,
                       describe(model) + " measured response matches its analytic curve at " +
                           std::to_string(hz) + " Hz (" + std::to_string(measured) + " vs " +
                           std::to_string(expected) + ")");
          }
        }
      }
    }
  }
}

void clean_and_ladder_curves_match_their_prototypes() {
  // Independent oracles at resonance 0: Butterworth SVFs and the
  // uncompensated four-pole ladder, through the bilinear transform.
  const double rate = oscillate::test::sample_rate_48k_hz;
  const double cutoff = 1'500.0;
  for (const double hz : {100.0, 700.0, 1'500.0, 4'000.0, 12'000.0, 19'000.0}) {
    const double w = warped(rate, hz, cutoff);
    const auto check = [&](FilterModel model, double expected_db) {
      const double analytic = filter_response_db(settings(model, cutoff, 0.0), hz, rate);
      REQUIRE_LT(std::fabs(analytic - expected_db), 0.05,
                 describe(model) + " response is its analog prototype at " + std::to_string(hz));
    };
    check(FilterModel::lp12, -10.0 * std::log10(1.0 + std::pow(w, 4.0)));
    check(FilterModel::hp12, -10.0 * std::log10(1.0 + std::pow(w, -4.0)));
    check(FilterModel::lp24, -10.0 * std::log10(1.0 + std::pow(w, 8.0)));
    check(FilterModel::hp24, -10.0 * std::log10(1.0 + std::pow(w, -8.0)));
    check(FilterModel::ladder24, -40.0 * std::log10(1.0 + w * w));
    check(FilterModel::ladder12, -20.0 * std::log10(1.0 + w * w));
  }
  REQUIRE_LT(std::fabs(filter_response_db(settings(FilterModel::lp12, cutoff, 0.0), cutoff, rate) +
                       3.0103),
             0.01, "LP 12 is 3 dB down at its cutoff with no resonance");
  // Resonance raises the clean low-pass peak towards Q = 40.
  REQUIRE_GT(filter_response_db(settings(FilterModel::lp12, cutoff, 1.0), cutoff, rate), 30.0,
             "full resonance gives a +30 dB SVF peak");
}

// ---- Self-oscillation ---------------------------------------------------------------

double oscillation_hz(FilterModel model, double cutoff, double rate, double& peak) {
  VoiceFilter filter;
  filter.prepare(rate);
  filter.set(settings(model, cutoff, 1.0), 0);
  const auto total = static_cast<std::size_t>(rate * 1.5);
  const auto start = static_cast<std::size_t>(rate * 1.0);
  double previous = 0.0;
  double first = -1.0;
  double last = -1.0;
  std::size_t crossings = 0;
  peak = 0.0;
  for (std::size_t i = 0; i < total; ++i) {
    float left = i == 0 ? 0.5F : 0.0F;
    float right = left;
    filter.process(left, right);
    const double y = left;
    if (i >= start) {
      peak = std::max(peak, std::fabs(y));
      if (previous < 0.0 && y >= 0.0) {
        const double at = static_cast<double>(i - 1) + (-previous) / (y - previous);
        if (first < 0.0) {
          first = at;
        }
        last = at;
        ++crossings;
      }
    }
    previous = y;
  }
  if (crossings < 2) {
    return 0.0;
  }
  return static_cast<double>(crossings - 1) / (last - first) * rate;
}

void ladders_self_oscillate_in_tune() {
  const double rate = oscillate::test::sample_rate_96k_hz;
  for (const auto model : {FilterModel::ladder24, FilterModel::ladder12, FilterModel::diode,
                           FilterModel::mg_low, FilterModel::ota}) {
    for (const double cutoff : {220.0, 880.0, 3'520.0}) {
      double peak = 0.0;
      const double hz = oscillation_hz(model, cutoff, rate, peak);
      require(hz > 0.0 && peak > 0.05, describe(model) + " self-oscillates at full resonance");
      REQUIRE_LT(peak, 2.0, describe(model) + " self-oscillation stays bounded");
      const double cents = 1'200.0 * std::log2(hz / cutoff);
      REQUIRE_LT(std::fabs(cents), 5.0,
                 describe(model) + " oscillates within 5 cents of " + std::to_string(cutoff) +
                     " Hz (" + std::to_string(cents) + " ct)");
    }
  }
  // Below the threshold the ladder rings down instead.
  VoiceFilter filter;
  filter.prepare(rate);
  filter.set(settings(FilterModel::ladder24, 880.0, 0.8), 0);
  double tail = 0.0;
  for (int i = 0; i < 96'000; ++i) {
    float left = i == 0 ? 0.5F : 0.0F;
    float right = left;
    filter.process(left, right);
    if (i > 86'000) {
      tail = std::max(tail, static_cast<double>(std::fabs(left)));
    }
  }
  REQUIRE_LT(tail, 1e-4, "a ladder below the self-oscillation threshold decays");
}

// ---- Combs --------------------------------------------------------------------------

// The analytic peak lies on the multiple within 0.5 %; the running comb
// agrees with it there and falls away 3 % of the peak spacing (the cutoff)
// either side. Damping (FAT) widens the upper peaks, so the side margin is
// 1 dB.
double analytic_peak(const FilterSettings& s, double around, double rate) {
  double best_hz = around;
  double best = -1e9;
  for (double hz = 0.97 * around; hz <= 1.03 * around; hz += 0.0005 * around) {
    const double value = filter_response_db(s, hz, rate);
    if (value > best) {
      best = value;
      best_hz = hz;
    }
  }
  return best_hz;
}

void require_peak(const FilterSettings& s, double at, double spacing, double rate,
                  const std::string& what) {
  REQUIRE_LT(std::fabs(analytic_peak(s, at, rate) / at - 1.0), 0.005,
             what + " has its analytic peak on " + std::to_string(at) + " Hz");
  const double peak = db(sine_gain(s, rate, at, 0.05, 0.3));
  const double above = db(sine_gain(s, rate, at + 0.03 * spacing, 0.05, 0.3));
  const double below = db(sine_gain(s, rate, at - 0.03 * spacing, 0.05, 0.3));
  require(peak > above + 1.0 && peak > below + 1.0,
          what + " peaks on " + std::to_string(at) + " Hz (" + std::to_string(below) + ", " +
              std::to_string(peak) + ", " + std::to_string(above) + " dB)");
  REQUIRE_LT(std::fabs(peak - filter_response_db(s, at, rate)), 1.0,
             what + " measured peak matches the analytic response");
}

void combs_peak_on_multiples_of_the_cutoff() {
  const double rate = oscillate::test::sample_rate_48k_hz;
  const double cutoff = 400.0;
  for (const double fat : {0.0, 0.5}) {
    const auto plus = settings(FilterModel::comb_plus, cutoff, 0.7, 0.0, fat);
    const auto minus = settings(FilterModel::comb_minus, cutoff, 0.7, 0.0, fat);
    for (int m = 1; m <= 4; ++m) {
      const double on = cutoff * m;
      require_peak(plus, on, cutoff, rate, "Comb +");
      REQUIRE_GT(db(sine_gain(plus, rate, on, 0.05, 0.3)),
                 db(sine_gain(plus, rate, on + 0.5 * cutoff, 0.05, 0.3)) + 10.0,
                 "Comb + is deep between its peaks");
      const double half = on - 0.5 * cutoff;
      require_peak(minus, half, cutoff, rate, "Comb −");
      REQUIRE_GT(db(sine_gain(minus, rate, half, 0.05, 0.3)),
                 db(sine_gain(minus, rate, on, 0.05, 0.3)) + 10.0,
                 "Comb − notches the multiples of the cutoff");
    }
  }
  // Damping (FAT) takes the top off the comb.
  const auto bright = settings(FilterModel::comb_plus, cutoff, 0.7, 0.0, 0.0);
  const auto dark = settings(FilterModel::comb_plus, cutoff, 0.7, 0.0, 1.0);
  REQUIRE_LT(filter_response_db(dark, 8'000.0, rate),
             filter_response_db(bright, 8'000.0, rate) - 6.0, "FAT damps the comb's high peaks");
  // The flanger's first notch sits on its cutoff.
  const auto flanger = settings(FilterModel::flanger, 1'000.0, 0.3);
  REQUIRE_LT(filter_response_db(flanger, 1'000.0, rate),
             filter_response_db(flanger, 2'000.0, rate) - 12.0,
             "the flanger notches at its cutoff and peaks at twice it");
}

// ---- Formants -----------------------------------------------------------------------

// The strongest response between low and high Hz, by a fine log scan.
double strongest(const FilterSettings& s, double low, double high, double rate) {
  double best_hz = low;
  double best = -1e9;
  for (double hz = low; hz <= high; hz *= 1.002) {
    const double value = filter_response_db(s, hz, rate);
    if (value > best) {
      best = value;
      best_hz = hz;
    }
  }
  return best_hz;
}

void formants_move_with_fat_and_cutoff() {
  const double rate = oscillate::test::sample_rate_48k_hz;
  // Formant: FAT walks the vowel, the cutoff shifts the whole voice.
  const double a =
      strongest(settings(FilterModel::formant, 800.0, 0.5, 0.0, 0.0), 120.0, 900.0, rate);
  const double i =
      strongest(settings(FilterModel::formant, 800.0, 0.5, 0.0, 0.5), 120.0, 900.0, rate);
  const double a_up =
      strongest(settings(FilterModel::formant, 1'600.0, 0.5, 0.0, 0.0), 240.0, 1'800.0, rate);
  REQUIRE_LT(std::fabs(a / 600.0 - 1.0), 0.06, "Formant A has its first formant near 600 Hz");
  REQUIRE_LT(std::fabs(i / 250.0 - 1.0), 0.1, "Formant FAT 0.5 sings I, first formant near 250 Hz");
  REQUIRE_LT(std::fabs(a_up / (2.0 * a) - 1.0), 0.03, "doubling the cutoff doubles the formants");
  // Vowel: the cutoff walks the vowel, FAT shifts the voice.
  const double low =
      strongest(settings(FilterModel::vowel, 100.0, 0.5, 0.0, 0.0), 120.0, 900.0, rate);
  const double mid =
      strongest(settings(FilterModel::vowel, 1'000.0, 0.5, 0.0, 0.0), 120.0, 900.0, rate);
  const double small =
      strongest(settings(FilterModel::vowel, 100.0, 0.5, 0.0, 1.0), 240.0, 1'800.0, rate);
  REQUIRE_LT(std::fabs(low / 600.0 - 1.0), 0.08, "Vowel at the bottom of its sweep sings A");
  REQUIRE_LT(std::fabs(mid / 250.0 - 1.0), 0.12, "Vowel mid-sweep sings I");
  REQUIRE_LT(std::fabs(small / (2.0 * low) - 1.0), 0.03, "Vowel FAT 1 raises the voice an octave");
  // The running filter agrees with the analytic curve at the peak.
  const auto s = settings(FilterModel::formant, 800.0, 0.5);
  REQUIRE_LT(std::fabs(db(sine_gain(s, rate, a, 0.01)) - filter_response_db(s, a, rate)), 0.5,
             "Formant measured peak matches its analytic response");
  const auto v = settings(FilterModel::vowel, 100.0, 0.5);
  REQUIRE_LT(std::fabs(db(sine_gain(v, rate, low, 0.01)) - filter_response_db(v, low, rate)), 0.5,
             "Vowel measured peak matches its analytic response");
}

// ---- Robustness ---------------------------------------------------------------------

void random_settings_and_sweeps_stay_finite() {
  for (const double rate : {44'100.0, 48'000.0, 96'000.0, 192'000.0}) {
    Random random(static_cast<std::uint32_t>(rate));
    VoiceFilter filter;
    filter.prepare(rate);
    const auto total = static_cast<std::size_t>(rate * 0.6);
    const auto silence_from = static_cast<std::size_t>(rate * 0.4);
    double phase = 0.0;
    for (std::size_t i = 0; i < total; ++i) {
      if (i % 32 == 0 && i < silence_from) {
        const double pick = random.uniform();
        const double cutoff = pick < 0.15  ? 8.0
                              : pick < 0.3 ? 22'000.0
                                           : 8.0 * std::pow(22'000.0 / 8.0, random.uniform());
        auto s = settings(static_cast<FilterModel>(
                              static_cast<std::size_t>(random.uniform() * filter_model_count) %
                              filter_model_count),
                          cutoff, random.uniform() < 0.3 ? 1.0 : random.uniform(),
                          24.0 * random.uniform(), random.uniform());
        const std::array<std::uint32_t, 5> ramps{0, 1, 7, 32, 64};
        filter.set(s, ramps[static_cast<std::size_t>(random.uniform() * 5.0) % 5]);
      }
      double x = 0.0;
      if (i < silence_from) {
        const auto segment = (i / 2'048) % 4;
        phase += 55.0 * (1.0 + 40.0 * random.uniform()) / rate;
        x = segment == 0   ? 4.0 * random.bipolar()
            : segment == 1 ? 2.0 * (phase - std::floor(phase)) - 1.0
            : segment == 2 ? (i % 997 == 0 ? 8.0 : 0.0)
                           : 0.5 * std::sin(2.0 * pi * phase);
      }
      float left = static_cast<float>(x);
      float right = static_cast<float>(-0.5 * x);
      filter.process(left, right);
      REQUIRE_TRUE(std::isfinite(left), "output stays finite");
      REQUIRE_TRUE(std::isfinite(right), "output stays finite");
      REQUIRE_LT(std::fabs(left), 100.0, "output stays bounded");
      REQUIRE_LT(std::fabs(right), 100.0, "output stays bounded");
      REQUIRE_NE(std::fpclassify(left), FP_SUBNORMAL, "output never goes denormal");
      REQUIRE_NE(std::fpclassify(right), FP_SUBNORMAL, "output never goes denormal");
    }
  }
  // Extreme sweeps: the full cutoff range every 16 frames under a loud saw,
  // at full resonance, for every model.
  for (std::size_t m = 0; m < filter_model_count; ++m) {
    const auto model = static_cast<FilterModel>(m);
    VoiceFilter filter;
    filter.prepare(192'000.0);
    filter.set(settings(model, 8.0, 1.0, 24.0, 1.0), 0);
    double phase = 0.0;
    double peak = 0.0;
    for (int i = 0; i < 48'000; ++i) {
      if (i % 16 == 0) {
        filter.set(settings(model, (i / 16) % 2 == 0 ? 22'000.0 : 8.0, 1.0, 24.0, 1.0), 16);
      }
      phase += 110.0 / 192'000.0;
      float left = static_cast<float>(2.0 * (phase - std::floor(phase)) - 1.0);
      float right = left;
      filter.process(left, right);
      if (!std::isfinite(left) || !std::isfinite(right)) {
        require(false, describe(model) + " survives sweeps");
      }
      peak = std::max(peak, static_cast<double>(std::fabs(left)));
    }
    REQUIRE_LT(peak, 30.0, describe(model) + " stays bounded under audio-rate sweeps");
  }
}

void resonance_never_blows_up() {
  const double rate = oscillate::test::sample_rate_48k_hz;
  for (std::size_t m = 0; m < filter_model_count; ++m) {
    const auto model = static_cast<FilterModel>(m);
    for (const double cutoff : {110.0, 1'000.0, 5'000.0}) {
      for (const double fat : {0.0, 1.0}) {
        VoiceFilter filter;
        filter.prepare(rate);
        filter.set(settings(model, cutoff, 1.0, 0.0, fat), 0);
        double phase = 0.0;
        double peak = 0.0;
        for (int i = 0; i < 24'000; ++i) {
          phase += 110.0 / rate;
          float left = static_cast<float>(2.0 * (phase - std::floor(phase)) - 1.0);
          float right = left;
          filter.process(left, right);
          peak = std::max(peak, static_cast<double>(std::fabs(left)));
        }
        REQUIRE_LT(peak, 20.0,
                   describe(model) + " at full resonance stays under +26 dB on a full-scale saw");
      }
    }
  }
}

// ---- Model switches -----------------------------------------------------------------

double largest_step(FilterModel model, double rate, std::size_t from, std::size_t length) {
  VoiceFilter filter;
  filter.prepare(rate);
  filter.set(settings(model, 1'000.0, 0.3), 0);
  double previous = 0.0;
  double largest = 0.0;
  for (std::size_t i = 0; i < from + length; ++i) {
    float left =
        static_cast<float>(0.5 * std::sin(2.0 * pi * 220.0 * static_cast<double>(i) / rate));
    float right = left;
    filter.process(left, right);
    if (i > from) {
      largest = std::max(largest, std::fabs(left - previous));
    }
    previous = left;
  }
  return largest;
}

void model_switches_crossfade_without_steps() {
  const double rate = oscillate::test::sample_rate_48k_hz;
  const auto settle = static_cast<std::size_t>(rate * 0.3);
  const auto window = static_cast<std::size_t>(rate * 0.05);
  const std::array<std::array<FilterModel, 2>, 6> pairs{{
      {FilterModel::ladder24, FilterModel::comb_plus},
      {FilterModel::lp12, FilterModel::hp12},
      {FilterModel::formant, FilterModel::ring},
      {FilterModel::diode, FilterModel::flanger},
      {FilterModel::hp24, FilterModel::ladder24},
      {FilterModel::phaser, FilterModel::notch},
  }};
  for (const auto& pair : pairs) {
    const double bound = std::max(largest_step(pair[0], rate, settle, window),
                                  largest_step(pair[1], rate, settle, window));
    VoiceFilter filter;
    filter.prepare(rate);
    filter.set(settings(pair[0], 1'000.0, 0.3), 0);
    double previous = 0.0;
    double largest = 0.0;
    for (std::size_t i = 0; i < settle + window; ++i) {
      if (i == settle) {
        filter.set(settings(pair[1], 1'000.0, 0.3), 64);
      }
      float left =
          static_cast<float>(0.5 * std::sin(2.0 * pi * 220.0 * static_cast<double>(i) / rate));
      float right = left;
      filter.process(left, right);
      if (i > settle - 8) {
        largest = std::max(largest, std::fabs(left - previous));
      }
      previous = left;
    }
    REQUIRE_LT(largest, 1.5 * bound + 0.01,
               describe(pair[0]) + " to " + describe(pair[1]) + " switches without a step");
  }
}

// ---- Calling patterns ---------------------------------------------------------------

struct Event {
  std::size_t at;
  FilterSettings settings;
  std::uint32_t frames;
};

std::vector<float> render(double rate, const std::vector<Event>& events, std::size_t length) {
  VoiceFilter filter;
  filter.prepare(rate);
  Random random(7);
  std::vector<float> out;
  out.reserve(2 * length);
  std::size_t next = 0;
  double phase = 0.0;
  for (std::size_t i = 0; i < length; ++i) {
    while (next < events.size() && events[next].at == i) {
      filter.set(events[next].settings, events[next].frames);
      ++next;
    }
    phase += 98.0 / rate;
    float left = static_cast<float>(0.6 * (2.0 * (phase - std::floor(phase)) - 1.0) +
                                    0.1 * random.bipolar());
    float right = static_cast<float>(0.3 * random.bipolar());
    filter.process(left, right);
    out.push_back(left);
    out.push_back(right);
  }
  return out;
}

void block_split_independence() {
  for (const double rate : {48'000.0, 96'000.0}) {
    // A ramped tour through several models, one target every 256 frames.
    std::vector<Event> plain;
    const std::array<FilterModel, 6> tour{FilterModel::ladder24,  FilterModel::formant,
                                          FilterModel::comb_plus, FilterModel::diode,
                                          FilterModel::phaser,    FilterModel::lp24};
    Random random(3);
    for (std::size_t k = 0; k < 48; ++k) {
      const auto model = tour[(k / 8) % tour.size()];
      plain.push_back({k * 256,
                       settings(model, 60.0 * std::pow(200.0, random.uniform()), random.uniform(),
                                12.0 * random.uniform(), random.uniform()),
                       k == 0 ? 0U : 256U});
    }
    const std::size_t length = 48 * 256 + 512;
    const auto reference = render(rate, plain, length);
    // The same targets re-sent mid-ramp with the frames that remain, and
    // re-sent again once reached: the output must not change.
    std::vector<Event> split;
    for (const auto& event : plain) {
      split.push_back(event);
      if (event.frames > 0) {
        const std::size_t offset = 1 + static_cast<std::size_t>(random.uniform() * 200.0);
        split.push_back(
            {event.at + offset, event.settings, static_cast<std::uint32_t>(event.frames - offset)});
        split.push_back({event.at + event.frames, event.settings, 64U});
      }
    }
    std::stable_sort(split.begin(), split.end(),
                     [](const Event& a, const Event& b) { return a.at < b.at; });
    const auto resent = render(rate, split, length);
    double worst = 0.0;
    for (std::size_t i = 0; i < reference.size(); ++i) {
      worst = std::max(worst, static_cast<double>(std::fabs(reference[i] - resent[i])));
    }
    REQUIRE_LT(worst, 1e-5,
               "output does not depend on how the ramps are re-sent (" + std::to_string(worst) +
                   ")");
    REQUIRE_EQ(render(rate, plain, length), reference, "rendering is deterministic");
  }
}

void ramps_reach_their_targets_linearly() {
  // After a ramp the filter matches one set to the target directly.
  const double rate = oscillate::test::sample_rate_48k_hz;
  VoiceFilter ramped;
  VoiceFilter direct;
  ramped.prepare(rate);
  direct.prepare(rate);
  ramped.set(settings(FilterModel::lp24, 200.0, 0.1), 0);
  direct.set(settings(FilterModel::lp24, 200.0, 0.1), 0);
  ramped.set(settings(FilterModel::lp24, 5'000.0, 0.6), 480);
  direct.set(settings(FilterModel::lp24, 5'000.0, 0.6), 0);
  // Feed silence through the ramp, then compare a sine's steady state.
  for (int i = 0; i < 480; ++i) {
    float l = 0.0F;
    float r = 0.0F;
    ramped.process(l, r);
    float dl = 0.0F;
    float dr = 0.0F;
    direct.process(dl, dr);
  }
  double worst = 0.0;
  for (int i = 0; i < 4'800; ++i) {
    const auto x = static_cast<float>(0.3 * std::sin(2.0 * pi * 1'000.0 * i / rate));
    float l = x;
    float r = x;
    ramped.process(l, r);
    float dl = x;
    float dr = x;
    direct.process(dl, dr);
    worst = std::max(worst, static_cast<double>(std::fabs(l - dl)));
  }
  REQUIRE_LT(worst, 1e-6, "a ramp lands exactly on its target");
}

// Formant, Vowel and Phaser blend their coefficients through a gentle
// ramp (VoiceFilter::blends): against the reference path, which derives at
// every refresh, a slow sweep of cutoff and FAT stays within 1e-6 of the
// output's peak (curvature of one tick's step), and a fast sweep, which
// derives exactly throughout, is identical. Ticks as a voice sends them: 64
// samples at the 2x-oversampled rate.
struct Pair {
  double worst = 0.0;
  double peak = 0.0;
};

// `sweep(t)`: the cutoff's offset in octaves at tick t (FAT moves with it).
template <typename Sweep> Pair blended_against_exact(FilterModel model, Sweep sweep_at) {
  const double rate = oscillate::test::sample_rate_96k_hz;
  constexpr std::uint32_t tick = 64;
  VoiceFilter blended;
  VoiceFilter exact;
  blended.prepare(rate);
  exact.prepare(rate);
  exact.set_exact(true);
  std::uint32_t noise = 0x1234567U;
  Pair result;
  double phase = 0.0;
  for (std::size_t t = 0; t < 3'000; ++t) {
    const double sweep = sweep_at(t);
    const auto s = settings(model, 1'000.0 * std::exp2(sweep), 0.45, 0.0, 0.5 + 0.4 * sweep);
    blended.set(s, t == 0 ? 0 : tick);
    exact.set(s, t == 0 ? 0 : tick);
    for (std::uint32_t i = 0; i < tick; ++i) {
      // A saw with a little noise: every band rings.
      phase += 110.0 / rate;
      phase -= std::floor(phase);
      noise = noise * 1'664'525U + 1'013'904'223U;
      const auto x =
          static_cast<float>(0.3 * (2.0 * phase - 1.0) + 0.02 * (noise / 4294967296.0 - 0.5));
      float bl = x, br = x, el = x, er = x;
      blended.process(bl, br);
      exact.process(el, er);
      result.worst = std::max(result.worst, static_cast<double>(std::fabs(bl - el)));
      result.worst = std::max(result.worst, static_cast<double>(std::fabs(br - er)));
      result.peak = std::max(result.peak, static_cast<double>(std::fabs(el)));
    }
  }
  return result;
}

void banked_models_blend_within_tolerance() {
  for (const auto model : {FilterModel::formant, FilterModel::vowel, FilterModel::phaser}) {
    // A sine LFO, one octave either way at 0.3 Hz: at most 1/800 octave
    // per tick, inside blend_step.
    const auto slow = blended_against_exact(model, [](std::size_t t) {
      return std::sin(2.0 * pi * 0.3 * static_cast<double>(t) * 64.0 / 96'000.0);
    });
    REQUIRE_GT(slow.peak, 0.01, describe(model) + " passes the saw");
    REQUIRE_LE(slow.worst, 1e-6 * slow.peak,
               describe(model) + " blends a slow sweep within 1e-6 of the peak (" +
                   std::to_string(db(slow.worst / slow.peak)) + " dB)");
    // An envelope's pace: a triangle of 1/25 octave per tick (60 octaves
    // a second), every step beyond blend_step, so exact throughout.
    const auto fast = blended_against_exact(model, [](std::size_t t) {
      const auto step = static_cast<double>(t % 50);
      return (step < 25.0 ? step : 50.0 - step) / 25.0 - 0.5;
    });
    REQUIRE_EQ(fast.worst, 0.0, describe(model) + " derives a fast sweep exactly");
  }
}

// ---- Drive --------------------------------------------------------------------------

void drive_stays_loudness_sane() {
  const double rate = oscillate::test::sample_rate_48k_hz;
  for (const double level_db : {-30.0, -18.0, -6.0, 0.0}) {
    const double amplitude = oscillate::native_dsp::dsp::db_to_gain(level_db);
    for (const double drive : {0.0, 6.0, 12.0, 18.0, 24.0}) {
      const auto s = settings(FilterModel::lp12, 22'000.0, 0.0, drive);
      const double change = db(sine_gain(s, rate, 200.0, amplitude, 0.1));
      require(change > -4.0 && change < 5.0,
              "drive " + std::to_string(drive) + " dB keeps a " + std::to_string(level_db) +
                  " dBFS sine within -4..+5 dB (" + std::to_string(change) + ")");
      if (drive == 0.0) {
        REQUIRE_LT(std::fabs(change), 0.05, "no drive is clean");
      }
    }
  }
  // Full drive on a full-scale sine adds harmonics.
  VoiceFilter filter;
  filter.prepare(rate);
  filter.set(settings(FilterModel::lp12, 22'000.0, 0.0, 24.0), 0);
  double sine = 0.0;
  double cosine = 0.0;
  double energy = 0.0;
  const int length = 48'000;
  for (int i = 0; i < length; ++i) {
    const double t = 2.0 * pi * 200.0 * i / rate;
    float left = static_cast<float>(std::sin(t));
    float right = left;
    filter.process(left, right);
    if (i >= 24'000) {
      sine += left * std::sin(t);
      cosine += left * std::cos(t);
      energy += static_cast<double>(left) * left;
    }
  }
  const double n = 24'000.0;
  const double fundamental = 2.0 * (sine * sine + cosine * cosine) / n;
  REQUIRE_GT((energy - fundamental) / energy, 0.01, "full drive saturates");
}

// ---- Stereo and real time -----------------------------------------------------------

void channels_are_independent() {
  for (std::size_t m = 0; m < filter_model_count; ++m) {
    const auto model = static_cast<FilterModel>(m);
    VoiceFilter stereo;
    stereo.prepare(48'000.0);
    stereo.set(settings(model, 900.0, 0.6, 6.0, 0.4), 0);
    double right_peak = 0.0;
    for (int i = 0; i < 9'600; ++i) {
      float left = static_cast<float>(0.5 * std::sin(i * 0.05));
      float right = 0.0F;
      stereo.process(left, right);
      right_peak = std::max(right_peak, static_cast<double>(std::fabs(right)));
    }
    REQUIRE_LT(right_peak, 1e-5, describe(model) + " keeps a silent channel silent");
  }
}

void processing_never_allocates() {
  VoiceFilter filter;
  filter.prepare(192'000.0);
  filter.set(settings(FilterModel::ladder24, 1'000.0, 0.5), 0);
  counting_allocations = true;
  counted_allocations = 0;
  double sink = 0.0;
  for (int i = 0; i < 22 * 2'048; ++i) {
    if (i % 2'048 == 0) {
      filter.set(settings(static_cast<FilterModel>(static_cast<std::size_t>(i / 2'048) %
                                                   filter_model_count),
                          700.0, 0.7, 10.0, 0.5),
                 64);
    }
    float left = static_cast<float>(std::sin(i * 0.01));
    float right = left;
    filter.process(left, right);
    sink += left + right;
  }
  filter.reset();
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "set, process and reset never allocate");
  require(std::isfinite(sink), "the chain stays finite");
  REQUIRE_EQ(filter_model_name(FilterModel::comb_minus), "Comb −",
             "models are named as the panel names them");
  REQUIRE_EQ(filter_model_name(FilterModel::ring), "Ring",
             "models are named as the panel names them");
}

} // namespace

int main() {
  try {
    clean_and_ladder_curves_match_their_prototypes();
    responses_match_analytic_curves();
    ladders_self_oscillate_in_tune();
    combs_peak_on_multiples_of_the_cutoff();
    formants_move_with_fat_and_cutoff();
    random_settings_and_sweeps_stay_finite();
    resonance_never_blows_up();
    model_switches_crossfade_without_steps();
    block_split_independence();
    ramps_reach_their_targets_linearly();
    banked_models_blend_within_tolerance();
    drive_stays_loudness_sane();
    channels_are_independent();
    processing_never_allocates();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "Nova filter contract passed\n";
  return 0;
}
