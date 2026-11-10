#include "oscillate/native_dsp/tonal_targets.hpp"

#include "dsp/biquad.hpp"
#include "dsp/bs1770.hpp"

#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>

namespace oscillate::native_dsp::tonal {
namespace {

// The K-weighting's published design rate: its response at this rate is
// the standard's (BS.1770 Annex 1, Tables 1 and 2).
constexpr double k_design_rate = 48'000.0;
constexpr double half_band_octaves = 0.5 / bands_per_octave;

[[nodiscard]] double response_power(const dsp::BiquadCoefficients& c, double omega) noexcept {
  const std::complex<double> z1 = std::polar(1.0, -omega);
  const std::complex<double> z2 = z1 * z1;
  const auto numerator = c.b0 + c.b1 * z1 + c.b2 * z2;
  const auto denominator = 1.0 + c.a1 * z1 + c.a2 * z2;
  return std::norm(numerator) / std::norm(denominator);
}

} // namespace

double band_hz(std::size_t band) noexcept {
  return pivot_hz * std::exp2((static_cast<double>(first_band_step) + static_cast<double>(band)) /
                              bands_per_octave);
}

double band_low_hz(std::size_t band) noexcept {
  return band_hz(band) * std::exp2(-half_band_octaves);
}

double band_high_hz(std::size_t band) noexcept {
  return band_hz(band) * std::exp2(half_band_octaves);
}

double k_weighting_db(double hz) noexcept {
  const double omega = 2.0 * std::numbers::pi * hz / k_design_rate;
  const double power = response_power(dsp::bs1770::k_shelf(k_design_rate), omega) *
                       response_power(dsp::bs1770::k_high_pass(k_design_rate), omega);
  return 10.0 * std::log10(power);
}

double loudness_db(const Curve& levels) noexcept {
  double sum = 0.0;
  for (std::size_t b = 0; b < bands; ++b) {
    sum += std::pow(10.0, (levels[b] + k_weighting_db(band_hz(b))) / 10.0);
  }
  return 10.0 * std::log10(sum);
}

Curve normalised(const Curve& levels) noexcept {
  const double offset = loudness_db(levels);
  Curve out{};
  for (std::size_t b = 0; b < bands; ++b) {
    out[b] = std::isfinite(offset) ? levels[b] - offset : levels[b];
  }
  return out;
}

const Target* find_factory_target(std::string_view id) {
  for (const auto& target : factory_targets()) {
    if (target.id == id) {
      return &target;
    }
  }
  return nullptr;
}

Placements assess(const Curve& normalised_levels, const Target& target) noexcept {
  Placements out{};
  for (std::size_t b = 0; b < bands; ++b) {
    const double level = normalised_levels[b];
    out[b] = level < target.low[b]    ? Placement::below
             : level > target.high[b] ? Placement::above
                                      : Placement::inside;
  }
  return out;
}

} // namespace oscillate::native_dsp::tonal
