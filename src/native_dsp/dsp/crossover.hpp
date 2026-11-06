#pragma once

#include "biquad.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <span>

// Linkwitz-Riley crossovers shared by the native DSP (ADR-0082): the RBJ
// Butterworth low- and high-pass sections, the fourth-order split built from
// them and a phase-compensated tree that splits one channel into up to
// MaxCrossovers + 1 bands summing to one allpass (flat magnitude).
namespace oscillate::native_dsp::dsp {

// A Butterworth section's Q: two in cascade make a Linkwitz-Riley
// fourth-order filter, whose low and high outputs sum to the RBJ allpass of
// the same Q.

// The RBJ second-order low- and high-pass, on the same bilinear prewarp as
// rbj_allpass, so LR4 low plus LR4 high equals rbj_allpass(hz, 1/sqrt2)
// exactly (up to rounding).
[[nodiscard]] inline BiquadCoefficients rbj_lowpass(double hz, double q, double rate) noexcept {
  const double w = 2.0 * std::numbers::pi * hz / rate;
  const double alpha = std::sin(w) / (2.0 * q);
  const double c = std::cos(w);
  const double a0 = 1.0 + alpha;
  const double b1 = (1.0 - c) / a0;
  return {0.5 * b1, b1, 0.5 * b1, -2.0 * c / a0, (1.0 - alpha) / a0};
}

[[nodiscard]] inline BiquadCoefficients rbj_highpass(double hz, double q, double rate) noexcept {
  const double w = 2.0 * std::numbers::pi * hz / rate;
  const double alpha = std::sin(w) / (2.0 * q);
  const double c = std::cos(w);
  const double a0 = 1.0 + alpha;
  const double b0 = (1.0 + c) / (2.0 * a0);
  return {b0, -2.0 * b0, b0, -2.0 * c / a0, (1.0 - alpha) / a0};
}

// |H(e^jw)| of a section at `hz`.
[[nodiscard]] inline double magnitude(const BiquadCoefficients& c, double hz,
                                      double rate) noexcept {
  const std::complex<double> z1 = std::polar(1.0, -2.0 * std::numbers::pi * hz / rate);
  const std::complex<double> z2 = z1 * z1;
  return std::abs((c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2));
}

// One crossover's three designs.
struct Lr4Design {
  BiquadCoefficients low{};
  BiquadCoefficients high{};
  BiquadCoefficients allpass{};
  static Lr4Design at(double hz, double rate) noexcept {
    return {rbj_lowpass(hz, butterworth_q, rate), rbj_highpass(hz, butterworth_q, rate),
            rbj_allpass(hz, butterworth_q, rate)};
  }
};

// A fourth-order Linkwitz-Riley split of one channel.
struct Lr4Split {
  std::array<Biquad, 2> low{};
  std::array<Biquad, 2> high{};
  void set(const Lr4Design& d) noexcept {
    for (auto& s : low) {
      s.set(d.low);
    }
    for (auto& s : high) {
      s.set(d.high);
    }
  }
  void reset() noexcept {
    for (auto* s : {&low[0], &low[1], &high[0], &high[1]}) {
      s->reset();
    }
  }
  void process(double x, double& lo, double& hi) noexcept {
    lo = low[1].process(low[0].process(x));
    hi = high[1].process(high[0].process(x));
  }
};

// Splits one channel at `count` ascending crossovers into count + 1 bands.
// The rest after split k feeds split k + 1; band k then passes the allpasses
// of every higher crossover, so every band carries the same total allpass
// and the bands sum to it: magnitude-flat, at zero latency. Coefficients may
// change between samples (no state is cleared), so crossovers can glide.
template <std::size_t MaxCrossovers> class CrossoverTree {
public:
  static constexpr std::size_t max_bands = MaxCrossovers + 1;

  // Control or callback: `designs` holds at least `count` designs.
  void set(std::span<const Lr4Design> designs, std::size_t count) noexcept {
    count_ = count < MaxCrossovers ? count : MaxCrossovers;
    for (std::size_t k = 0; k < count_; ++k) {
      split_[k].set(designs[k]);
      for (std::size_t band = 0; band < k; ++band) {
        compensation_[band][k].set(designs[k].allpass);
      }
    }
  }
  void reset() noexcept {
    for (auto& s : split_) {
      s.reset();
    }
    for (auto& row : compensation_) {
      for (auto& s : row) {
        s.reset();
      }
    }
  }
  [[nodiscard]] std::size_t bands() const noexcept { return count_ + 1; }
  // Writes bands() values.
  void process(double x, std::span<double> out) noexcept {
    double rest = x;
    for (std::size_t k = 0; k < count_; ++k) {
      double lo = 0.0;
      double hi = 0.0;
      split_[k].process(rest, lo, hi);
      out[k] = lo;
      rest = hi;
    }
    out[count_] = rest;
    for (std::size_t band = 0; band + 1 < count_; ++band) {
      for (std::size_t k = band + 1; k < count_; ++k) {
        out[band] = compensation_[band][k].process(out[band]);
      }
    }
  }

private:
  std::array<Lr4Split, MaxCrossovers> split_{};
  // compensation_[band][k]: crossover k's allpass on band `band` (k > band).
  std::array<std::array<Biquad, MaxCrossovers>, MaxCrossovers> compensation_{};
  std::size_t count_ = 0;
};

// The magnitude of band `band` of such a tree at `hz`: its own low-pass and
// every lower crossover's high-pass (the allpasses are unity).
[[nodiscard]] inline double tree_band_magnitude(std::span<const Lr4Design> designs,
                                                std::size_t count, std::size_t band, double hz,
                                                double rate) noexcept {
  double m = 1.0;
  for (std::size_t k = 0; k < count && k <= band; ++k) {
    const double section =
        k == band ? magnitude(designs[k].low, hz, rate) : magnitude(designs[k].high, hz, rate);
    m *= section * section;
  }
  return m;
}

} // namespace oscillate::native_dsp::dsp
