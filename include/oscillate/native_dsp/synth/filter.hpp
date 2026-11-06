#pragma once

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"

#include <cmath>
#include <cstdint>
#include <numbers>

namespace oscillate::native_dsp::synth {

enum class FilterMode : std::uint8_t {
  lowpass_12,
  lowpass_24,
  bandpass_12,
  bandpass_24,
  highpass_12,
  highpass_24,
  notch_12,
  notch_24,
  ladder_24
};

inline constexpr double min_filter_q = 0.5;
inline constexpr double max_filter_q = 40.0;
inline constexpr double max_ladder_feedback = 4.2; // Above 4 the ladder self-oscillates.

// Normalised cutoff is Hz divided by the rate the filter runs at. It is
// clamped below Nyquist so prewarping stays finite.
[[nodiscard]] inline double prewarp(double normalised_cutoff) noexcept {
  const double clamped = normalised_cutoff > 1e-6
                             ? (normalised_cutoff < oscillate::native_dsp::dsp::nyquist_guard
                                    ? normalised_cutoff
                                    : oscillate::native_dsp::dsp::nyquist_guard)
                             : 1e-6;
  return std::tan(std::numbers::pi * clamped);
}

// Resonance 0..1 maps exponentially from a flat to a sharply resonant Q.
[[nodiscard]] inline double resonance_q(double resonance, double flat_q) noexcept {
  const double r = resonance > 0.0 ? (resonance < 1.0 ? resonance : 1.0) : 0.0;
  return flat_q * std::pow(max_filter_q / flat_q, r);
}

// Per-sample coefficients, derived once and shared by every channel and
// oversampled frame that uses them.
struct SvfCoefficients {
  double k = 1.0; // Damping, 1 / Q.
  double a1 = 1.0;
  double a2 = 0.0;
  double a3 = 0.0;
  [[nodiscard]] static SvfCoefficients make(double g, double k) noexcept {
    SvfCoefficients c;
    c.k = k;
    c.a1 = 1.0 / (1.0 + g * (g + k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    return c;
  }
};

struct LadderCoefficients {
  double gain = 0.0; // g / (1 + g)
  double g2 = 0.0;
  double hold = 1.0; // 1 / (1 + g)
  double feedback = 0.0;
  double input_gain = 1.0;  // Partial passband compensation.
  double solve_scale = 1.0; // 1 / (1 + feedback * gain^4)
  [[nodiscard]] static LadderCoefficients make(double g, double feedback) noexcept {
    LadderCoefficients c;
    c.hold = 1.0 / (1.0 + g);
    c.gain = g * c.hold;
    c.g2 = c.gain * c.gain;
    c.feedback = feedback;
    c.input_gain = 1.0 + 0.5 * feedback;
    c.solve_scale = 1.0 / (1.0 + feedback * c.g2 * c.g2);
    return c;
  }
};

// Trapezoidal (zero-delay-feedback) state-variable filter, after Simper.
// Coefficients may change every sample without instability. T is double or
// StereoSample.
template <typename T> class BasicZdfSvf {
public:
  struct Outputs {
    T low;
    T band;
    T high;
  };

  void reset() noexcept {
    ic1_ = T{};
    ic2_ = T{};
  }

  [[nodiscard]] Outputs process(T x, const SvfCoefficients& c) noexcept {
    const T v3 = x - ic2_;
    const T v1 = c.a1 * ic1_ + c.a2 * v3;
    const T v2 = ic2_ + c.a2 * ic1_ + c.a3 * v3;
    ic1_ = 2.0 * v1 - ic1_;
    ic2_ = 2.0 * v2 - ic2_;
    return {v2, v1, x - c.k * v1 - v2};
  }

private:
  T ic1_{};
  T ic2_{};
};

// Four trapezoidal one-pole stages with resolved global feedback and a
// saturating input, after Zavalishin. Feedback 0..max_ladder_feedback.
template <typename T> class BasicZdfLadder {
public:
  void reset() noexcept { s1_ = s2_ = s3_ = s4_ = T{}; }

  [[nodiscard]] T process(T x, const LadderCoefficients& c) noexcept {
    const T tail = c.hold * (c.gain * c.g2 * s1_ + c.g2 * s2_ + c.gain * s3_ + s4_);
    const T u = soft_clip((x * c.input_gain - c.feedback * tail) * c.solve_scale);
    const T y1 = stage(u, c.gain, s1_);
    const T y2 = stage(y1, c.gain, s2_);
    const T y3 = stage(y2, c.gain, s3_);
    return stage(y3, c.gain, s4_);
  }

private:
  [[nodiscard]] static T stage(T x, double gain, T& state) noexcept {
    const T v = (x - state) * gain;
    const T y = v + state;
    state = y + v;
    return y;
  }

  T s1_{}, s2_{}, s3_{}, s4_{};
};

using ZdfSvf = BasicZdfSvf<double>;
using ZdfLadder = BasicZdfLadder<double>;

struct FilterCoefficients {
  FilterMode mode = FilterMode::lowpass_24;
  double g = 0.0;
  double k1 = 2.0; // Damping (1 / Q) of the first SVF stage.
  double k2 = 2.0; // Damping of the second stage, for 24 dB modes.
  double feedback = 0.0;
  double drive_gain = 1.0;
  double drive_makeup = 1.0;
  // Derived by finalise(); callers that ramp the fields above re-finalise.
  SvfCoefficients first{};
  SvfCoefficients second{};
  LadderCoefficients ladder{};

  void finalise() noexcept {
    if (mode == FilterMode::ladder_24) {
      ladder = LadderCoefficients::make(g, feedback);
      return;
    }
    first = SvfCoefficients::make(g, k1);
    second = SvfCoefficients::make(g, k2);
  }
};

// normalised_cutoff is Hz over the filter's own rate; drive is linear gain >= 1.
[[nodiscard]] inline FilterCoefficients make_filter_coefficients(FilterMode mode,
                                                                 double normalised_cutoff,
                                                                 double resonance,
                                                                 double drive_gain) noexcept {
  FilterCoefficients c;
  c.mode = mode;
  c.g = prewarp(normalised_cutoff);
  const double r = resonance > 0.0 ? (resonance < 1.0 ? resonance : 1.0) : 0.0;
  switch (mode) {
  case FilterMode::ladder_24:
    c.feedback = r * max_ladder_feedback;
    break;
  case FilterMode::lowpass_12:
  case FilterMode::bandpass_12:
  case FilterMode::highpass_12:
  case FilterMode::notch_12:
    c.k1 = 1.0 / resonance_q(r, oscillate::native_dsp::dsp::butterworth_q);
    break;
  default:
    // 24 dB: a Butterworth pair, with resonance on the second stage only.
    c.k1 = 1.0 / 0.5411961;
    c.k2 = 1.0 / resonance_q(r, 1.3065630);
    break;
  }
  c.drive_gain = drive_gain > 1.0 ? drive_gain : 1.0;
  // Drive adds harmonics, and only gently adds loudness.
  c.drive_makeup = (1.0 + std::log(c.drive_gain)) / c.drive_gain;
  c.finalise();
  return c;
}

// The synth filter section: optional pre-drive, any mode. Runs at whatever
// rate it is fed; the voice runs it oversampled, both channels at once.
template <typename T> class BasicMultimodeFilter {
public:
  void reset() noexcept {
    first_.reset();
    second_.reset();
    ladder_.reset();
  }

  [[nodiscard]] T process(T x, const FilterCoefficients& c) noexcept {
    if (c.drive_gain > 1.0) {
      x = soft_clip(x * c.drive_gain) * c.drive_makeup;
    }
    switch (c.mode) {
    case FilterMode::ladder_24:
      return ladder_.process(x, c.ladder);
    case FilterMode::lowpass_12:
      return first_.process(x, c.first).low;
    case FilterMode::bandpass_12:
      return first_.process(x, c.first).band;
    case FilterMode::highpass_12:
      return first_.process(x, c.first).high;
    case FilterMode::notch_12: {
      const auto out = first_.process(x, c.first);
      return out.low + out.high;
    }
    case FilterMode::lowpass_24:
      return second_.process(first_.process(x, c.first).low, c.second).low;
    case FilterMode::bandpass_24:
      return second_.process(first_.process(x, c.first).band, c.second).band;
    case FilterMode::highpass_24:
      return second_.process(first_.process(x, c.first).high, c.second).high;
    case FilterMode::notch_24: {
      const auto a = first_.process(x, c.first);
      const auto b = second_.process(a.low + a.high, c.second);
      return b.low + b.high;
    }
    }
    return T{};
  }

private:
  BasicZdfSvf<T> first_;
  BasicZdfSvf<T> second_;
  BasicZdfLadder<T> ladder_;
};

using MultimodeFilter = BasicMultimodeFilter<double>;
using StereoMultimodeFilter = BasicMultimodeFilter<StereoSample>;

} // namespace oscillate::native_dsp::synth
