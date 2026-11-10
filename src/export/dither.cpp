#include "oscillate/export/dither.hpp"

#include <algorithm>
#include <cmath>

namespace oscillate::exporting {
namespace {

// Wannamaker, "Psychoacoustically optimal noise shaping" (JAES 1992): the
// three-tap E-weighted and nine-tap F-weighted error filters for 44.1 kHz,
// used at 44.1 and 48 kHz.
constexpr std::array<double, 3> e_weighted{1.623, -0.982, 0.109};
constexpr std::array<double, 9> f_weighted{2.412,  -3.370, 3.937,  -4.174, 3.353,
                                           -2.205, 1.281,  -0.569, 0.0847};
// At 88.2 and 96 kHz: zeros on the unit circle at 5, 12 and 18 kHz, so
// the noise stays at least 6.9 (88.2) and 10.3 dB (96 kHz) under flat TPDF
// up to 20 kHz and rises above it.
constexpr std::array<double, 6> high_rate_88k{3.756175,  -7.274174, 8.912526,
                                              -7.274174, 3.756175,  -1.0};
constexpr std::array<double, 6> high_rate_96k{4.073441,  -8.210213, 10.196781,
                                              -8.210213, 4.073441,  -1.0};
static_assert(e_weighted.size() <= DitherQuantizer::max_taps &&
                  f_weighted.size() <= DitherQuantizer::max_taps &&
                  high_rate_88k.size() <= DitherQuantizer::max_taps &&
                  high_rate_96k.size() <= DitherQuantizer::max_taps,
              "every error filter fits the quantizer's history");
// Rates up to this one take the 44.1 kHz designs; above it, 88.2 kHz takes
// its own and 96 kHz its own.
constexpr std::uint32_t base_rate_limit = 48'000;
constexpr std::uint32_t rate_88k = 88'200;
// The total error (TPDF plus rounding) stays within 1.5 LSB unless the
// output clips; holding the fed-back error there keeps the loop stable
// when it does.
constexpr double max_error_lsb = 1.5;

} // namespace

std::span<const double> dither_taps(DitherShape shape, std::uint32_t sample_rate) noexcept {
  switch (shape) {
  case DitherShape::flat:
    return {};
  case DitherShape::shaped:
  case DitherShape::psychoacoustic:
    if (sample_rate > base_rate_limit) {
      return sample_rate <= rate_88k ? std::span<const double>(high_rate_88k)
                                     : std::span<const double>(high_rate_96k);
    }
    return shape == DitherShape::shaped ? std::span<const double>(e_weighted)
                                        : std::span<const double>(f_weighted);
  }
  return {};
}

DitherQuantizer::DitherQuantizer(unsigned bits, DitherShape shape, std::uint32_t sample_rate,
                                 std::uint64_t seed) noexcept
    : taps_(dither_taps(shape, sample_rate)),
      scale_(static_cast<double>(std::uint32_t{1} << (bits - 1U))), random_(seed) {}

std::int32_t DitherQuantizer::quantize(float sample, std::size_t channel) noexcept {
  auto& errors = errors_[channel];
  double feedback = 0.0;
  for (std::size_t k = 0; k < taps_.size(); ++k) {
    feedback += taps_[k] * errors[k];
  }
  const double wanted = static_cast<double>(sample) * scale_ - feedback;
  // Two draws, in this order: the operands of a subtraction are unsequenced.
  const double first = random_.unit();
  const double second = random_.unit();
  const double noise = first - second;
  const double rounded = std::round(wanted + noise);
  const double clamped = std::clamp(rounded, -scale_, scale_ - 1.0);
  const double error = std::clamp(clamped - wanted, -max_error_lsb, max_error_lsb);
  for (std::size_t k = taps_.size(); k-- > 1;) {
    errors[k] = errors[k - 1];
  }
  if (!taps_.empty()) {
    errors[0] = error;
  }
  return static_cast<std::int32_t>(clamped);
}

} // namespace oscillate::exporting
