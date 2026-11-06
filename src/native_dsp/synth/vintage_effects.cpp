#include "oscillate/native_dsp/synth/vintage_effects.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace oscillate::native_dsp::synth {
void DelayLine::prepare(std::size_t max_frames) {
  std::size_t size = 8;
  while (size < max_frames + 8) {
    size <<= 1U;
  }
  buffer_.assign(size, 0.0F);
  mask_ = size - 1;
  write_ = 0;
}

void DelayLine::reset() noexcept {
  std::fill(buffer_.begin(), buffer_.end(), 0.0F);
  write_ = 0;
}

double DelayLine::read(double delay_frames) const noexcept {
  delay_frames = std::clamp(delay_frames, 1.0, static_cast<double>(capacity()));
  const double position = static_cast<double>(write_) - delay_frames;
  const double floor_position = fast_floor(position);
  const double t = position - floor_position;
  const auto base = static_cast<std::size_t>(static_cast<std::int64_t>(floor_position));
  const double y0 = buffer_[(base - 1) & mask_];
  const double y1 = buffer_[base & mask_];
  const double y2 = buffer_[(base + 1) & mask_];
  const double y3 = buffer_[(base + 2) & mask_];
  const double c1 = 0.5 * (y2 - y0);
  const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
  const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
  return ((c3 * t + c2) * t + c1) * t + y1;
}

// ---- Comb ----------------------------------------------------------------------

void CombFilter::prepare(double sample_rate, double min_hz) {
  rate_ = sample_rate;
  line_.prepare(static_cast<std::size_t>(std::ceil(sample_rate / min_hz)) + 4);
  delay_.reset(sample_rate / 440.0);
  reset();
}

void CombFilter::reset() noexcept {
  line_.reset();
  last_ = 0.0;
  delay_.reset(delay_.target());
  feedback_.reset(feedback_.target());
}

void CombFilter::set(double hz, double feedback, std::uint32_t ramp_frames) noexcept {
  hz = std::clamp(hz, 20.0, native_dsp::dsp::nyquist_guard * rate_);
  delay_.set_target(rate_ / hz, ramp_frames);
  feedback_.set_target(std::clamp(feedback, -0.98, 0.98), ramp_frames);
}

double CombFilter::process(double x) noexcept {
  const double delay = delay_.next();
  const double feedback = feedback_.next();
  // DelayLine::read(d) lags by d - 1 frames, so add one for an exact period.
  const double echo = line_.read(delay + 1.0);
  const double y = x + feedback * echo;
  line_.write(soft_clip(y));
  // Peak gain is 1 / (1 - |feedback|); scale it back to roughly unity.
  last_ = y * (1.0 - std::fabs(feedback));
  return last_;
}

// ---- Ensemble ----------------------------------------------------------------

void Ensemble::prepare(double sample_rate) {
  rate_ = sample_rate;
  line_.prepare(static_cast<std::size_t>(std::ceil(0.016 * sample_rate)));
  reset();
}

void Ensemble::reset() noexcept {
  line_.reset();
  slow_phase_ = 0.0;
  fast_phase_ = 0.0;
  mix_.reset(mix_.target());
}

StereoOut Ensemble::process(double left, double right) noexcept {
  line_.write(0.5 * (left + right));
  slow_phase_ += 0.63 / rate_;
  fast_phase_ += 5.7 / rate_;
  slow_phase_ -= fast_floor(slow_phase_);
  fast_phase_ -= fast_floor(fast_phase_);
  const double mix = mix_.next();
  if (mix == 0.0 && mix_.settled()) {
    return {left, right};
  }
  const double centre = 0.0055 * rate_;
  const double slow_depth = 0.0016 * rate_;
  const double fast_depth = 0.00022 * rate_;
  double taps[3];
  for (int tap = 0; tap < 3; ++tap) {
    const double offset = static_cast<double>(tap) / 3.0;
    taps[tap] = line_.read(centre + slow_depth * sin_cycle(slow_phase_ + offset) +
                           fast_depth * sin_cycle(fast_phase_ + offset));
  }
  const double wet_left = (taps[0] + 0.5 * taps[1]) / 1.5;
  const double wet_right = (taps[2] + 0.5 * taps[1]) / 1.5;
  return {left + mix * (wet_left - left), right + mix * (wet_right - right)};
}

// ---- BBD delay ---------------------------------------------------------------

void BbdDelay::prepare(double sample_rate) {
  rate_ = sample_rate;
  const auto frames = static_cast<std::size_t>(std::ceil(max_seconds * sample_rate)) + 4;
  left_.prepare(frames);
  right_.prepare(frames);
  // Each repeat loses top end, like a bucket-brigade line.
  tone_coefficient_ =
      native_dsp::dsp::one_pole_coefficient(std::min(4'500.0, 0.4 * sample_rate), sample_rate);
  dc_left_.prepare(sample_rate, 20.0);
  dc_right_.prepare(sample_rate, 20.0);
  time_.reset(0.375 * sample_rate);
  reset();
}

void BbdDelay::reset() noexcept {
  left_.reset();
  right_.reset();
  tone_left_ = 0.0;
  tone_right_ = 0.0;
  dc_left_.reset();
  dc_right_.reset();
  time_.reset(time_.target());
  feedback_.reset(feedback_.target());
  mix_.reset(mix_.target());
  ping_pong_.reset(ping_pong_.target());
}

void BbdDelay::set(double seconds, double feedback, double mix, bool ping_pong,
                   std::uint32_t ramp_frames) noexcept {
  seconds = std::clamp(seconds, 0.001, max_seconds);
  const double routing = ping_pong ? 1.0 : 0.0;
  if (!primed_) {
    // The first setting applies at once: no glide from the prepared default.
    time_.reset(seconds * rate_);
    feedback_.reset(std::clamp(feedback, 0.0, 0.95));
    mix_.reset(std::clamp(mix, 0.0, 1.0));
    ping_pong_.reset(routing);
    primed_ = true;
    return;
  }
  // Time glides over at least 50 ms so a change bends pitch rather than clicks.
  time_.set_target(seconds * rate_,
                   std::max(ramp_frames, static_cast<std::uint32_t>(0.05 * rate_)));
  feedback_.set_target(std::clamp(feedback, 0.0, 0.95), ramp_frames);
  mix_.set_target(std::clamp(mix, 0.0, 1.0), ramp_frames);
  ping_pong_.set_target(
      routing, std::max(ramp_frames, static_cast<std::uint32_t>(ping_pong_seconds * rate_)));
}

StereoOut BbdDelay::process(double left, double right) noexcept {
  const double delay = time_.next();
  const double feedback = feedback_.next();
  const double mix = mix_.next();
  if (mix == 0.0 && mix_.settled() && feedback_.settled()) {
    // Unheard: keep recording the dry input, skip the repeat path.
    left_.write(left);
    right_.write(right);
    tone_left_ = 0.0;
    tone_right_ = 0.0;
    return {left, right};
  }
  const double echo_left = left_.read(delay);
  const double echo_right = right_.read(delay);
  tone_left_ += (echo_left - tone_left_) * tone_coefficient_;
  tone_right_ += (echo_right - tone_right_) * tone_coefficient_;
  const double back_left = dc_left_.process(soft_clip(tone_left_ * feedback));
  const double back_right = dc_right_.process(soft_clip(tone_right_ * feedback));
  const double pong = ping_pong_.next();
  if (pong == 1.0) {
    // Mono input enters the left line; repeats alternate sides.
    left_.write(0.5 * (left + right) + back_right);
    right_.write(back_left);
  } else if (pong == 0.0) {
    left_.write(left + back_left);
    right_.write(right + back_right);
  } else {
    const double straight = 1.0 - pong;
    left_.write(pong * (0.5 * (left + right) + back_right) + straight * (left + back_left));
    right_.write(pong * back_left + straight * (right + back_right));
  }
  return {left + mix * tone_left_, right + mix * tone_right_};
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
double BbdDelay::tail_seconds(double seconds, double feedback) noexcept {
  feedback = std::clamp(feedback, 0.0, 0.95);
  if (feedback < 1e-6) {
    return seconds;
  }
  const double repeats = std::log(1e-4) / std::log(feedback);
  return std::min(60.0, seconds * (repeats + 1.0));
}

// ---- Tape stage --------------------------------------------------------------

void TapeStage::prepare(double sample_rate) {
  rate_ = sample_rate;
  const auto frames = static_cast<std::size_t>(std::ceil(0.006 * sample_rate));
  left_.prepare(frames);
  right_.prepare(frames);
  reset();
}

void TapeStage::reset() noexcept {
  left_.reset();
  right_.reset();
  wow_phase_ = 0.0;
  flutter_phase_ = 0.0;
  drive_.reset(drive_.target());
  wobble_.reset(wobble_.target());
}

double TapeStage::saturate(double x, double gain, double bias) noexcept {
  // Level-matched for small signals; the bias adds the even harmonics tape is
  // loved for.
  return (soft_clip(x * gain + bias) - soft_clip(bias)) / gain;
}

StereoOut TapeStage::process(double left, double right) noexcept {
  const double drive = drive_.next();
  const double wobble = wobble_.next();
  if (drive > 0.0) {
    // Up to 18 dB into a slightly asymmetric curve.
    const double gain = std::pow(10.0, 18.0 * drive / 20.0);
    const double bias = 0.12 * drive;
    left = saturate(left, gain, bias);
    right = saturate(right, gain, bias);
  }
  left_.write(left);
  right_.write(right);
  wow_phase_ += 0.55 / rate_;
  flutter_phase_ += 6.3 / rate_;
  wow_phase_ -= fast_floor(wow_phase_);
  flutter_phase_ -= fast_floor(flutter_phase_);
  // Always read through the short delay: a constant latency, so wobble can
  // rise from zero without combing against a dry path.
  const double sweep = wobble * (0.0012 * rate_ * sin_cycle(wow_phase_) +
                                 0.00012 * rate_ * sin_cycle(flutter_phase_));
  return {left_.read(centre_frames() + sweep), right_.read(centre_frames() + sweep * 0.97)};
}

} // namespace oscillate::native_dsp::synth
