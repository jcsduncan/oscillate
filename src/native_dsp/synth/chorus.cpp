#include "oscillate/native_dsp/synth/chorus.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

namespace oscillate::native_dsp::synth {
namespace {

struct ModeShape {
  double rate_hz;
  double low_ms;
  double high_ms;
};

// Published Juno-60 chorus measurements: LFO rate and BBD delay sweep.
constexpr ModeShape mode_shape(ChorusMode mode) noexcept {
  switch (mode) {
  case ChorusMode::one:
    return {0.513, 1.66, 5.35};
  case ChorusMode::two:
    return {0.863, 1.66, 5.35};
  case ChorusMode::both:
    return {9.75, 3.3, 3.7};
  case ChorusMode::off:
    break;
  }
  return {0.513, 1.66, 5.35};
}

constexpr double glide_seconds = 0.05;

} // namespace

void Chorus::prepare(double sample_rate) {
  rate_ = sample_rate;
  std::size_t size = 1;
  while (static_cast<double>(size) < max_delay_seconds * sample_rate + 8.0) {
    size <<= 1U;
  }
  left_line_.assign(size, 0.0F);
  right_line_.assign(size, 0.0F);
  mask_ = size - 1;
  // The BBD's anti-imaging filter darkens the wet path around 9 kHz.
  const double corner = std::min(9'000.0, 0.4 * sample_rate);
  tone_coefficient_ = native_dsp::dsp::one_pole_coefficient(corner, sample_rate);
  reset();
}

void Chorus::reset() noexcept {
  std::fill(left_line_.begin(), left_line_.end(), 0.0F);
  std::fill(right_line_.begin(), right_line_.end(), 0.0F);
  write_ = 0;
  lfo_phase_ = 0.0;
  tone_left_ = 0.0;
  tone_right_ = 0.0;
  const ModeShape shape = mode_shape(mode_);
  lfo_increment_ = shape.rate_hz / rate_;
  centre_.reset(0.5 * (shape.low_ms + shape.high_ms) * 0.001 * rate_);
  depth_.reset(0.5 * (shape.high_ms - shape.low_ms) * 0.001 * rate_);
  wet_.reset(mode_ == ChorusMode::off ? 0.0 : 0.5 * mix_);
}

void Chorus::set(ChorusMode mode, double mix) noexcept {
  mix = mix > 0.0 ? (mix < 1.0 ? mix : 1.0) : 0.0;
  const auto frames = static_cast<std::uint32_t>(glide_seconds * rate_);
  if (mode != ChorusMode::off) {
    const ModeShape shape = mode_shape(mode);
    lfo_increment_ = shape.rate_hz / rate_;
    centre_.set_target(0.5 * (shape.low_ms + shape.high_ms) * 0.001 * rate_, frames);
    depth_.set_target(0.5 * (shape.high_ms - shape.low_ms) * 0.001 * rate_, frames);
  }
  wet_.set_target(mode == ChorusMode::off ? 0.0 : 0.5 * mix, frames);
  mode_ = mode;
  mix_ = mix;
}

double Chorus::read(const std::vector<float>& line, double delay_frames) const noexcept {
  // Cubic Hermite interpolation between the four frames around the tap.
  const double position = static_cast<double>(write_) - delay_frames;
  const double floor_position = fast_floor(position);
  const double t = position - floor_position;
  const auto base = static_cast<std::size_t>(static_cast<std::int64_t>(floor_position));
  const double y0 = line[(base - 1) & mask_];
  const double y1 = line[base & mask_];
  const double y2 = line[(base + 1) & mask_];
  const double y3 = line[(base + 2) & mask_];
  const double c1 = 0.5 * (y2 - y0);
  const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
  const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
  return ((c3 * t + c2) * t + c1) * t + y1;
}

Chorus::Frame Chorus::process(double left, double right) noexcept {
  left_line_[write_ & mask_] = static_cast<float>(left);
  right_line_[write_ & mask_] = static_cast<float>(right);

  lfo_phase_ += lfo_increment_;
  if (lfo_phase_ >= 1.0) {
    lfo_phase_ -= 1.0;
  }
  const double triangle = lfo_phase_ < 0.5 ? 4.0 * lfo_phase_ - 1.0 : 3.0 - 4.0 * lfo_phase_;
  const double centre = centre_.next();
  const double depth = depth_.next();
  const double wet = wet_.next();
  if (wet == 0.0 && wet_.settled()) {
    // Off: keep the lines filling so switching on is seamless; skip reads.
    ++write_;
    return {left, right};
  }

  tone_left_ += (read(left_line_, centre + depth * triangle) - tone_left_) * tone_coefficient_;
  tone_right_ += (read(right_line_, centre - depth * triangle) - tone_right_) * tone_coefficient_;
  ++write_;
  return {left + wet * (tone_left_ - left), right + wet * (tone_right_ - right)};
}

} // namespace oscillate::native_dsp::synth
