#include "effects/dust/age.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace oscillate::native_dsp::effects::dust::detail {

void Age::prepare(double rate, std::uint32_t latency) {
  rate_ = rate;
  latency_ = latency;
  // Wide enough for the base delay and the deepest wow and flutter.
  for (auto& line : delay_) {
    line.prepare(2 * static_cast<std::size_t>(latency) + delay_spare_frames);
  }
  for (auto& f : dark_) {
    f.set(dropout_dark_hz, rate);
  }
  wow_frames_ = wow_max_deviation / (two_pi * wow_hz) * rate;
  double flutter_slope = 0.0;
  for (std::size_t i = 0; i < flutter_hz.size(); ++i) {
    flutter_slope += flutter_share[i] * two_pi * flutter_hz[i];
  }
  flutter_frames_ = flutter_max_deviation / (flutter_slope * (1.0 + flutter_wander)) * rate;
}

void Age::reset() noexcept {
  for (auto& line : delay_) {
    line.reset();
  }
  for (std::size_t c = 0; c < channels; ++c) {
    high_[c].reset();
    low_[c].reset();
    dark_[c].reset();
  }
  for (auto& f : mono_high_) {
    f.reset();
  }
  wow_drift_.reset();
  flutter_wander_.reset();
}

void Age::set(std::span<const double> v, double steps, bool jump) noexcept {
  const double amount = v[p::age_amount];
  if (amount != amount_set_) {
    amount_set_ = amount;
    const double high = age_high_hz * std::pow(age_high_ratio, amount);
    const double low =
        std::min(age_low_hz * std::pow(age_low_ratio, amount), nyquist_guard * rate_);
    for (std::size_t c = 0; c < channels; ++c) {
      high_[c].set(high, butterworth_q, rate_);
      low_[c].set(low, butterworth_q, rate_);
    }
  }
  blend_.to(std::min(1.0, age_blend * amount), steps, jump);
  drive_ = 1.0 + age_drive * amount;
  wow_.to(v[p::age_wow] * wow_frames_, steps, jump);
  flutter_.to(v[p::age_flutter] * flutter_frames_, steps, jump);
  const double mono = v[p::age_mono];
  mono_on_ = mono > t::mono_off_hz;
  if (mono_on_ && mono != mono_set_) {
    mono_set_ = mono;
    for (auto& f : mono_high_) {
      f.set(mono, butterworth_q, rate_);
    }
  }
}

} // namespace oscillate::native_dsp::effects::dust::detail
