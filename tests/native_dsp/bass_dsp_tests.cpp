#include "oscillate/native_dsp/synth/bass_dsp.hpp"
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
#include <set>
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

// Independent oracle: the analog diode ladder's closed-loop response at s,
// normalised to wc = 1, by Gaussian elimination on (sI - A) y = e1.
Complex analog_ladder(Complex s, double k) {
  const std::array<std::array<double, 4>, 4> a{{
      {-1.0, 1.0, 0.0, 0.0},
      {0.5, -1.0, 0.5, 0.0},
      {0.0, 0.5, -1.0, 0.5},
      {0.0, 0.0, 0.5, -1.0},
  }};
  std::array<std::array<Complex, 5>, 4> m{};
  for (std::size_t r = 0; r < 4; ++r) {
    for (std::size_t c = 0; c < 4; ++c) {
      m[r][c] = (r == c ? s : Complex{}) - a[r][c];
    }
    m[r][4] = r == 0 ? 1.0 : 0.0;
  }
  for (std::size_t p = 0; p < 4; ++p) {
    for (std::size_t r = p + 1; r < 4; ++r) {
      const Complex f = m[r][p] / m[p][p];
      for (std::size_t c = p; c < 5; ++c) {
        m[r][c] -= f * m[p][c];
      }
    }
  }
  std::array<Complex, 4> y{};
  for (std::size_t r = 4; r-- > 0;) {
    Complex sum = m[r][4];
    for (std::size_t c = r + 1; c < 4; ++c) {
      sum -= m[r][c] * y[c];
    }
    y[r] = sum / m[r][r];
  }
  const Complex open = y[3];
  return open / (1.0 + k * open);
}

double sine_gain(DiodeLadder& ladder, const DiodeLadderCoefficients& c, double hz, double rate) {
  ladder.reset();
  constexpr double amplitude = 1e-4; // Small signal: the input stage is linear.
  const auto settle = static_cast<std::size_t>(rate * 0.5);
  const double cycles = std::max(20.0, std::floor(hz * 0.2));
  const auto length = static_cast<std::size_t>(std::round(cycles * rate / hz));
  double re = 0.0;
  double im = 0.0;
  for (std::size_t i = 0; i < settle + length; ++i) {
    const double t = 2.0 * pi * hz * static_cast<double>(i) / rate;
    const double y = ladder.process(amplitude * std::sin(t), c);
    if (i >= settle) {
      re += y * std::cos(t);
      im += y * std::sin(t);
    }
  }
  return 2.0 * std::hypot(re, im) / static_cast<double>(length) / amplitude;
}

void diode_ladder_matches_the_analog_model() {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  DiodeLadder ladder;
  for (double cutoff : {300.0, 1'500.0, 6'000.0}) {
    for (double resonance : {0.0, 0.5, 0.9}) {
      const auto c = DiodeLadderCoefficients::make(cutoff, rate, resonance);
      // Independent constants: wc from the resonant ratio 1/sqrt(2), k from 17.
      const double g = std::tan(pi * cutoff * std::sqrt(2.0) / rate);
      const double k = 1.02 * 17.0 * resonance;
      for (double hz : {40.0, 120.0, 400.0, 1'000.0, 3'000.0, 8'000.0, 15'000.0, 19'000.0}) {
        const double omega = 2.0 * pi * hz / rate;
        const Complex s{0.0, std::tan(omega / 2.0) / g};
        const double expected = std::abs(analog_ladder(s, k)) * (1.0 + 0.3 * k);
        const double measured = sine_gain(ladder, c, hz, rate);
        REQUIRE_LT(std::fabs(20.0 * std::log10(measured / expected)), 0.5,
                   "diode ladder small-signal response matches the analog model");
      }
    }
  }
}

void diode_ladder_self_oscillates_at_the_analytic_pitch() {
  constexpr double rate = oscillate::test::sample_rate_48k_hz;
  for (double cutoff : {220.0, 880.0, 3'000.0}) {
    DiodeLadder ladder;
    const auto c = DiodeLadderCoefficients::make(cutoff, rate, 1.0);
    std::vector<double> out(96'000);
    for (std::size_t i = 0; i < out.size(); ++i) {
      out[i] = ladder.process(i == 0 ? 0.1 : 0.0, c);
    }
    double first = -1.0;
    double last = -1.0;
    int crossings = 0;
    for (std::size_t i = 48'001; i < out.size(); ++i) {
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
    REQUIRE_GT(crossings, 10, "full resonance self-oscillates");
    const double measured = rate * crossings / (last - first);
    // Analytic: the loop rings where the analog phase crosses -180 degrees,
    // Omega = 1/sqrt(2), mapped through the bilinear transform.
    const double g = std::tan(pi * cutoff * std::sqrt(2.0) / rate);
    const double expected = rate / pi * std::atan(g / std::sqrt(2.0));
    REQUIRE_LT(std::fabs(1'200.0 * std::log2(measured / expected)), 25.0,
               "self-oscillation within 25 cents of the analytic pitch");
  }
  // Just below the threshold, a kick dies away.
  DiodeLadder ladder;
  auto c = DiodeLadderCoefficients::make(880.0, rate, 0.96); // k = 16.6 < 17
  double tail = 0.0;
  for (int i = 0; i < 96'000; ++i) {
    const double y = ladder.process(i == 0 ? 0.1 : 0.0, c);
    if (i > 90'000) {
      tail = std::max(tail, std::fabs(y));
    }
  }
  REQUIRE_LT(tail, 1e-6, "below k = 17 the ladder rings down");
}

void accent_sweep_climbs_then_relaxes() {
  constexpr double rate = 1'500.0; // The voice runs it at control rate.
  AccentSweep sweep;
  sweep.prepare(rate);
  sweep.reset();
  const auto step = static_cast<int>(0.125 * rate);
  std::vector<double> peaks;
  for (int note = 0; note < 5; ++note) {
    double peak = 0.0;
    for (int i = 0; i < step; ++i) {
      const double env = std::exp(-static_cast<double>(i) / (0.2 * rate));
      peak = std::max(peak, sweep.next(env, true));
    }
    peaks.push_back(peak);
  }
  for (std::size_t i = 1; i < peaks.size(); ++i) {
    REQUIRE_GT(peaks[i], peaks[i - 1], "consecutive accents climb");
  }
  REQUIRE_LT(peaks.back(), 1.0, "the sweep stays below the envelope");
  for (int i = 0; i < static_cast<int>(rate); ++i) {
    (void)sweep.next(1.0, false);
  }
  REQUIRE_LT(sweep.value(), 0.05 * peaks.back(), "the sweep relaxes when accents stop");
  // Unaccented notes never charge it.
  sweep.reset();
  for (int i = 0; i < step; ++i) {
    (void)sweep.next(1.0, false);
  }
  REQUIRE_EQ(sweep.value(), 0.0, "unaccented notes leave the sweep at rest");
}

void square_tilt_is_an_exact_one_pole_high_pass() {
  SquareTilt tilt;
  const double c = 0.01;
  for (int n = 0; n < 200; ++n) {
    const double y = tilt.process(1.0, c);
    REQUIRE_LT(std::fabs(y - std::pow(1.0 - c, n + 1)), tolerance::double_rounding,
               "the top sags exponentially");
  }
}

std::vector<double> harmonics(BassDistortion& d, double rate, double hz, std::size_t count) {
  const auto settle = static_cast<std::size_t>(rate * 0.2);
  const auto length = static_cast<std::size_t>(std::round(40.0 * rate / hz));
  std::vector<double> re(count + 1);
  std::vector<double> im(count + 1);
  for (std::size_t i = 0; i < settle + length; ++i) {
    const double t = 2.0 * pi * hz * static_cast<double>(i) / rate;
    const double y = d.process(0.5 * std::sin(t));
    if (i >= settle) {
      for (std::size_t h = 1; h <= count; ++h) {
        re[h] += y * std::cos(static_cast<double>(h) * t);
        im[h] += y * std::sin(static_cast<double>(h) * t);
      }
    }
  }
  std::vector<double> out(count + 1);
  for (std::size_t h = 1; h <= count; ++h) {
    out[h] = 2.0 * std::hypot(re[h], im[h]) / static_cast<double>(length);
  }
  return out;
}

void distortion_modes_have_their_character() {
  constexpr double rate = oscillate::test::sample_rate_96k_hz;
  BassDistortion d;
  d.prepare(rate);
  d.set(BassDistortionMode::fuzz, 1.0, 1.0, 0.0);
  for (int i = 0; i < 100; ++i) {
    const double x = std::sin(i * 0.1);
    REQUIRE_EQ(d.process(x), x, "mix 0 is an exact bypass");
  }
  d.set(BassDistortionMode::tube, 0.5, 1.0, 1.0);
  d.reset();
  const auto tube = harmonics(d, rate, 110.0, 3);
  REQUIRE_GT(tube[2] / tube[1], 0.01, "tube asymmetry adds even harmonics");
  d.set(BassDistortionMode::fuzz, 0.8, 1.0, 1.0);
  d.reset();
  const auto fuzz = harmonics(d, rate, 110.0, 3);
  REQUIRE_LT(fuzz[2] / fuzz[1], 1e-4, "fuzz is symmetric: no even harmonics");
  REQUIRE_GT(fuzz[3] / fuzz[1], 0.1, "fuzz is rich in odd harmonics");
  // Full-drive crush with the tone open: 4 bits (8 steps per unit) held
  // for 24 samples.
  d.set(BassDistortionMode::crush, 1.0, 1.0, 1.0);
  d.reset();
  std::set<double> levels;
  double previous = 0.0;
  for (int i = 0; i < 20'000; ++i) {
    const double y = d.process(0.99 * std::sin(i * 0.002));
    levels.insert(y);
    REQUIRE_LT(std::fabs(y * 8.0 - std::round(y * 8.0)), tolerance::double_rounding,
               "crush lands on 4-bit levels");
    require(i == 0 || y == previous || i % 24 == 0, "crush only resamples every 24 samples");
    previous = y;
  }
  REQUIRE_EQ(levels.size(), 17, "four-bit crush spans 17 levels over [-1, 1]");
}

// A mode switch crossfades over 10 ms instead of stepping: around the switch
// no sample moves further than either mode moves on its own.
void distortion_mode_switches_crossfade() {
  constexpr double rate = oscillate::test::sample_rate_96k_hz;
  BassDistortion d;
  d.prepare(rate);
  d.set(BassDistortionMode::fuzz, 0.8, 1.0, 1.0);
  std::vector<double> y(20'000);
  for (std::size_t i = 0; i < y.size(); ++i) {
    if (i == 10'000) {
      d.set(BassDistortionMode::tube, 0.3, 1.0, 1.0);
    }
    y[i] = d.process(0.5 * std::sin(2.0 * pi * 110.0 * static_cast<double>(i) / rate));
  }
  const auto largest_step = [&](std::size_t from, std::size_t to) {
    double largest = 0.0;
    for (std::size_t i = from; i < to; ++i) {
      largest = std::max(largest, std::fabs(y[i] - y[i - 1]));
    }
    return largest;
  };
  const double steady = std::max(largest_step(1'000, 9'000), largest_step(12'000, 19'000));
  REQUIRE_LE(largest_step(9'990, 11'000), 1.05 * steady, "a distortion mode switch does not step");
}

struct Fired {
  std::uint64_t frame;
  SequencerEvent::Kind kind;
  int semitones;
  bool accent;
  friend bool operator==(const Fired&, const Fired&) = default;
};

std::vector<Fired> run(std::span<const SequencerStep> pattern, std::uint64_t frames,
                       std::uint32_t block, double fps, double swing, double gate,
                       std::uint64_t tempo_change_at = 0, double new_fps = 0.0) {
  StepSequencer seq;
  seq.set_timing(fps, swing, gate);
  seq.start();
  std::vector<Fired> fired;
  std::uint64_t position = 0;
  std::uint32_t cycle = 0;
  const std::uint32_t irregular[] = {37, 100, 513, 64, 1, 255};
  while (position < frames) {
    std::uint32_t size = block == 0 ? irregular[cycle++ % 6] : block;
    size = static_cast<std::uint32_t>(std::min<std::uint64_t>(size, frames - position));
    if (tempo_change_at > position && tempo_change_at < position + size) {
      size = static_cast<std::uint32_t>(tempo_change_at - position);
    }
    if (tempo_change_at != 0 && position == tempo_change_at) {
      seq.set_timing(new_fps, swing, gate);
    }
    std::uint32_t done = 0;
    while (auto event = seq.peek(pattern, size - done)) {
      seq.advance(event->offset);
      done += event->offset;
      seq.consume(pattern);
      if (!(event->kind == SequencerEvent::Kind::note_off && fired.empty())) {
        fired.push_back({position + done, event->kind, event->semitones, event->accent});
      }
    }
    seq.advance(size - done);
    position += size;
  }
  return fired;
}

void sequencer_steps_land_on_exact_frames() {
  using K = SequencerEvent::Kind;
  // 120 BPM at 48 kHz in sixteenths: 6000 frames per step.
  const std::array<SequencerStep, 4> plain{{{0, 0, StepGate::on, true, false},
                                            {3, 0, StepGate::on, false, false},
                                            {7, 1, StepGate::on, false, false},
                                            {0, -1, StepGate::on, false, false}}};
  const auto x = run(plain, 24'000, 128, 6'000.0, 0.0, 0.5);
  const std::vector<Fired> expected{
      {0, K::note_on, 0, true},         {3'000, K::note_off, 0, false},
      {6'000, K::note_on, 3, false},    {9'000, K::note_off, 0, false},
      {12'000, K::note_on, 19, false},  {15'000, K::note_off, 0, false},
      {18'000, K::note_on, -12, false}, {21'000, K::note_off, 0, false}};
  REQUIRE_EQ(x, expected, "onsets every 6000 frames, gates close at half a step");

  // Swing 0.5 delays odd steps by a quarter step.
  const auto swung = run(plain, 24'000, 64, 6'000.0, 0.5, 0.5);
  REQUIRE_EQ(swung[2].frame, 7'500, "swing delays odd steps by swing / 2 of a step");
  REQUIRE_EQ(swung[6].frame, 19'500, "swing delays odd steps by swing / 2 of a step");
  REQUIRE_EQ(swung[4].frame, 12'000, "swing delays odd steps by swing / 2 of a step");

  // Slide holds the gate and glides; a tie keeps the pitch; a rest is silent.
  const std::array<SequencerStep, 4> phrase{{{0, 0, StepGate::on, false, true},
                                             {5, 0, StepGate::on, false, false},
                                             {0, 0, StepGate::tie, false, false},
                                             {0, 0, StepGate::off, false, false}}};
  const auto y = run(phrase, 24'000, 128, 6'000.0, 0.0, 0.5);
  const std::vector<Fired> phrased{{0, K::note_on, 0, false},
                                   {6'000, K::glide, 5, false},
                                   {12'000, K::glide, 5, false},
                                   {15'000, K::note_off, 0, false},
                                   {18'000, K::note_off, 0, false}};
  REQUIRE_EQ(y, phrased, "slide glides, tie holds the pitch, rest stays silent");

  // Any block split fires the same events at the same frames.
  for (std::uint32_t block : {1U, 37U, 512U, 0U}) {
    REQUIRE_EQ(run(plain, 48'000, block, 6'000.0, 0.5, 0.3),
               run(plain, 48'000, 128, 6'000.0, 0.5, 0.3),
               "sequencer events are block-split invariant");
  }

  // A tempo change rebases: at frame 9000 (step 1.5) the step halves, so
  // step 2 lands at 9000 + 0.5 * 3000.
  const auto faster = run(plain, 16'000, 128, 6'000.0, 0.0, 0.5, 9'000, 3'000.0);
  REQUIRE_EQ(faster[4].frame, 10'500, "tempo change rebases");
  REQUIRE_EQ(faster[4].kind, K::note_on, "tempo change rebases");

  // A pattern shorter than 16 wraps.
  const auto wrapped = run(std::span(plain).first(3), 24'000, 128, 6'000.0, 0.0, 0.5);
  REQUIRE_EQ(wrapped[6].frame, 18'000, "length 3 wraps to step 0");
  REQUIRE_EQ(wrapped[6].semitones, 0, "length 3 wraps to step 0");
}

void processing_does_not_allocate() {
  DiodeLadder ladder;
  const auto c = DiodeLadderCoefficients::make(1'000.0, 96'000.0, 0.8);
  BassDistortion d;
  d.prepare(96'000.0);
  d.set(BassDistortionMode::tube, 0.6, 0.5, 1.0);
  AccentSweep sweep;
  sweep.prepare(1'500.0);
  StepSequencer seq;
  const std::array<SequencerStep, 2> pattern{};
  seq.start();
  counting_allocations = true;
  counted_allocations = 0;
  double sink = 0.0;
  for (int i = 0; i < 10'000; ++i) {
    sink += d.process(ladder.process(std::sin(i * 0.01), c)) + sweep.next(0.5, true);
    if (auto e = seq.peek(pattern, 1)) {
      seq.consume(pattern);
      sink += e->semitones;
    }
    seq.advance(1);
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "bass primitives never allocate");
  REQUIRE_TRUE(std::isfinite(sink), "bass primitives never allocate");
}

} // namespace

int main() {
  try {
    diode_ladder_matches_the_analog_model();
    diode_ladder_self_oscillates_at_the_analytic_pitch();
    accent_sweep_climbs_then_relaxes();
    square_tilt_is_an_exact_one_pole_high_pass();
    distortion_modes_have_their_character();
    distortion_mode_switches_crossfade();
    sequencer_steps_land_on_exact_frames();
    processing_does_not_allocate();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "bass DSP contract passed\n";
  return 0;
}
