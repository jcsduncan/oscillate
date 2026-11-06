#pragma once

// Room-acoustics measurements on impulse responses: octave-band RT60 from
// Schroeder backward integration, the normalised echo density profile
// (Abel and Huang), inter-channel correlation and spectral colouration.

#include "effect_test_support.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/test/audio_constants.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <vector>

namespace expanse_acoustics {

using namespace effect_test;

// RBJ band-pass (constant 0 dB peak), Q for one octave.
inline std::vector<double> octave_band(const std::vector<double>& x, double hz,
                                       double rate = oscillate::test::sample_rate_48k_hz) {
  const double w = 2.0 * std::numbers::pi * hz / rate;
  const double q = std::numbers::sqrt2;
  const double alpha = std::sin(w) / (2.0 * q);
  const double a0 = 1.0 + alpha;
  const double b0 = alpha / a0;
  const double b2 = -alpha / a0;
  const double a1 = -2.0 * std::cos(w) / a0;
  const double a2 = (1.0 - alpha) / a0;
  std::vector<double> y = x;
  // Twice, for a steeper fourth-order band.
  for (int pass = 0; pass < 2; ++pass) {
    double z1 = 0.0;
    double z2 = 0.0;
    for (double& value : y) {
      const double in = value;
      const double out = b0 * in + z1;
      z1 = -a1 * out + z2;
      z2 = b2 * in - a2 * out;
      value = out;
    }
  }
  return y;
}

// RT60 from the Schroeder curve's -5 to -25 dB slope (T20 x 3).
inline double rt60(const std::vector<double>& h,
                   double rate = oscillate::test::sample_rate_48k_hz) {
  std::vector<double> edc(h.size());
  double sum = 0.0;
  for (std::size_t i = h.size(); i-- > 0;) {
    sum += h[i] * h[i];
    edc[i] = sum;
  }
  const double total = edc[0];
  std::size_t start = 0;
  std::size_t end = 0;
  for (std::size_t i = 0; i < edc.size(); ++i) {
    const double level = 10.0 * std::log10(edc[i] / total + 1e-300);
    if (start == 0 && level <= -5.0) {
      start = i;
    }
    if (level <= -25.0) {
      end = i;
      break;
    }
  }
  if (start == 0 || end <= start) {
    return 0.0;
  }
  // Least-squares slope over the range.
  double sx = 0.0;
  double sy = 0.0;
  double sxx = 0.0;
  double sxy = 0.0;
  const auto n = static_cast<double>(end - start);
  for (std::size_t i = start; i < end; ++i) {
    const double t = static_cast<double>(i) / rate;
    const double level = 10.0 * std::log10(edc[i] / total);
    sx += t;
    sy += level;
    sxx += t * t;
    sxy += t * level;
  }
  const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
  return -60.0 / slope;
}

// The normalised echo density at each window: the share of samples beyond
// one standard deviation, over a Gaussian's share (erfc(1/sqrt2)). 1 means
// fully diffuse.
inline std::vector<double> echo_density(const std::vector<double>& h, std::size_t window = 960) {
  const double gaussian = std::erfc(1.0 / std::numbers::sqrt2);
  std::vector<double> out;
  for (std::size_t start = 0; start + window <= h.size(); start += window / 2) {
    double power = 0.0;
    for (std::size_t i = start; i < start + window; ++i) {
      power += h[i] * h[i];
    }
    const double sigma = std::sqrt(power / static_cast<double>(window));
    std::size_t beyond = 0;
    for (std::size_t i = start; i < start + window; ++i) {
      beyond += std::fabs(h[i]) > sigma ? 1U : 0U;
    }
    out.push_back(static_cast<double>(beyond) / static_cast<double>(window) / gaussian);
  }
  return out;
}

inline double correlation(const std::vector<double>& a, const std::vector<double>& b,
                          std::size_t from, std::size_t to) {
  double ab = 0.0;
  double aa = 0.0;
  double bb = 0.0;
  for (std::size_t i = from; i < to; ++i) {
    ab += a[i] * b[i];
    aa += a[i] * a[i];
    bb += b[i] * b[i];
  }
  return ab / std::sqrt(aa * bb);
}

// Welch-averaged power spectrum of a span, in dB per bin (4096-point Hann).
inline std::vector<double> welch_db(const std::vector<double>& x, std::size_t from,
                                    std::size_t to) {
  constexpr std::size_t n = 4'096;
  std::vector<double> power(n / 2, 0.0);
  std::size_t segments = 0;
  for (std::size_t start = from; start + n <= to; start += n / 2) {
    std::vector<std::complex<double>> window(n);
    for (std::size_t i = 0; i < n; ++i) {
      const double hann =
          oscillate::native_dsp::dsp::hann(static_cast<double>(i), static_cast<double>(n));
      window[i] = x[start + i] * hann;
    }
    fft(window);
    for (std::size_t k = 0; k < n / 2; ++k) {
      power[k] += std::norm(window[k]);
    }
    ++segments;
  }
  for (double& value : power) {
    value = 10.0 * std::log10(value / static_cast<double>(segments) + 1e-300);
  }
  return power;
}

// The largest excess of any bin over its third-octave neighbourhood mean,
// between `low` and `high` Hz: a ringing mode stands out.
inline double colouration_db(const std::vector<double>& spectrum_db, double low, double high,
                             double rate = oscillate::test::sample_rate_48k_hz,
                             double* worst_hz = nullptr) {
  const double bin_hz = rate / (2.0 * static_cast<double>(spectrum_db.size()));
  double worst = 0.0;
  for (std::size_t k = 1; k < spectrum_db.size(); ++k) {
    const double hz = static_cast<double>(k) * bin_hz;
    if (hz < low || hz > high) {
      continue;
    }
    const auto from = static_cast<std::size_t>(hz / std::pow(2.0, 1.0 / 6.0) / bin_hz);
    const auto to = std::min(spectrum_db.size(),
                             static_cast<std::size_t>(hz * std::pow(2.0, 1.0 / 6.0) / bin_hz) + 1);
    double mean = 0.0;
    for (std::size_t j = from; j < to; ++j) {
      mean += std::pow(10.0, spectrum_db[j] / 10.0);
    }
    mean = 10.0 * std::log10(mean / static_cast<double>(to - from));
    if (spectrum_db[k] - mean > worst) {
      worst = spectrum_db[k] - mean;
      if (worst_hz != nullptr) {
        *worst_hz = hz;
      }
    }
  }
  return worst;
}

} // namespace expanse_acoustics
