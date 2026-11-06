#include "oscillate/native_dsp/synth/oversampler.hpp"

#include <cmath>
#include <cstddef>
#include <numbers>

namespace oscillate::native_dsp::synth {
namespace {

// Elliptic half-band allpass design (de Soras, HIIR PolyphaseIir2Designer).
struct Transition {
  double k;
  double q;
};

Transition transition_parameters(double transition) {
  double k = std::tan((1.0 - transition * 2.0) * std::numbers::pi / 4.0);
  k *= k;
  const double kksqrt = std::pow(1.0 - k * k, 0.25);
  const double e = 0.5 * (1.0 - kksqrt) / (1.0 + kksqrt);
  const double e2 = e * e;
  const double e4 = e2 * e2;
  const double q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));
  return {k, q};
}

double numerator(double q, int order, int c) {
  double sum = 0.0;
  double sign = 1.0;
  for (int i = 0; i < 64; ++i) {
    const double term =
        std::pow(q, i * (i + 1)) * std::sin((i * 2 + 1) * c * std::numbers::pi / order) * sign;
    sum += term;
    sign = -sign;
    if (std::fabs(term) <= 1e-100) {
      break;
    }
  }
  return sum;
}

double denominator(double q, int order, int c) {
  double sum = 0.0;
  double sign = -1.0;
  for (int i = 1; i < 64; ++i) {
    const double term = std::pow(q, i * i) * std::cos(i * 2 * c * std::numbers::pi / order) * sign;
    sum += term;
    sign = -sign;
    if (std::fabs(term) <= 1e-100) {
      break;
    }
  }
  return sum;
}

} // namespace

HalfbandCoefficients design_halfband(double transition) {
  const auto [k, q] = transition_parameters(transition);
  const int order = static_cast<int>(halfband_coefficient_count) * 2 + 1;
  HalfbandCoefficients coefficients{};
  for (std::size_t index = 0; index < coefficients.size(); ++index) {
    const int c = static_cast<int>(index) + 1;
    const double num = numerator(q, order, c) * std::pow(q, 0.25);
    const double den = denominator(q, order, c) + 0.5;
    const double ww = num / den;
    const double wwsq = ww * ww;
    const double x = std::sqrt((1.0 - wwsq * k) * (1.0 - wwsq / k)) / (1.0 + wwsq);
    coefficients[index] = (1.0 - x) / (1.0 + x);
  }
  return coefficients;
}

const HalfbandCoefficients& halfband_coefficients() {
  static const HalfbandCoefficients coefficients = design_halfband(halfband_transition);
  return coefficients;
}

} // namespace oscillate::native_dsp::synth
