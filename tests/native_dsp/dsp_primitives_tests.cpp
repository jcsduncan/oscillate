#include "dsp/biquad.hpp"
#include "dsp/fft.hpp"
#include "dsp/ramp.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

// The shared DSP primitives against independent oracles: the ramp lands on
// its targets; the RBJ allpass has unity magnitude and the shelves their
// exact end gains, by direct evaluation of the transfer function and by
// filtering; the FFT matches a naive DFT and inverts exactly.
namespace {
namespace dsp = oscillate::native_dsp::dsp;
using Complex = std::complex<double>;

using oscillate::test::require;

void ramp_lands_on_its_targets() {
  dsp::Ramp ramp;
  ramp.to(1.0, 64.0, true);
  REQUIRE_EQ(ramp.value, 1.0, "a jump lands at once");
  REQUIRE_EQ(ramp.step, 0.0, "a jump lands at once");
  ramp.to(-0.5, 0.0, false);
  REQUIRE_EQ(ramp.value, -0.5, "no frames is a jump");
  ramp.to(0.25, 100.0, false);
  for (int i = 0; i < 100; ++i) {
    (void)ramp.next();
  }
  REQUIRE_LT(std::fabs(ramp.value - 0.25), 1e-15, "a ramp reaches its target in its frames");
  ramp.settle(0.25, 100.0, false);
  REQUIRE_EQ(ramp.value, 0.25, "settle snaps onto a target it is at");
  REQUIRE_EQ(ramp.step, 0.0, "settle snaps onto a target it is at");
  ramp.settle(1.0, 3.0, false);
  std::vector<double> filled(3);
  dsp::Ramp copy = ramp;
  ramp.fill(filled.data(), filled.size());
  for (const double value : filled) {
    REQUIRE_EQ(value, copy.next(), "fill equals next() frame by frame");
  }
  dsp::Ramp skipped;
  skipped.to(2.0, 8.0, false);
  skipped.skip(8);
  REQUIRE_LT(std::fabs(skipped.value - 2.0), 1e-15, "skip advances by whole frames");
  REQUIRE_TRUE(dsp::Ramp{}.silent(), "silent means at zero and still");
  REQUIRE_TRUE(!skipped.silent(), "silent means at zero and still");
}

Complex response(const dsp::BiquadCoefficients& c, double w) {
  const Complex z = std::polar(1.0, -w);
  return (c.b0 + c.b1 * z + c.b2 * z * z) / (1.0 + c.a1 * z + c.a2 * z * z);
}

// The magnitude at `hz` by filtering a sine and reading its settled peak.
double filtered_gain(const dsp::BiquadCoefficients& c, double hz, double rate) {
  dsp::Biquad filter;
  filter.set(c);
  double top = 0.0;
  const std::size_t frames = 96'000;
  for (std::size_t i = 0; i < frames; ++i) {
    const double y =
        filter.process(std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate));
    if (i > frames / 2) {
      top = std::max(top, std::fabs(y));
    }
  }
  return top;
}

void allpass_is_flat() {
  for (const double rate : {44'100.0, 48'000.0, 96'000.0}) {
    const auto c = dsp::rbj_allpass({.hz = 150.0, .rate = rate}, 0.7);
    REQUIRE_EQ(c.b2, 1.0, "the allpass is mirror-symmetric");
    REQUIRE_EQ(c.a2, c.b0, "the allpass is mirror-symmetric");
    REQUIRE_EQ(c.a1, c.b1, "the allpass is mirror-symmetric");
    for (double hz = 10.0; hz < 0.49 * rate; hz *= 1.5) {
      const double w = 2.0 * std::numbers::pi * hz / rate;
      REQUIRE_LT(std::fabs(std::abs(response(c, w)) - 1.0), tolerance::double_rounding,
                 "the allpass has unity magnitude");
    }
    REQUIRE_LT(std::fabs(filtered_gain(c, 1'000.0, rate) - 1.0), tolerance::coarse,
               "filtering through it keeps a sine's level");
    // The phase passes -180 degrees at the centre.
    const Complex centre = response(c, 2.0 * std::numbers::pi * 150.0 / rate);
    REQUIRE_GT(std::fabs(std::arg(centre)), std::numbers::pi - 1e-9,
               "the phase is -180 at the centre");
  }
}

void shelves_hit_their_end_gains() {
  for (const double rate : {44'100.0, 48'000.0}) {
    for (const double gain : {0.25, 0.5, 2.0}) {
      const auto low = dsp::rbj_shelf(false, gain, {.hz = 300.0, .rate = rate});
      const auto high = dsp::rbj_shelf(true, gain, {.hz = 3'000.0, .rate = rate});
      REQUIRE_LT(std::fabs(std::abs(response(low, 0.0)) - gain), tolerance::double_rounding,
                 "the low shelf's DC gain");
      REQUIRE_LT(std::fabs(std::abs(response(low, std::numbers::pi)) - 1.0),
                 tolerance::double_rounding, "the low shelf is unity at Nyquist");
      REQUIRE_LT(std::fabs(std::abs(response(high, std::numbers::pi)) - gain),
                 tolerance::double_rounding, "the high shelf's Nyquist gain");
      REQUIRE_LT(std::fabs(std::abs(response(high, 0.0)) - 1.0), tolerance::double_rounding,
                 "the high shelf is unity at DC");
      REQUIRE_LT(std::fabs(filtered_gain(low, 20.0, rate) - gain), 0.01 * gain,
                 "a low sine through the low shelf takes its gain");
    }
  }
}

std::vector<Complex> naive_dft(const std::vector<Complex>& x, double sign) {
  const std::size_t n = x.size();
  std::vector<Complex> out(n);
  for (std::size_t k = 0; k < n; ++k) {
    Complex sum{};
    for (std::size_t t = 0; t < n; ++t) {
      // Reduce k t mod n first so the angle stays accurate.
      const double angle =
          sign * 2.0 * std::numbers::pi * static_cast<double>((k * t) % n) / static_cast<double>(n);
      sum += x[t] * std::polar(1.0, angle);
    }
    out[k] = sum;
  }
  return out;
}

void fft_matches_a_naive_dft() {
  std::uint32_t state = 12'345;
  const auto random = [&] {
    state = state * 1'664'525U + 1'013'904'223U;
    return static_cast<double>(state >> 8U) / 8'388'608.0 - 1.0;
  };
  for (std::size_t n = 1; n <= 2'048; n *= 2) {
    std::vector<Complex> x(n);
    for (auto& value : x) {
      value = {random(), random()};
    }
    const dsp::Fft fft(n);
    REQUIRE_EQ(fft.size(), n, "the size is kept");
    auto forward = x;
    fft.forward(forward);
    const auto expected = naive_dft(x, -1.0);
    double error = 0.0;
    double energy_time = 0.0;
    double energy_frequency = 0.0;
    for (std::size_t k = 0; k < n; ++k) {
      error = std::max(error, std::abs(forward[k] - expected[k]));
      energy_time += std::norm(x[k]);
      energy_frequency += std::norm(forward[k]);
    }
    REQUIRE_LT(error, 1e-12 * static_cast<double>(n),
               "forward matches the DFT: " + std::to_string(n));
    REQUIRE_LT(std::fabs(energy_frequency - static_cast<double>(n) * energy_time),
               tolerance::double_accumulation * energy_frequency + 1e-12, "Parseval holds");
    auto back = forward;
    fft.inverse(back);
    double round_trip = 0.0;
    for (std::size_t k = 0; k < n; ++k) {
      round_trip = std::max(round_trip, std::abs(back[k] - x[k]));
    }
    REQUIRE_LT(round_trip, 1e-14 * static_cast<double>(n) + 1e-15, "inverse undoes forward");
    auto inverse_only = x;
    fft.inverse(inverse_only);
    const auto expected_inverse = naive_dft(x, 1.0);
    for (std::size_t k = 0; k < n; ++k) {
      REQUIRE_LT(std::abs(inverse_only[k] * static_cast<double>(n) - expected_inverse[k]),
                 tolerance::double_rounding * static_cast<double>(n),
                 "inverse matches the conjugate DFT scaled by 1/N");
    }
  }
  // A unit impulse at t0 is a pure phase ramp, at 2^16 points.
  const std::size_t n = 65'536;
  std::vector<Complex> impulse(n);
  impulse[3] = 1.0;
  dsp::Fft(n).forward(impulse);
  double worst = 0.0;
  for (std::size_t k = 0; k < n; k += 97) {
    const Complex expected = std::polar(
        1.0, -2.0 * std::numbers::pi * static_cast<double>((3 * k) % n) / static_cast<double>(n));
    worst = std::max(worst, std::abs(impulse[k] - expected));
  }
  REQUIRE_LT(worst, 1e-12, "long transforms stay accurate (direct twiddles)");
}

} // namespace

int main() {
  try {
    ramp_lands_on_its_targets();
    allpass_is_flat();
    shelves_hit_their_end_gains();
    fft_matches_a_naive_dft();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "native dsp primitives contract passed\n";
  return 0;
}
