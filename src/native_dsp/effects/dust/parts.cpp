#include "effects/dust/parts.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>

namespace oscillate::native_dsp::effects::dust::detail {
namespace {

// The noise-power integral's grid: this many points from power_lowest_hz to
// just below Nyquist (power_top of pi, where tan() is still finite).
constexpr std::size_t power_points = 768;
constexpr double power_lowest_hz = 1.0;
constexpr double power_top = 1.0 - 1e-6;
// A Pareto tail this close to alpha 2 takes the logarithmic closed form.
constexpr double pareto_alpha_tolerance = 1e-9;

} // namespace

double stage_power(const Stage& stage, double x) noexcept {
  const double x2 = x * x;
  const double k = 1.0 / stage.q;
  const double d = (1.0 - x2) * (1.0 - x2) + k * k * x2;
  switch (stage.kind) {
  case Kind::low1:
    return 1.0 / (1.0 + x2);
  case Kind::high1:
    return x2 / (1.0 + x2);
  case Kind::low2:
    return 1.0 / d;
  case Kind::high2:
    return x2 * x2 / d;
  case Kind::band2:
    return k * k * x2 / d;
  case Kind::peak2:
    return ((1.0 - x2) * (1.0 - x2) + stage.gain * stage.gain * k * k * x2) / d;
  }
  return 1.0;
}

double noise_power(std::span<const Stage> stages, double rate) noexcept {
  std::array<double, max_stages> g{};
  const std::size_t count = std::min(stages.size(), max_stages);
  for (std::size_t s = 0; s < count; ++s) {
    g[s] = warp(stages[s].hz, rate);
  }
  const auto response = [&](double w) {
    const double tw = std::tan(0.5 * w);
    double h = 1.0;
    for (std::size_t s = 0; s < count; ++s) {
      h *= stage_power(stages[s], tw / g[s]);
    }
    return h;
  };
  const double lowest = two_pi * power_lowest_hz / rate;
  const double highest = pi * power_top;
  const double span = std::log(highest / lowest);
  // Below the grid: flat at the bottom value.
  double total = response(lowest) * lowest;
  double previous = response(lowest) * lowest;
  for (std::size_t i = 1; i <= power_points; ++i) {
    const double w =
        lowest * std::exp(span * static_cast<double>(i) / static_cast<double>(power_points));
    const double value = response(w) * w; // d(ln w) integrand
    total += 0.5 * (value + previous) * span / static_cast<double>(power_points);
    previous = value;
  }
  return total / pi;
}

double pareto_square_mean(double alpha, double cap) noexcept {
  const double top = std::pow(cap, 2.0 - alpha);
  if (std::fabs(alpha - 2.0) < pareto_alpha_tolerance) {
    return 2.0 * std::log(cap) + 1.0;
  }
  return alpha * (top - 1.0) / (2.0 - alpha) + top;
}

} // namespace oscillate::native_dsp::effects::dust::detail
