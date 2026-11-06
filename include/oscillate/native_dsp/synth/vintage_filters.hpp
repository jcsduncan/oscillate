#pragma once

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/filter.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace oscillate::native_dsp::synth {

inline constexpr double max_ota_feedback = 4.3; // Above 4 the cascade self-oscillates.

// Four OTA-style one-pole stages with a resolved global feedback path, after
// the IR3109-family cascades of classic polys. Unlike ZdfLadder, the early
// stage inputs saturate gently, resonance is uncompensated below 1 so it thins the
// bass like the hardware, and the stages stay linear for small signals.
struct OtaCoefficients {
  LadderCoefficients ladder{};
  double stage_drive = 1.0; // > 0; higher values saturate stages earlier.
  double stage_makeup = 1.0;

  [[nodiscard]] static OtaCoefficients make(double g, double resonance,
                                            double stage_drive) noexcept {
    OtaCoefficients c;
    const double r = resonance > 0.0 ? (resonance < 1.0 ? resonance : 1.0) : 0.0;
    c.ladder = LadderCoefficients::make(g, r * max_ota_feedback);
    // A quarter of the classic compensation: resonance still costs bass.
    c.ladder.input_gain = 1.0 + 0.25 * c.ladder.feedback;
    c.stage_drive = stage_drive > 0.05 ? stage_drive : 0.05;
    c.stage_makeup = 1.0 / c.stage_drive;
    return c;
  }
};

template <typename T> class BasicOtaLadder {
public:
  void reset() noexcept { s1_ = s2_ = s3_ = s4_ = T{}; }

  [[nodiscard]] T process(T x, const OtaCoefficients& c) noexcept {
    const auto& l = c.ladder;
    const T tail = l.hold * (l.gain * l.g2 * s1_ + l.g2 * s2_ + l.gain * s3_ + s4_);
    const T u = soft_clip((x * l.input_gain - l.feedback * tail) * l.solve_scale);
    // Saturate all four OTA stages to retain the measured ladder character.
    const T y1 = stage(saturate(u, c), l.gain, s1_);
    const T y2 = stage(saturate(y1, c), l.gain, s2_);
    const T y3 = stage(saturate(y2, c), l.gain, s3_);
    return stage(saturate(y3, c), l.gain, s4_);
  }

private:
  [[nodiscard]] static T saturate(T x, const OtaCoefficients& c) noexcept {
    return soft_clip(x * c.stage_drive) * c.stage_makeup;
  }
  [[nodiscard]] static T stage(T x, double gain, T& state) noexcept {
    const T v = (x - state) * gain;
    const T y = v + state;
    state = y + v;
    return y;
  }

  T s1_{}, s2_{}, s3_{}, s4_{};
};

// 12 dB state-variable filter with a continuous LP -> notch -> HP morph, in
// the spirit of the classic two-pole multimode modules.
struct SemCoefficients {
  SvfCoefficients svf{};
  double morph = 0.0; // 0 low-pass, 0.5 notch, 1 high-pass.

  [[nodiscard]] static SemCoefficients make(double g, double resonance, double morph) noexcept {
    SemCoefficients c;
    c.svf = SvfCoefficients::make(
        g, 1.0 / resonance_q(resonance, oscillate::native_dsp::dsp::butterworth_q));
    c.morph = morph > 0.0 ? (morph < 1.0 ? morph : 1.0) : 0.0;
    return c;
  }
};

template <typename T> class BasicSemFilter {
public:
  void reset() noexcept { svf_.reset(); }

  [[nodiscard]] T process(T x, const SemCoefficients& c) noexcept {
    const auto out = svf_.process(x, c.svf);
    const T notch = out.low + out.high;
    if (c.morph <= 0.5) {
      const double t = 2.0 * c.morph;
      return out.low * (1.0 - t) + notch * t;
    }
    const double t = 2.0 * c.morph - 1.0;
    return notch * (1.0 - t) + out.high * t;
  }

private:
  BasicZdfSvf<T> svf_;
};

// Four-position non-resonant high-pass: 0 bypasses, then 2-pole Butterworth
// corners at 120, 300 and 720 Hz. All three filters stay warm, so both
// bypass and active-position changes can crossfade without a coefficient step.
inline constexpr std::array<double, 4> hpf_position_hz{0.0, 120.0, 300.0, 720.0};

template <typename T> class BasicHighPass4 {
public:
  void prepare(double sample_rate) noexcept {
    rate_ = sample_rate;
    blend_step_ = 1.0 / (0.01 * sample_rate);
    for (std::size_t i = 0; i < filters_.size(); ++i) {
      filters_[i].reset();
      coefficients_[i] =
          SvfCoefficients::make(prewarp(hpf_position_hz[i + 1] / rate_), std::numbers::sqrt2);
    }
    weights_ = {};
    weights_[position_ > 0 ? static_cast<std::size_t>(position_ - 1) : 0] = 1.0;
    from_weights_ = weights_;
    target_weights_ = weights_;
    transition_ = 1.0;
    target_blend_ = position_ > 0 ? 1.0 : 0.0;
    blend_ = target_blend_;
  }
  void reset() noexcept {
    for (auto& filter : filters_) {
      filter.reset();
    }
  }
  void set_position(int position) noexcept {
    const int next = position < 0 ? 0 : (position > 3 ? 3 : position);
    if (next == position_) {
      return;
    }
    position_ = next;
    if (position_ > 0) {
      from_weights_ = weights_;
      target_weights_ = {};
      target_weights_[static_cast<std::size_t>(position_ - 1)] = 1.0;
      transition_ = 0.0;
    }
    target_blend_ = position_ > 0 ? 1.0 : 0.0;
  }

  [[nodiscard]] T process(T x) noexcept {
    // The filter states stay warm even during bypass and repeated retargets.
    std::array<T, 3> high{};
    for (std::size_t i = 0; i < filters_.size(); ++i) {
      high[i] = filters_[i].process(x, coefficients_[i]).high;
    }
    if (transition_ < 1.0) {
      transition_ = std::min(1.0, transition_ + blend_step_);
      for (std::size_t i = 0; i < weights_.size(); ++i) {
        weights_[i] = from_weights_[i] + (target_weights_[i] - from_weights_[i]) * transition_;
      }
    }
    const T selected = high[0] * weights_[0] + high[1] * weights_[1] + high[2] * weights_[2];
    if (blend_ != target_blend_) {
      blend_ = target_blend_ > blend_ ? std::min(target_blend_, blend_ + blend_step_)
                                      : std::max(target_blend_, blend_ - blend_step_);
    }
    return x * (1.0 - blend_) + selected * blend_;
  }

private:
  std::array<BasicZdfSvf<T>, 3> filters_{};
  std::array<SvfCoefficients, 3> coefficients_{};
  std::array<double, 3> weights_{1.0, 0.0, 0.0};
  std::array<double, 3> from_weights_{1.0, 0.0, 0.0};
  std::array<double, 3> target_weights_{1.0, 0.0, 0.0};
  double rate_ = 48'000.0;
  int position_ = 0;
  double blend_ = 0.0;
  double target_blend_ = 0.0;
  double transition_ = 1.0;
  double blend_step_ = 0.002;
};

using OtaLadder = BasicOtaLadder<double>;
using StereoOtaLadder = BasicOtaLadder<StereoSample>;
using SemFilter = BasicSemFilter<double>;
using StereoSemFilter = BasicSemFilter<StereoSample>;
using HighPass4 = BasicHighPass4<double>;
using StereoHighPass4 = BasicHighPass4<StereoSample>;

} // namespace oscillate::native_dsp::synth
