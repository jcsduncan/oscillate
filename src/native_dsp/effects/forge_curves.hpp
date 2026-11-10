#pragma once

#include "forge_table.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <type_traits>

// Forge's saturation curves (ADR-0040), shared with Gild (ADR-0122): each
// curve passes through the origin with unit slope and has a closed-form
// antiderivative for first-order antiderivative antialiasing.
namespace oscillate::native_dsp::effects::forge_curves {

using forge_table::Model;

// The tube's cut-off side saturates later and more softly than its
// grid-conduction side: tanh(a u) / a there.
constexpr double tube_negative = 0.5;

// Below this input step the antialiased value is the curve at the step's
// midpoint: the antiderivatives' difference would cancel to noise.
constexpr double antialias_min_step = 1e-7;

// log(cosh(u)) = |u| + log1p(exp(-2|u|)) - ln 2, without overflow, for a
// whole oversampled frame at once. Three of the curves spend most of their
// time here, and the library's exp and log1p take one value at a time; these
// loops have no branches or calls, so each step runs over every value
// together (two at a time on SSE2). As accurate as the library form: within
// 3e-16 of the exact value for |u| <= 1, and 3e-16 |u| beyond.
namespace log_cosh_batch {
// exp(-2|u|) only matters while it moves log1p's result: below e^-60 it adds
// nothing to |u| >= 30.
constexpr double exp_floor = -60.0;
// Cody-Waite split of ln 2 (fdlibm): k ln2_hi is exact for |k| < 2^21.
constexpr double ln2_hi = 6.93147180369123816490e-01;
constexpr double ln2_lo = 1.90821492927058770002e-10;
// Adding then subtracting 1.5 * 2^52 rounds to the nearest integer, which
// the sum's low mantissa bits then hold.
constexpr double round_shift = 6755399436066816.0;
constexpr std::uint64_t exponent_bias = 1023;
constexpr int mantissa_bits = 52;
// e^r for |r| <= ln2 / 2 through r^13 / 13!: the next term is below 1e-17.
constexpr std::size_t exp_terms = 14;
// log1p(z) = 2 atanh(s), s = z / (2 + z) <= 1/3, through s^33: the next term
// is below 2e-18.
constexpr std::size_t atanh_terms = 16;

constexpr auto exp_coefficients = [] {
  std::array<double, exp_terms> c{};
  double factorial = 1.0;
  for (std::size_t n = 0; n < exp_terms; ++n) {
    factorial *= n == 0 ? 1.0 : static_cast<double>(n);
    c[n] = 1.0 / factorial;
  }
  return c;
}();
constexpr auto atanh_coefficients = [] {
  std::array<double, atanh_terms> c{};
  for (std::size_t k = 0; k < atanh_terms; ++k) {
    c[k] = 1.0 / static_cast<double>(2 * k + 3);
  }
  return c;
}();

template <std::size_t N>
void evaluate(const std::array<double, N>& u, std::array<double, N>& out) noexcept {
  std::array<double, N> a{};
  std::array<double, N> r{};
  std::array<double, N> scale{};
  for (std::size_t j = 0; j < N; ++j) {
    a[j] = std::fabs(u[j]);
    const double x = -2.0 * a[j];
    // NaN goes to the floor too, and comes back out through a.
    const double clamped = x >= exp_floor ? x : exp_floor;
    const double shifted = clamped * std::numbers::log2e + round_shift;
    const double k = shifted - round_shift;
    r[j] = (clamped - k * ln2_hi) - k * ln2_lo;
    const std::uint64_t power =
        std::bit_cast<std::uint64_t>(shifted) - std::bit_cast<std::uint64_t>(round_shift);
    scale[j] = std::bit_cast<double>((power + exponent_bias) << mantissa_bits);
  }
  std::array<double, N> p{};
  p.fill(exp_coefficients[exp_terms - 1]);
  for (std::size_t n = exp_terms - 1; n-- > 0;) {
    for (std::size_t j = 0; j < N; ++j) {
      p[j] = p[j] * r[j] + exp_coefficients[n];
    }
  }
  // p becomes z = exp(-2|u|), then s, with r holding s^2.
  for (std::size_t j = 0; j < N; ++j) {
    const double z = p[j] * scale[j];
    p[j] = z / (2.0 + z);
    r[j] = p[j] * p[j];
  }
  std::array<double, N> q{};
  q.fill(atanh_coefficients[atanh_terms - 1]);
  for (std::size_t k = atanh_terms - 1; k-- > 0;) {
    for (std::size_t j = 0; j < N; ++j) {
      q[j] = q[j] * r[j] + atanh_coefficients[k];
    }
  }
  for (std::size_t j = 0; j < N; ++j) {
    const double s = p[j];
    out[j] = a[j] + (2.0 * s + 2.0 * s * r[j] * q[j]) - std::numbers::ln2;
  }
}
} // namespace log_cosh_batch

[[nodiscard]] inline double log_cosh(double u) noexcept {
  std::array<double, 1> out{};
  log_cosh_batch::evaluate(std::array<double, 1>{u}, out);
  return out[0];
}

// The triangle fold maps u to 4|s| - 1, s = (u + 1) / 4 wrapped to
// [-0.5, 0.5): slope 1 through the origin, folding at +-1, +-3, ...
[[nodiscard]] inline double fold_phase(double u) noexcept {
  const double t = (u + 1.0) / 4.0;
  return t - synth::floor_inline(t + 0.5);
}

// Every curve passes through the origin with unit slope.
template <int M> [[nodiscard]] inline double curve_of(double u) noexcept {
  if constexpr (M == Model::tube) {
    return u >= 0.0 ? std::tanh(u) : std::tanh(tube_negative * u) / tube_negative;
  } else if constexpr (M == Model::transistor) {
    return u / std::sqrt(1.0 + u * u);
  } else if constexpr (M == Model::diode) {
    return std::copysign(1.0 - std::exp(-std::fabs(u)), u);
  } else if constexpr (M == Model::clip) {
    return std::clamp(u, -1.0, 1.0);
  } else if constexpr (M == Model::fold) {
    return 4.0 * std::fabs(fold_phase(u)) - 1.0;
  } else if constexpr (M == Model::sine_fold) {
    return std::sin(u);
  } else if constexpr (M == Model::rectify) {
    return std::fabs(std::tanh(u));
  } else {
    return std::tanh(u);
  }
}

// The antiderivative of each curve, zero at the origin.
template <int M> [[nodiscard]] inline double antiderivative_of(double u) noexcept {
  if constexpr (M == Model::tube) {
    return u >= 0.0 ? log_cosh(u) : log_cosh(tube_negative * u) / (tube_negative * tube_negative);
  } else if constexpr (M == Model::transistor) {
    return std::sqrt(1.0 + u * u) - 1.0;
  } else if constexpr (M == Model::diode) {
    const double a = std::fabs(u);
    return a + std::exp(-a) - 1.0;
  } else if constexpr (M == Model::clip) {
    const double a = std::fabs(u);
    return a <= 1.0 ? 0.5 * u * u : a - 0.5;
  } else if constexpr (M == Model::fold) {
    const double s = fold_phase(u);
    return 8.0 * s * std::fabs(s) - 4.0 * s + 0.5;
  } else if constexpr (M == Model::sine_fold) {
    return 1.0 - std::cos(u);
  } else if constexpr (M == Model::rectify) {
    return std::copysign(log_cosh(u), u);
  } else {
    return log_cosh(u);
  }
}

// The antiderivative at every input of an oversampled frame. The log-cosh
// curves go through log_cosh_batch; every element is the value
// antiderivative_of<M> gives.
template <int M, std::size_t N>
void antiderivatives_of(const std::array<double, N>& u, std::array<double, N>& out) noexcept {
  if constexpr (M == Model::tube) {
    std::array<double, N> v{};
    for (std::size_t j = 0; j < N; ++j) {
      v[j] = u[j] >= 0.0 ? u[j] : tube_negative * u[j];
    }
    log_cosh_batch::evaluate(v, out);
    for (std::size_t j = 0; j < N; ++j) {
      out[j] = u[j] >= 0.0 ? out[j] : out[j] / (tube_negative * tube_negative);
    }
  } else if constexpr (M == Model::rectify) {
    log_cosh_batch::evaluate(u, out);
    for (std::size_t j = 0; j < N; ++j) {
      out[j] = std::copysign(out[j], u[j]);
    }
  } else if constexpr (M == Model::transistor || M == Model::diode || M == Model::clip ||
                       M == Model::fold || M == Model::sine_fold) {
    for (std::size_t j = 0; j < N; ++j) {
      out[j] = antiderivative_of<M>(u[j]);
    }
  } else {
    log_cosh_batch::evaluate(u, out);
  }
}

// Calls f(std::integral_constant<int, model>{}), so a loop over samples is
// compiled once per curve instead of switching on every sample.
template <typename F> decltype(auto) with_model(int model, F&& f) {
  switch (model) {
  case Model::tube:
    return f(std::integral_constant<int, Model::tube>{});
  case Model::transistor:
    return f(std::integral_constant<int, Model::transistor>{});
  case Model::diode:
    return f(std::integral_constant<int, Model::diode>{});
  case Model::clip:
    return f(std::integral_constant<int, Model::clip>{});
  case Model::fold:
    return f(std::integral_constant<int, Model::fold>{});
  case Model::sine_fold:
    return f(std::integral_constant<int, Model::sine_fold>{});
  case Model::rectify:
    return f(std::integral_constant<int, Model::rectify>{});
  default:
    return f(std::integral_constant<int, Model::tape>{});
  }
}

[[nodiscard]] inline double curve(int model, double u) noexcept {
  return with_model(model, [u](auto m) { return curve_of<decltype(m)::value>(u); });
}

[[nodiscard]] inline double antiderivative(int model, double u) noexcept {
  return with_model(model, [u](auto m) { return antiderivative_of<decltype(m)::value>(u); });
}

// First-order antiderivative antialiasing: the curve's mean over the
// segment from the previous input, which suppresses the aliases of every
// harmonic by a further ~1/n^2 at the cost of a half-sample delay.
[[nodiscard]] inline double antialiased(int model, double u, double previous) noexcept {
  const double du = u - previous;
  if (std::fabs(du) < antialias_min_step) {
    return curve(model, 0.5 * (u + previous));
  }
  return (antiderivative(model, u) - antiderivative(model, previous)) / du;
}

// One frame of a model switch: both curves, their offsets at the bias, and
// the old curve's weight.
struct Crossfade {
  int model;
  int fading;
  double offset;
  double fading_offset;
  double weight;
};

// One channel's antialiasing history. The antiderivative at the previous
// input is kept from the step that computed it, so a step evaluates the
// antiderivative once: the same value antialiased() computes, in half the
// work.
struct Antialias {
  double previous_u = 0.0;
  double previous_ad = 0.0;
  bool ad_known = false;
  // The last midpoint step: a held input (silence at the bias point) steps
  // from the same point again and again.
  double midpoint = 0.0;
  double midpoint_y = 0.0;
  bool midpoint_known = false;

  void seed(double u) noexcept {
    previous_u = u;
    forget();
  }
  // What it kept belongs to another curve.
  void forget() noexcept {
    ad_known = false;
    midpoint_known = false;
  }

  // ad: the antiderivative at u.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): an input and its antiderivative.
  template <int M> [[nodiscard]] double step(double u, double ad) noexcept {
    const double du = u - previous_u;
    double y = 0.0;
    if (std::fabs(du) < antialias_min_step) {
      const double mid = 0.5 * (u + previous_u);
      if (!midpoint_known || mid != midpoint) {
        midpoint = mid;
        midpoint_y = curve_of<M>(mid);
        midpoint_known = true;
      }
      y = midpoint_y;
      ad_known = false;
    } else {
      if (!ad_known) {
        previous_ad = antiderivative_of<M>(previous_u);
      }
      y = (ad - previous_ad) / du;
      previous_ad = ad;
      ad_known = true;
    }
    previous_u = u;
    return y;
  }

  // The crossfade between two curves after a model switch.
  [[nodiscard]] double blend(const Crossfade& fade, double u) noexcept {
    double y = antialiased(fade.model, u, previous_u) - fade.offset;
    if (fade.weight > 0.0) {
      const double old = antialiased(fade.fading, u, previous_u) - fade.fading_offset;
      y += (old - y) * fade.weight;
    }
    previous_u = u;
    forget();
    return y;
  }
};

} // namespace oscillate::native_dsp::effects::forge_curves
