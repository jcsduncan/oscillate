#include "oscillate/native_dsp/correlation.hpp"

#include <algorithm>
#include <cmath>

namespace oscillate::native_dsp {
namespace {
// -100 dBFS as a mean square.
constexpr double silence_power = 1e-10;
} // namespace

void StereoCorrelationMeter::prepare(double sample_rate, double time_constant_seconds) noexcept {
  coefficient_ = sample_rate > 0.0 && time_constant_seconds > 0.0
                     ? 1.0 - std::exp(-1.0 / (sample_rate * time_constant_seconds))
                     : 1.0;
  reset();
}

void StereoCorrelationMeter::reset() noexcept {
  left_power_ = 0.0;
  right_power_ = 0.0;
  cross_ = 0.0;
}

void StereoCorrelationMeter::process(std::span<const engine::StereoFrame> frames) noexcept {
  for (const auto& frame : frames) {
    const auto left = static_cast<double>(frame.left);
    const auto right = static_cast<double>(frame.right);
    // A non-finite sample says nothing about phase; it is skipped.
    if (!std::isfinite(left) || !std::isfinite(right))
      continue;
    left_power_ += coefficient_ * (left * left - left_power_);
    right_power_ += coefficient_ * (right * right - right_power_);
    cross_ += coefficient_ * (left * right - cross_);
  }
}

std::optional<double> StereoCorrelationMeter::correlation() const noexcept {
  if (left_power_ < silence_power && right_power_ < silence_power)
    return std::nullopt;
  // One silent side is unrelated to the other, not undefined.
  const auto norm = std::sqrt(left_power_ * right_power_);
  if (norm <= 0.0)
    return 0.0;
  return std::clamp(cross_ / norm, -1.0, 1.0);
}

std::optional<double> StereoCorrelationMeter::mono_loss_db() const noexcept {
  const auto mean = 0.5 * (left_power_ + right_power_);
  if (mean < silence_power)
    return std::nullopt;
  // Mono is (L + R) / 2 at -6 dB per side, so a mono source folds to itself:
  // its power, (L² + 2LR + R²) / 4, over the mean (L² + R²) / 2.
  const auto mono = 0.25 * (left_power_ + 2.0 * cross_ + right_power_);
  return 10.0 * std::log10(std::max(mono / mean, 1e-12));
}

} // namespace oscillate::native_dsp
