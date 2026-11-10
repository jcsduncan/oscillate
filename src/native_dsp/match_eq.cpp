#include "oscillate/native_dsp/match_eq.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

namespace oscillate::native_dsp::match_eq {
namespace {

using tonal::fine_bands;
using tonal::fine_hz;

constexpr double fine_per_octave =
    tonal::bands_per_octave * static_cast<double>(tonal::fine_per_band);
// Silent bands (dsp::silent_db) take no part: anything this far under the
// loudness is not a measurement.
constexpr double silent_below_db = -150.0;

// The fit. Outside low..high the curve fades to zero; it is still fitted
// there, more loosely, so no band runs away beyond the limits.
constexpr double outside_weight = 0.3;
// Parameters per band: log2 frequency, log2 shape, gain.
constexpr std::size_t per_band = 3;
constexpr std::size_t max_parameters = max_bands * per_band;
// Shape's ranges.
constexpr double max_slope = 1.0;
constexpr double max_gain_db = 24.0;
// A shelf's slope when first placed.
constexpr double initial_slope = 0.7;
// Where a shelf is first placed, and the region whose mean error sets its
// gain.
constexpr double low_shelf_hz = 120.0;
constexpr double high_shelf_hz = 6'000.0;
// Peaks start at least this wide.
constexpr double narrowest_start_octaves = 1.0 / 6.0;
// Bands left with less gain than this after the fit are dropped.
constexpr double negligible_gain_db = 0.1;
// Levenberg-Marquardt.
constexpr int max_iterations = 60;
constexpr double initial_damping = 1e-2;
constexpr double damping_down = 3.0;
constexpr double damping_up = 4.0;
constexpr double max_damping = 1e8;
constexpr double converged = 1e-7;
constexpr double ridge = 1e-12;
// Forward-difference steps: octaves, log2 shape and dB.
constexpr std::array<double, per_band> difference_step{1e-3, 1e-3, 1e-3};

[[nodiscard]] double octaves(double from_hz, double to_hz) noexcept {
  return std::log2(to_hz / from_hz);
}

// 1 inside low..high, a raised cosine down to 0 over fade_octaves outside.
[[nodiscard]] double inside(double hz, const Settings& s) noexcept {
  double beyond = 0.0;
  if (hz < s.low_hz) {
    beyond = octaves(hz, s.low_hz);
  } else if (hz > s.high_hz) {
    beyond = octaves(s.high_hz, hz);
  }
  if (beyond >= fade_octaves) {
    return 0.0;
  }
  const double c = std::cos(0.5 * std::numbers::pi * beyond / fade_octaves);
  return c * c;
}

[[nodiscard]] bool in_limits(double hz, const Settings& s) noexcept {
  return hz >= s.low_hz && hz <= s.high_hz;
}

[[nodiscard]] FineCurve normalised(const FineCurve& levels) noexcept {
  const double loudness = tonal::fine_loudness_db(levels);
  FineCurve out{};
  for (std::size_t i = 0; i < fine_bands; ++i) {
    out[i] = levels[i] - loudness;
  }
  return out;
}

struct Bounds {
  double low_x;
  double high_x;
  double low_y;
  double high_y;
  double gain;
};

[[nodiscard]] Bounds bounds_for(EqBandKind kind) noexcept {
  const bool shelf = kind != EqBandKind::peaking;
  return {std::log2(fine_hz(0)), std::log2(fine_hz(fine_bands - 1)),
          std::log2(shelf ? min_slope : min_q), std::log2(shelf ? max_slope : max_q), max_gain_db};
}

// One band's response at every fine band, as Shape designs it at fit_rate.
void band_response(const Band& band, FineCurve& out) noexcept {
  const EqBandSettings settings{band.kind, band.hz, band.shape, band.gain_db};
  auto prepared = prepare_eq(std::span(&settings, 1), static_cast<std::uint32_t>(fit_rate));
  if (!prepared) {
    out.fill(0.0);
    return;
  }
  for (std::size_t i = 0; i < fine_bands; ++i) {
    out[i] = eq_magnitude_db(prepared.value(), fine_hz(i)).value_or(0.0);
  }
}

// The Levenberg-Marquardt refinement of every band together.
class Refiner {
public:
  Refiner(const FineCurve& curve, const FineCurve& weight) : curve_(curve), weight_(weight) {}

  // Refines `bands` in place; returns the weighted squared error.
  double refine(std::span<Band> bands) {
    const std::size_t n = bands.size() * per_band;
    std::array<double, max_parameters> p{};
    for (std::size_t k = 0; k < bands.size(); ++k) {
      p[k * per_band] = std::log2(bands[k].hz);
      p[k * per_band + 1] = std::log2(bands[k].shape);
      p[k * per_band + 2] = bands[k].gain_db;
    }
    std::vector<FineCurve> responses(bands.size());
    for (std::size_t k = 0; k < bands.size(); ++k) {
      band_response(decode(bands[k].kind, p, k), responses[k]);
    }
    double cost = error(responses);
    double damping = initial_damping;
    std::vector<FineCurve> jacobian(n);
    for (int iteration = 0; iteration < max_iterations && damping < max_damping; ++iteration) {
      // Residual r = w (curve - sum), Jacobian of the sum.
      FineCurve residual = residuals(responses);
      for (std::size_t k = 0; k < bands.size(); ++k) {
        for (std::size_t j = 0; j < per_band; ++j) {
          auto moved = p;
          moved[k * per_band + j] += difference_step[j];
          FineCurve shifted{};
          band_response(decode(bands[k].kind, moved, k), shifted);
          for (std::size_t i = 0; i < fine_bands; ++i) {
            jacobian[k * per_band + j][i] =
                weight_[i] * (shifted[i] - responses[k][i]) / difference_step[j];
          }
        }
      }
      // Normal equations: (J'J + damping diag(J'J)) step = J'r.
      std::array<std::array<double, max_parameters>, max_parameters> a{};
      std::array<double, max_parameters> g{};
      for (std::size_t r = 0; r < n; ++r) {
        for (std::size_t c = 0; c <= r; ++c) {
          double sum = 0.0;
          for (std::size_t i = 0; i < fine_bands; ++i) {
            sum += jacobian[r][i] * jacobian[c][i];
          }
          a[r][c] = sum;
          a[c][r] = sum;
        }
        double sum = 0.0;
        for (std::size_t i = 0; i < fine_bands; ++i) {
          sum += jacobian[r][i] * residual[i];
        }
        g[r] = sum;
      }
      bool improved = false;
      while (!improved && damping < max_damping) {
        auto system = a;
        for (std::size_t r = 0; r < n; ++r) {
          system[r][r] += damping * a[r][r] + ridge;
        }
        const auto step = solve(system, g, n);
        auto trial = p;
        for (std::size_t r = 0; r < n; ++r) {
          trial[r] += step[r];
        }
        clamp(bands, trial);
        std::vector<FineCurve> trial_responses(bands.size());
        for (std::size_t k = 0; k < bands.size(); ++k) {
          band_response(decode(bands[k].kind, trial, k), trial_responses[k]);
        }
        const double trial_cost = error(trial_responses);
        if (trial_cost < cost) {
          const double gain = (cost - trial_cost) / std::max(cost, ridge);
          p = trial;
          responses = std::move(trial_responses);
          cost = trial_cost;
          damping /= damping_down;
          improved = true;
          if (gain < converged) {
            damping = max_damping;
          }
        } else {
          damping *= damping_up;
        }
      }
    }
    for (std::size_t k = 0; k < bands.size(); ++k) {
      bands[k] = decode(bands[k].kind, p, k);
    }
    return cost;
  }

private:
  [[nodiscard]] static Band decode(EqBandKind kind, const std::array<double, max_parameters>& p,
                                   std::size_t k) noexcept {
    return {kind, std::exp2(p[k * per_band]), std::exp2(p[k * per_band + 1]), p[k * per_band + 2]};
  }

  static void clamp(std::span<const Band> bands, std::array<double, max_parameters>& p) noexcept {
    for (std::size_t k = 0; k < bands.size(); ++k) {
      const auto b = bounds_for(bands[k].kind);
      p[k * per_band] = std::clamp(p[k * per_band], b.low_x, b.high_x);
      p[k * per_band + 1] = std::clamp(p[k * per_band + 1], b.low_y, b.high_y);
      p[k * per_band + 2] = std::clamp(p[k * per_band + 2], -b.gain, b.gain);
    }
  }

  [[nodiscard]] FineCurve residuals(const std::vector<FineCurve>& responses) const noexcept {
    FineCurve r{};
    for (std::size_t i = 0; i < fine_bands; ++i) {
      double sum = 0.0;
      for (const auto& response : responses) {
        sum += response[i];
      }
      r[i] = weight_[i] * (curve_[i] - sum);
    }
    return r;
  }

  [[nodiscard]] double error(const std::vector<FineCurve>& responses) const noexcept {
    const auto r = residuals(responses);
    double sum = 0.0;
    for (const double value : r) {
      sum += value * value;
    }
    return sum;
  }

  // Gaussian elimination with partial pivoting on the leading n x n block.
  [[nodiscard]] static std::array<double, max_parameters>
  solve(std::array<std::array<double, max_parameters>, max_parameters> a,
        std::array<double, max_parameters> b, std::size_t n) noexcept {
    for (std::size_t col = 0; col < n; ++col) {
      std::size_t pivot = col;
      for (std::size_t r = col + 1; r < n; ++r) {
        if (std::abs(a[r][col]) > std::abs(a[pivot][col])) {
          pivot = r;
        }
      }
      std::swap(a[col], a[pivot]);
      std::swap(b[col], b[pivot]);
      if (std::abs(a[col][col]) < ridge) {
        continue;
      }
      for (std::size_t r = col + 1; r < n; ++r) {
        const double f = a[r][col] / a[col][col];
        for (std::size_t c = col; c < n; ++c) {
          a[r][c] -= f * a[col][c];
        }
        b[r] -= f * b[col];
      }
    }
    std::array<double, max_parameters> x{};
    for (std::size_t r = n; r-- > 0;) {
      double sum = b[r];
      for (std::size_t c = r + 1; c < n; ++c) {
        sum -= a[r][c] * x[c];
      }
      x[r] = std::abs(a[r][r]) < ridge ? 0.0 : sum / a[r][r];
    }
    return x;
  }

  const FineCurve& curve_;
  const FineCurve& weight_;
};

// Where to try the next band: a peak at the largest error, and a shelf at
// either end that has none yet.
[[nodiscard]] std::vector<Band> candidates(const FineCurve& residual, const Settings& s,
                                           std::span<const Band> placed) {
  std::vector<Band> out;
  std::optional<std::size_t> worst;
  for (std::size_t i = 0; i < fine_bands; ++i) {
    if (in_limits(fine_hz(i), s) &&
        (!worst || std::abs(residual[i]) > std::abs(residual[*worst]))) {
      worst = i;
    }
  }
  if (!worst) {
    return out;
  }
  const double peak = residual[*worst];
  // Half-gain width, on the same side of zero.
  std::size_t left = *worst;
  while (left > 0 && residual[left - 1] * peak > 0.5 * peak * peak) {
    --left;
  }
  std::size_t right = *worst;
  while (right + 1 < fine_bands && residual[right + 1] * peak > 0.5 * peak * peak) {
    ++right;
  }
  const double width =
      std::max(narrowest_start_octaves, static_cast<double>(right - left + 1) / fine_per_octave);
  const double ratio = std::exp2(width);
  const double q = std::clamp(std::sqrt(ratio) / (ratio - 1.0), min_q, max_q);
  out.push_back({EqBandKind::peaking, fine_hz(*worst), q, peak});
  const auto has = [&](EqBandKind kind) {
    return std::ranges::any_of(placed, [&](const Band& b) { return b.kind == kind; });
  };
  const auto mean = [&](auto&& wanted) {
    double sum = 0.0;
    double count = 0.0;
    for (std::size_t i = 0; i < fine_bands; ++i) {
      if (in_limits(fine_hz(i), s) && wanted(fine_hz(i))) {
        sum += residual[i];
        count += 1.0;
      }
    }
    return count > 0.0 ? sum / count : 0.0;
  };
  if (!has(EqBandKind::low_shelf)) {
    out.push_back({EqBandKind::low_shelf, low_shelf_hz, initial_slope,
                   mean([](double hz) { return hz < low_shelf_hz; })});
  }
  if (!has(EqBandKind::high_shelf)) {
    out.push_back({EqBandKind::high_shelf, high_shelf_hz, initial_slope,
                   mean([](double hz) { return hz > high_shelf_hz; })});
  }
  return out;
}

void measure(Fit& result, const FineCurve& curve, const Settings& s) {
  result.response_db = response_db(std::span(result.band.data(), result.size));
  double sum = 0.0;
  double count = 0.0;
  result.max_db = 0.0;
  for (std::size_t i = 0; i < fine_bands; ++i) {
    if (in_limits(fine_hz(i), s)) {
      const double e = curve[i] - result.response_db[i];
      sum += e * e;
      count += 1.0;
      result.max_db = std::max(result.max_db, std::abs(e));
    }
  }
  result.rms_db = count > 0.0 ? std::sqrt(sum / count) : 0.0;
}

} // namespace

FineCurve correction(const FineCurve& mix_db, const FineCurve& reference_db,
                     const Settings& settings) noexcept {
  const auto mix = normalised(mix_db);
  const auto reference = normalised(reference_db);
  FineCurve difference{};
  std::array<bool, fine_bands> valid{};
  for (std::size_t i = 0; i < fine_bands; ++i) {
    valid[i] = mix[i] > silent_below_db && reference[i] > silent_below_db;
    difference[i] = valid[i] ? reference[i] - mix[i] : 0.0;
  }
  // A Hann window in octaves whose half-power width is the smoothing: it
  // reaches the smoothing either side.
  const double reach = std::max(settings.smoothing_octaves, 1.0 / fine_per_octave);
  FineCurve out{};
  for (std::size_t i = 0; i < fine_bands; ++i) {
    double sum = 0.0;
    double weights = 0.0;
    for (std::size_t j = 0; j < fine_bands; ++j) {
      const double distance = std::abs(octaves(fine_hz(i), fine_hz(j)));
      if (!valid[j] || distance >= reach) {
        continue;
      }
      const double c = std::cos(0.5 * std::numbers::pi * distance / reach);
      sum += c * c * difference[j];
      weights += c * c;
    }
    const double smoothed = weights > 0.0 ? sum / weights : 0.0;
    const double scaled = std::clamp(std::clamp(settings.amount, 0.0, 1.0) * smoothed,
                                     -settings.max_cut_db, settings.max_boost_db);
    out[i] = scaled * inside(fine_hz(i), settings);
  }
  return out;
}

FineCurve target_levels(const tonal::Target& target) noexcept {
  FineCurve out{};
  for (std::size_t i = 0; i < fine_bands; ++i) {
    // The fine band's position on the grid, in bands from the first.
    const double at = tonal::bands_per_octave * std::log2(fine_hz(i) / tonal::pivot_hz) -
                      static_cast<double>(tonal::first_band_step);
    if (at <= 0.0) {
      out[i] = target.median.front();
    } else if (at >= static_cast<double>(tonal::bands - 1)) {
      out[i] = target.median.back();
    } else {
      const auto below = static_cast<std::size_t>(at);
      const double frac = at - static_cast<double>(below);
      out[i] = target.median[below] + frac * (target.median[below + 1] - target.median[below]);
    }
  }
  return out;
}

FineCurve response_db(std::span<const Band> bands) {
  FineCurve out{};
  FineCurve one{};
  for (const auto& band : bands) {
    band_response(band, one);
    for (std::size_t i = 0; i < fine_bands; ++i) {
      out[i] += one[i];
    }
  }
  return out;
}

Fit fit(const FineCurve& curve_db, const Settings& settings) {
  FineCurve weight{};
  for (std::size_t i = 0; i < fine_bands; ++i) {
    weight[i] = in_limits(fine_hz(i), settings) ? 1.0 : outside_weight;
  }
  Refiner refiner(curve_db, weight);
  Fit result;
  measure(result, curve_db, settings);
  while (result.size < max_bands && result.rms_db > fit_tolerance_db) {
    FineCurve residual{};
    for (std::size_t i = 0; i < fine_bands; ++i) {
      residual[i] = curve_db[i] - result.response_db[i];
    }
    const auto tries = candidates(residual, settings, std::span(result.band.data(), result.size));
    if (tries.empty()) {
      break;
    }
    std::optional<Fit> best;
    double best_cost = std::numeric_limits<double>::infinity();
    for (const auto& candidate : tries) {
      Fit trial = result;
      trial.band[trial.size++] = candidate;
      const double cost = refiner.refine(std::span(trial.band.data(), trial.size));
      if (cost < best_cost) {
        best_cost = cost;
        best = trial;
      }
    }
    const double before = result.rms_db;
    result = *best;
    measure(result, curve_db, settings);
    if (result.rms_db >= before) {
      break;
    }
  }
  // Drop bands that ended up doing nothing, then refine what is left.
  const auto kept = std::ranges::remove_if(
      result.band.begin(), result.band.begin() + static_cast<std::ptrdiff_t>(result.size),
      [](const Band& b) { return std::abs(b.gain_db) < negligible_gain_db; });
  const auto dropped = static_cast<std::size_t>(std::distance(kept.begin(), kept.end()));
  if (dropped > 0) {
    result.size -= dropped;
    (void)refiner.refine(std::span(result.band.data(), result.size));
  }
  measure(result, curve_db, settings);
  return result;
}

} // namespace oscillate::native_dsp::match_eq
