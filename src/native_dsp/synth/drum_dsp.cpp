#include "oscillate/native_dsp/synth/drum_dsp.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace oscillate::native_dsp::synth {
namespace {

[[nodiscard]] double gain_db(double gain) noexcept {
  return 20.0 * std::log10(std::max(gain, 1e-12));
}

} // namespace

// ---- Glue compressor ----------------------------------------------------------

void GlueCompressor::prepare(double sample_rate) noexcept {
  rate_ = sample_rate;
  attack_ = 1.0 - std::exp(-1.0 / (0.01 * sample_rate));
  set(0.0, 0.2);
  reset();
}

void GlueCompressor::set(double amount, double release_seconds) noexcept {
  amount_ = std::clamp(amount, 0.0, 1.0);
  threshold_db_ = -24.0 * amount_;
  ratio_ = 1.0 + 3.0 * amount_;
  release_ = 1.0 - std::exp(-1.0 / (std::clamp(release_seconds, 0.02, 2.0) * rate_));
  // Half the full-scale reduction comes back as makeup.
  makeup_ = std::pow(10.0, 0.5 * -threshold_db_ * (1.0 - 1.0 / ratio_) / 20.0);
}

StereoOut GlueCompressor::process(double left, double right) noexcept {
  // The envelope tracks even while off, so turning the glue back up starts
  // from the signal as it is now, not from before.
  const double level = std::max(std::fabs(left), std::fabs(right));
  envelope_ += (level - envelope_) * (level > envelope_ ? attack_ : release_);
  if (amount_ <= 0.0) {
    reduction_db_ = 0.0;
    return {left, right};
  }
  const double over = gain_db(envelope_) - threshold_db_;
  constexpr double knee = 6.0;
  double reduction = 0.0;
  if (over > knee / 2.0) {
    reduction = over * (1.0 - 1.0 / ratio_);
  } else if (over > -knee / 2.0) {
    const double x = over + knee / 2.0;
    reduction = (1.0 - 1.0 / ratio_) * x * x / (2.0 * knee);
  }
  reduction_db_ = reduction;
  const double gain = std::pow(10.0, -reduction / 20.0) * makeup_;
  return {left * gain, right * gain};
}

} // namespace oscillate::native_dsp::synth
