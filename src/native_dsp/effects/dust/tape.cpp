#include "effects/dust/tape.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace oscillate::native_dsp::effects::dust::detail {

void Tape::reset() noexcept {
  for (std::size_t c = 0; c < channels; ++c) {
    high_[c].reset();
    peak_[c].reset();
    low_[c].reset();
  }
  drop_left_ = 0;
  envelope_ = 1.0;
}

void Tape::set(std::span<const double> v, double tone, double width) noexcept {
  if (tone != tone_) {
    tone_ = tone;
    const double peak = tape_peak_low_hz * std::pow(tape_peak_ratio, tone);
    const double high = peak * tape_hp_ratio;
    const double low = std::min(peak * tape_lp_ratio, nyquist_guard * rate_);
    for (std::size_t c = 0; c < channels; ++c) {
      high_[c].set(high, rate_);
      peak_[c].set(peak, tape_peak_q, rate_);
      low_[c].set(low, butterworth_q, rate_);
    }
    const std::array chain{high1(high), peak2(peak, tape_peak_q, tape_peak_gain),
                           low2(low, butterworth_q)};
    power_ = noise_power(chain, rate_);
  }
  hiss_gain_ = reference_ * std::sqrt(std::max(v[p::tape_hiss], 0.0) / def(p::tape_hiss) / power_);
  const double dropouts = v[p::tape_dropouts];
  drop_chance_ = tape_dropout_rate * dropouts * dropouts / rate_;
  drop_depth_db_ = tape_drop_depth_span_db * dropouts;
  width_.set(width);
}

} // namespace oscillate::native_dsp::effects::dust::detail
