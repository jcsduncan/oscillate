#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/grain_dsp.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
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
constexpr double rate = oscillate::test::sample_rate_48k_hz;

using oscillate::test::require;

// Writes `frames` of source and renders the cloud alongside, live.
std::vector<double> run_live(GrainCloud& cloud, GrainBuffer& buffer, std::size_t frames,
                             const auto& source) {
  std::vector<double> left(frames);
  std::vector<double> right(frames);
  for (std::size_t start = 0; start < frames; start += 32) {
    const std::size_t n = std::min<std::size_t>(32, frames - start);
    const auto before = buffer.count();
    for (std::size_t i = 0; i < n; ++i) {
      buffer.write(source(before + i));
    }
    cloud.render(buffer, false, before, left.data() + start, right.data() + start, n);
  }
  return left;
}

std::vector<double> run_frozen(GrainCloud& cloud, const GrainBuffer& buffer, std::size_t frames) {
  std::vector<double> left(frames);
  std::vector<double> right(frames);
  for (std::size_t start = 0; start < frames; start += 32) {
    const std::size_t n = std::min<std::size_t>(32, frames - start);
    cloud.render(buffer, true, buffer.count(), left.data() + start, right.data() + start, n);
  }
  return left;
}

void hann_grains_overlap_add_to_a_constant() {
  GrainBuffer buffer;
  buffer.prepare(4 * 48'000);
  GrainCloud cloud;
  cloud.reset(7);
  GrainSettings s;
  s.size_frames = 960;
  s.interval_frames = 480; // Overlap 2.
  s.lag_frames = 4'800;
  cloud.set(s);
  const auto out = run_live(cloud, buffer, 24'000, [](std::uint64_t) { return 1.0; });
  for (std::size_t i = 12'000; i < 24'000; ++i) {
    REQUIRE_LT(std::fabs(out[i] - 1.0), tolerance::double_accumulation,
               "Hann grains at overlap 2 sum to a constant");
  }
}

void frozen_sine(GrainBuffer& buffer, double hz) {
  buffer.prepare(4 * 48'000);
  for (std::size_t i = 0; i < 3 * 48'000; ++i) {
    buffer.write(std::sin(2.0 * pi * hz * static_cast<double>(i) / rate));
  }
}

double crossing_hz(const std::vector<double>& x, std::size_t from, std::size_t to) {
  double first = -1.0;
  double last = -1.0;
  int count = 0;
  for (std::size_t i = from + 1; i < to; ++i) {
    if (x[i - 1] < 0.0 && x[i] >= 0.0) {
      const double t = static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
      if (first < 0.0) {
        first = t;
      } else {
        ++count;
      }
      last = t;
    }
  }
  return rate * count / (last - first);
}

void grains_play_at_their_rate() {
  GrainBuffer buffer;
  frozen_sine(buffer, 220.0);
  for (double r : {0.5, 1.5, 2.0}) {
    GrainCloud cloud;
    cloud.reset(3);
    GrainSettings s;
    s.size_frames = 24'000;
    s.interval_frames = 1e9; // One grain.
    s.shape = GrainShape::tukey;
    s.scan = 0.0;
    s.lag_frames = 60'000;
    s.rate = r;
    cloud.set(s);
    const auto out = run_frozen(cloud, buffer, 24'000);
    const double measured = crossing_hz(out, 7'000, 17'000);
    REQUIRE_LT(std::fabs(1'200.0 * std::log2(measured / (220.0 * r))), 0.5,
               "a grain at rate r plays r f within 0.5 cent");
  }
}

double level_at(const std::vector<double>& y, std::size_t from, std::size_t length, double hz) {
  std::complex<double> sum{};
  for (std::size_t i = from; i < from + length; ++i) {
    const double t = static_cast<double>(i - from);
    const double hann = oscillate::native_dsp::dsp::hann(t, static_cast<double>(length));
    sum += y[i] * hann * std::polar(1.0, -2.0 * pi * hz * static_cast<double>(i) / rate);
  }
  return 4.0 * std::abs(sum) / static_cast<double>(length); // Hann coherent gain 1/2.
}

// A grain an octave up reads the half-rate level: a 19.2 kHz source tone,
// which would play at 38.4 kHz and fold to 9.6 kHz, is simply absent.
void pitched_up_grains_do_not_alias() {
  GrainBuffer buffer;
  frozen_sine(buffer, 0.4 * rate);
  GrainCloud cloud;
  cloud.reset(17);
  GrainSettings s;
  s.size_frames = 24'000;
  s.interval_frames = 1e9;
  s.shape = GrainShape::tukey;
  s.scan = 0.0;
  s.lag_frames = 70'000;
  s.rate = 2.0;
  cloud.set(s);
  const auto out = run_frozen(cloud, buffer, 24'000);
  const double folded = level_at(out, 7'000, 10'000, 0.2 * rate);
  REQUIRE_LT(20.0 * std::log10(folded + 1e-30), -70.0,
             "an octave-up grain leaves no folded image of a near-Nyquist tone");
}

void reverse_grains_read_backwards() {
  GrainBuffer buffer;
  buffer.prepare(4 * 48'000);
  for (std::size_t i = 0; i < 3 * 48'000; ++i) {
    // A ramp in steps of 2^-17: exact in float, so reads are exact too.
    buffer.write(static_cast<double>(i) / 131'072.0);
  }
  GrainCloud cloud;
  cloud.reset(5);
  GrainSettings s;
  s.size_frames = 8'000;
  s.interval_frames = 1e9;
  s.shape = GrainShape::tukey;
  s.scan = 0.0;
  s.reverse_probability = 1.0;
  s.lag_frames = 50'000;
  cloud.set(s);
  const auto out = run_frozen(cloud, buffer, 8'000);
  const double gain = GrainCloud::density_gain(s);
  for (std::size_t i = 2'500; i < 5'500; ++i) {
    REQUIRE_LT(std::fabs((out[i + 1] - out[i]) + gain / 131'072.0), tolerance::double_rounding,
               "a reversed grain reads the ramp backwards, sample for sample");
  }
}

// Early in a note a reversed grain lacks history; it plays forwards instead
// of dropping, so the note speaks at once.
void early_reverse_grains_fall_back_to_forward() {
  GrainBuffer buffer;
  buffer.prepare(4 * 48'000);
  GrainCloud cloud;
  cloud.reset(13);
  GrainSettings s;
  s.size_frames = 19'200; // 0.4 s: a reversed grain needs that much history.
  s.interval_frames = 960;
  s.reverse_probability = 1.0;
  s.lag_frames = 28'800;
  cloud.set(s);
  const auto out = run_live(cloud, buffer, 4'800, [](std::uint64_t) { return 0.5; });
  double level = 0.0;
  for (double x : out) {
    level = std::max(level, std::fabs(x));
  }
  REQUIRE_GT(level, 0.01, "the first grain sounds before a reversed one could fit");
}

void a_frozen_buffer_repeats_with_the_grain_period() {
  GrainBuffer buffer;
  buffer.prepare(4 * 48'000);
  std::uint32_t state = 12'345;
  for (std::size_t i = 0; i < 2 * 48'000; ++i) {
    state = state * 1'664'525U + 1'013'904'223U;
    buffer.write(static_cast<double>(state) / 4'294'967'296.0 - 0.5);
  }
  GrainCloud cloud;
  cloud.reset(9);
  GrainSettings s;
  s.size_frames = 2'400;
  s.interval_frames = 1'200;
  s.scan = 0.0;
  s.lag_frames = 30'000;
  cloud.set(s);
  const auto out = run_frozen(cloud, buffer, 20'000);
  for (std::size_t i = 6'000; i < 18'000; ++i) {
    REQUIRE_LT(std::fabs(out[i + 1'200] - out[i]), tolerance::double_accumulation,
               "a frozen moment repeats exactly");
  }
}

// Frozen, sprayed grains near "now" wrap round the held moment instead of
// piling onto its newest edge: every grain reads a different place.
void frozen_grains_wrap_instead_of_piling() {
  GrainBuffer buffer;
  buffer.prepare(4 * 48'000);
  for (std::size_t i = 0; i < 3 * 48'000; ++i) {
    buffer.write(static_cast<double>(i) / 131'072.0);
  }
  GrainCloud cloud;
  cloud.reset(21);
  GrainSettings s;
  s.size_frames = 2'400;
  s.interval_frames = 12'000;
  s.shape = GrainShape::tukey;
  s.scan = 0.0;
  s.lag_frames = 0.0;        // Centred on "now"...
  s.spray_frames = 24'000.0; // ...with half a second of spray either way.
  cloud.set(s);
  const auto out = run_frozen(cloud, buffer, 96'000);
  std::vector<double> places;
  for (std::size_t spawn = 0; spawn + 2'400 < out.size(); spawn += 12'000) {
    places.push_back(out[spawn + 1'200] * 131'072.0);
  }
  std::sort(places.begin(), places.end());
  for (std::size_t i = 1; i < places.size(); ++i) {
    REQUIRE_GT(places[i] - places[i - 1], 48.0, "frozen grains do not pile onto one place");
  }
}

void synced_grains_land_on_exact_frames() {
  GrainBuffer buffer;
  buffer.prepare(4 * 48'000);
  for (int i = 0; i < 48'000; ++i) {
    buffer.write(0.1);
  }
  GrainCloud cloud;
  cloud.reset(1);
  GrainSettings s;
  s.size_frames = 100;
  s.synced = true;
  s.frames_per_division = 6'000.0; // 120 BPM sixteenths.
  s.scan = 0.0;
  s.lag_frames = 10'000;
  cloud.set(s);
  std::vector<std::uint64_t> onsets;
  std::array<double, 1> l{};
  std::array<double, 1> r{};
  std::size_t active = 0;
  for (std::uint64_t frame = 0; frame < 30'000; ++frame) {
    if (frame == 15'000) {
      // A tempo change rebases: at step 2.5 the division halves, so step 3
      // lands at 15000 + 0.5 * 3000.
      s.frames_per_division = 3'000.0;
      cloud.set(s);
    }
    cloud.render(buffer, true, buffer.count(), l.data(), r.data(), 1);
    if (cloud.active() > active) {
      onsets.push_back(frame);
    }
    active = cloud.active();
  }
  REQUIRE_GE(onsets.size(), 5, "synced grains spawn on exact frames and rebase on a tempo change");
  REQUIRE_EQ(onsets[0], 0, "synced grains spawn on exact frames and rebase on a tempo change");
  REQUIRE_EQ(onsets[1], 6'000, "synced grains spawn on exact frames and rebase on a tempo change");
  REQUIRE_EQ(onsets[2], 12'000, "synced grains spawn on exact frames and rebase on a tempo change");
  REQUIRE_EQ(onsets[3], 16'500, "synced grains spawn on exact frames and rebase on a tempo change");
  REQUIRE_EQ(onsets[4], 19'500, "synced grains spawn on exact frames and rebase on a tempo change");
}

double peak_frequency(const std::vector<double>& x, double guess) {
  const auto magnitude = [&](double hz) {
    std::complex<double> sum{};
    const auto n = static_cast<double>(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
      const double t = static_cast<double>(i);
      const double hann = oscillate::native_dsp::dsp::hann(t, n);
      sum += x[i] * hann * std::polar(1.0, -2.0 * pi * hz * t / rate);
    }
    return std::abs(sum);
  };
  double low = guess * std::exp2(-5.0 / 1'200.0);
  double high = guess * std::exp2(5.0 / 1'200.0);
  for (int i = 0; i < 40; ++i) {
    const double a = low + (high - low) * 0.382;
    const double b = low + (high - low) * 0.618;
    (magnitude(a) < magnitude(b) ? low : high) = magnitude(a) < magnitude(b) ? a : b;
  }
  return 0.5 * (low + high);
}

void partials_follow_the_stiff_string_law() {
  PartialBank bank;
  bank.reset();
  constexpr double f = 110.0;
  constexpr double stretch = 0.0004;
  bank.set(f / rate, 0.0, 0.0, stretch);
  std::vector<double> x(32'768);
  for (double& sample : x) {
    sample = bank.next();
  }
  for (std::size_t n : {std::size_t{1}, std::size_t{5}, std::size_t{12}}) {
    const double expected = f * PartialBank::ratio(n, stretch);
    const double independent =
        f * static_cast<double>(n) * std::sqrt(1.0 + stretch * static_cast<double>(n * n));
    REQUIRE_LT(std::fabs(expected / independent - 1.0), tolerance::double_rounding,
               "ratio matches the law");
    const double measured = peak_frequency(x, expected);
    REQUIRE_LT(std::fabs(1'200.0 * std::log2(measured / independent)), 0.5,
               "partial n sits at n f sqrt(1 + B n^2) within 0.5 cent");
  }
}

void octave_shifter_doubles_the_pitch() {
  OctaveShifter shifter;
  shifter.prepare(rate);
  std::vector<double> y(48'000);
  for (std::size_t i = 0; i < y.size(); ++i) {
    y[i] = shifter.process(std::sin(2.0 * pi * 440.0 * static_cast<double>(i) / rate));
  }
  const auto level = [&](double hz) {
    std::complex<double> sum{};
    for (std::size_t i = 4'800; i < 4'800 + 38'400; ++i) {
      sum += y[i] * std::polar(1.0, -2.0 * pi * hz * static_cast<double>(i) / rate);
    }
    return std::abs(sum);
  };
  REQUIRE_LT(20.0 * std::log10(level(440.0) / level(880.0)), -30.0,
             "the shifter moves 440 Hz to 880 Hz, leaving the original below -30 dB");
}

// Doubling a 16.8 kHz tone would fold 33.6 kHz to 14.4 kHz; the shifter's
// input band-limit removes it first.
void octave_shifter_does_not_alias() {
  OctaveShifter shifter;
  shifter.prepare(rate);
  std::vector<double> y(48'000);
  for (std::size_t i = 0; i < y.size(); ++i) {
    y[i] = shifter.process(std::sin(2.0 * pi * 0.35 * static_cast<double>(i)));
  }
  REQUIRE_LT(20.0 * std::log10(level_at(y, 9'600, 32'768, 0.3 * rate) + 1e-30), -60.0,
             "the shifter leaves no folded image of a high tone");
}

void fdn_decays_at_its_rt60_and_stays_bounded() {
  for (double decay : {1.0, 3.0}) {
    ShimmerReverb reverb;
    reverb.prepare(rate);
    reverb.set(0.5, decay, 0.0, 0.0, 1.0);
    const auto frames = static_cast<std::size_t>(rate * decay * 1.2);
    std::vector<double> energy(frames);
    for (std::size_t i = 0; i < frames; ++i) {
      const auto out = reverb.process(i == 0 ? 1.0 : 0.0, i == 0 ? 1.0 : 0.0);
      energy[i] = out.left * out.left + out.right * out.right;
    }
    // Schroeder backward integration, then the slope from -5 to -25 dB.
    std::vector<double> integral(frames);
    double sum = 0.0;
    for (std::size_t i = frames; i-- > 0;) {
      sum += energy[i];
      integral[i] = sum;
    }
    const auto db_at = [&](std::size_t i) { return 10.0 * std::log10(integral[i] / integral[0]); };
    std::size_t a = 0;
    while (db_at(a) > -5.0) {
      ++a;
    }
    std::size_t b = a;
    while (db_at(b) > -25.0) {
      ++b;
    }
    const double rt60 = 60.0 / ((db_at(a) - db_at(b)) / (static_cast<double>(b - a) / rate));
    REQUIRE_LT(std::fabs(rt60 / decay - 1.0), 0.1, "the FDN decays at its RT60 within 10 %");
  }
  ShimmerReverb loud;
  loud.prepare(rate);
  loud.set(1.0, 30.0, 0.0, 1.0, 1.0);
  std::uint32_t state = 1;
  double peak = 0.0;
  for (int i = 0; i < 10 * 48'000; ++i) {
    state = state * 1'664'525U + 1'013'904'223U;
    const double noise = i < 96'000 ? static_cast<double>(state) / 4'294'967'296.0 - 0.5 : 0.0;
    const auto out = loud.process(noise, -noise);
    REQUIRE_TRUE(std::isfinite(out.left), "the FDN stays finite");
    REQUIRE_TRUE(std::isfinite(out.right), "the FDN stays finite");
    peak = std::max({peak, std::fabs(out.left), std::fabs(out.right)});
  }
  REQUIRE_LT(peak, 20.0, "full shimmer with a 30 s decay stays bounded");
}

void processing_does_not_allocate() {
  GrainBuffer buffer;
  buffer.prepare(4 * 48'000);
  GrainCloud cloud;
  cloud.reset(11);
  GrainSettings s;
  s.size_frames = 4'800;
  s.interval_frames = 240;
  s.chaos = 0.5;
  s.spray_frames = 2'000;
  s.intervals = GrainIntervals::pentatonic;
  s.interval_probability = 0.5;
  s.jitter_cents = 30;
  s.reverse_probability = 0.3;
  s.spread = 1.0;
  cloud.set(s);
  PartialBank bank;
  bank.reset();
  bank.set(0.005, 1.0, 0.3, 0.001);
  ShimmerReverb reverb;
  reverb.prepare(rate);
  reverb.set(0.7, 5.0, 0.4, 0.6, 0.5);
  std::array<double, 32> left{};
  std::array<double, 32> right{};
  double sink = 0.0;
  counting_allocations = true;
  counted_allocations = 0;
  for (int chunk = 0; chunk < 750; ++chunk) {
    const auto before = buffer.count();
    for (std::size_t i = 0; i < 32; ++i) {
      buffer.write(bank.next());
    }
    left.fill(0.0);
    right.fill(0.0);
    cloud.render(buffer, false, before, left.data(), right.data(), 32);
    for (std::size_t i = 0; i < 32; ++i) {
      sink += reverb.process(left[i], right[i]).left;
    }
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "grain primitives never allocate");
  REQUIRE_TRUE(std::isfinite(sink), "grain primitives never allocate");
}

} // namespace

int main() {
  try {
    hann_grains_overlap_add_to_a_constant();
    grains_play_at_their_rate();
    pitched_up_grains_do_not_alias();
    reverse_grains_read_backwards();
    early_reverse_grains_fall_back_to_forward();
    a_frozen_buffer_repeats_with_the_grain_period();
    frozen_grains_wrap_instead_of_piling();
    synced_grains_land_on_exact_frames();
    partials_follow_the_stiff_string_law();
    octave_shifter_doubles_the_pitch();
    octave_shifter_does_not_alias();
    fdn_decays_at_its_rt60_and_stays_bounded();
    processing_does_not_allocate();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "grain DSP contract passed\n";
  return 0;
}
