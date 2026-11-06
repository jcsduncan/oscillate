#include "oscillate/warp_dsp/bandlimited.hpp"

#include <array>
#include <cmath>
#include <numbers>

namespace oscillate::warp_dsp {
namespace {

// Q0 ~ 64 taps at unity speed; lower tiers halve the work per step.
constexpr std::array<KernelDesign, 3> tier_designs{{{32, 9.0}, {16, 8.0}, {8, 6.0}}};

[[nodiscard]] double bessel_i0(double x) noexcept {
  double sum = 1.0;
  double term = 1.0;
  const double quarter_square = x * x / 4.0;
  for (int k = 1; k < 64; ++k) {
    term *= quarter_square / (static_cast<double>(k) * static_cast<double>(k));
    sum += term;
    if (term < sum * 1e-17)
      break;
  }
  return sum;
}

} // namespace

double reference_kernel(double x, const KernelDesign& design) {
  const double kaiser_beta = design.kaiser_beta;
  const double magnitude = std::fabs(x);
  const double span = static_cast<double>(design.zero_crossings);
  if (magnitude >= span)
    return 0.0;
  if (magnitude == std::round(magnitude))
    return magnitude == 0.0 ? 1.0 : 0.0;
  const double ratio = magnitude / span;
  const double window =
      bessel_i0(kaiser_beta * std::sqrt(1.0 - ratio * ratio)) / bessel_i0(kaiser_beta);
  const double angle = std::numbers::pi * magnitude;
  return std::sin(angle) / angle * window;
}

BandlimitedKernel::BandlimitedKernel(const KernelDesign& design)
    : design_(design), zero_crossings_(design.zero_crossings) {
  const auto zero_crossings = design.zero_crossings;
  const auto size = static_cast<std::size_t>(zero_crossings) * table_resolution + 1;
  values_.resize(size + 1, 0.0F);
  deltas_.resize(size, 0.0F);
  for (std::size_t index = 0; index < size; ++index)
    values_[index] =
        static_cast<float>(reference_kernel(static_cast<double>(index) / table_resolution, design));
  for (std::size_t index = 0; index < size; ++index)
    deltas_[index] = values_[index + 1] - values_[index];
  const std::size_t taps = std::size_t{2} * zero_crossings;
  std::vector<float> rows((table_resolution + 1) * taps);
  for (std::size_t phase = 0; phase <= table_resolution; ++phase) {
    for (std::size_t tap = 0; tap < taps; ++tap) {
      // Distance (in table steps) from the read point to source frame
      // n - zero_crossings + 1 + tap.
      const std::size_t distance = tap < zero_crossings
                                       ? phase + (zero_crossings - 1 - tap) * table_resolution
                                       : (tap - zero_crossings + 1) * table_resolution - phase;
      rows[phase * taps + tap] = values_[distance];
    }
  }
  rows_.assign(rows.begin(), rows.end() - static_cast<std::ptrdiff_t>(taps));
  row_deltas_.resize(table_resolution * taps);
  for (std::size_t index = 0; index < row_deltas_.size(); ++index)
    row_deltas_[index] = rows[index + taps] - rows[index];
}

std::shared_ptr<const BandlimitedKernel> BandlimitedKernel::for_tier(QualityTier tier) {
  static const std::array<std::shared_ptr<const BandlimitedKernel>, 3> kernels = [] {
    std::array<std::shared_ptr<const BandlimitedKernel>, 3> built;
    for (std::size_t index = 0; index < built.size(); ++index)
      built[index] = std::make_shared<const BandlimitedKernel>(tier_designs[index]);
    return built;
  }();
  return kernels[static_cast<std::size_t>(tier)];
}

} // namespace oscillate::warp_dsp
