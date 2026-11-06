#include "fx_modules.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>

// Nova's Shifter module (08a §5): a frequency shifter. A Hilbert pair (two
// chains of second-order allpasses, Niemitalo's design: 90 degrees apart
// within 0.7 degrees from about 30 Hz to 20 kHz at 48 kHz) gives the
// analytic signal; rotating it by a quadrature oscillator moves every
// partial by the same number of hertz, so harmonics turn inharmonic. Ring
// keeps both sidebands. Feedback sends the output back into the shifter,
// which climbs or falls endlessly (the barber-pole); a soft clip keeps it
// bounded. Spread turns the right channel's shift the other way.
namespace oscillate::native_dsp::synth::nova::fx_detail {
namespace {

constexpr std::size_t allpass_stages = 4;
constexpr std::array<double, allpass_stages> path_a{0.6923878, 0.9360654322959, 0.9882295226860,
                                                    0.9987488452737};
constexpr std::array<double, allpass_stages> path_b{0.4021921162426, 0.8561710882420,
                                                    0.9722909545651, 0.9952884791278};
constexpr double mode_fade_seconds = 0.01;
// Both sidebands carry the input's power between them.
constexpr double ring_gain = std::numbers::sqrt2;
constexpr double tail_seconds = 0.1;
constexpr std::uint32_t renormalise_interval = 64;

// y[n] = a^2 (x[n] + y[n - 2]) - x[n - 2], in series: the design lists a.
class AllpassChain {
public:
  explicit AllpassChain(const std::array<double, allpass_stages>& a) {
    for (std::size_t s = 0; s < allpass_stages; ++s) {
      a_[s] = a[s] * a[s];
    }
  }
  void reset() noexcept {
    x1_ = {};
    x2_ = {};
    y1_ = {};
    y2_ = {};
  }
  [[nodiscard]] double process(double x) noexcept {
    for (std::size_t s = 0; s < allpass_stages; ++s) {
      const double y = a_[s] * (x + y2_[s]) - x2_[s];
      x2_[s] = x1_[s];
      x1_[s] = x;
      y2_[s] = y1_[s];
      y1_[s] = y;
      x = y;
    }
    return x;
  }

private:
  std::array<double, allpass_stages> a_{};
  std::array<double, allpass_stages> x1_{};
  std::array<double, allpass_stages> x2_{};
  std::array<double, allpass_stages> y1_{};
  std::array<double, allpass_stages> y2_{};
};

// One channel: the Hilbert pair, its oscillator and its feedback.
struct Channel {
  AllpassChain real{path_a};
  AllpassChain imaginary{path_b};
  double real_delay = 0.0; // Path A's extra sample.
  double cos = 1.0;
  double sin = 0.0;
  double feedback = 0.0;
  // The rotation per sample, recomputed only when the shift moves.
  double step = 0.0;
  double step_cos = 1.0;
  double step_sin = 0.0;

  void reset() noexcept {
    real.reset();
    imaginary.reset();
    real_delay = 0.0;
    cos = 1.0;
    sin = 0.0;
    feedback = 0.0;
  }
  // The shifted and the ring outputs for one input sample; the oscillator
  // then turns by `radians`.
  void process(double x, double radians, double gain, double ring_weight, double& out) noexcept {
    const double in = x + gain * std::tanh(feedback);
    const double re = real_delay;
    real_delay = real.process(in);
    const double im = imaginary.process(in);
    const double shifted = re * cos + im * sin;
    const double ring = ring_gain * re * cos;
    out = shifted + ring_weight * (ring - shifted);
    feedback = out;
    if (radians != step) {
      step = radians;
      step_cos = std::cos(step);
      step_sin = std::sin(step);
    }
    const double next_cos = cos * step_cos - sin * step_sin;
    sin = sin * step_cos + cos * step_sin;
    cos = next_cos;
  }
  void renormalise() noexcept {
    const double length = std::hypot(cos, sin);
    if (length > 0.0 && std::isfinite(length)) {
      cos /= length;
      sin /= length;
    } else {
      cos = 1.0;
      sin = 0.0;
    }
  }
};

class ShifterModule final : public FxModule {
public:
  ShifterModule(double base_rate, std::size_t) : base_(base_rate) {
    rate_ = base_rate;
    set(fx_defaults(FxKind::shifter).p, {2.0, base_rate}, 0);
  }
  [[nodiscard]] FxKind kind() const noexcept override { return FxKind::shifter; }
  void reset() noexcept override {
    for (auto& channel : channels_) {
      channel.reset();
    }
    tick_ = 0;
  }
  void set(const FxParams& p, const FxContext& context, std::uint32_t frames) noexcept override {
    rate_ = base_ * factor_for(context.sample_rate, base_);
    shift_.to(shifter_hz(p[0]), frames);
    feedback_.to(shifter_max_feedback * clamp01(p[1]), frames);
    spread_.to(clamp01(p[2]), frames);
    const bool ring = choice(p[7], 2) == 1;
    if (!primed_) {
      ring_weight_.jump(ring ? 1.0 : 0.0);
    } else if (ring != ring_) {
      ring_weight_.to(ring ? 1.0 : 0.0,
                      static_cast<std::uint32_t>(std::lround(mode_fade_seconds * rate_)));
    }
    ring_ = ring;
    feedback_on_ = p[1] > 0.0;
    primed_ = true;
  }
  void process(Frames io) noexcept override {
    const double radians_per_hz = two_pi / rate_;
    for (auto& frame : io) {
      const double shift = shift_.next();
      const double gain = feedback_.next();
      const double spread = spread_.next();
      const double ring = ring_weight_.next();
      double left = 0.0;
      double right = 0.0;
      channels_[0].process(frame.left, shift * radians_per_hz, gain, ring, left);
      channels_[1].process(frame.right, shift * (1.0 - 2.0 * spread) * radians_per_hz, gain, ring,
                           right);
      if ((++tick_ % renormalise_interval) == 0) {
        channels_[0].renormalise();
        channels_[1].renormalise();
      }
      frame.left = std::isfinite(left) ? static_cast<float>(left) : 0.0F;
      frame.right = std::isfinite(right) ? static_cast<float>(right) : 0.0F;
    }
    live_hz_ = shift_.value;
  }
  [[nodiscard]] float telemetry() const noexcept override { return static_cast<float>(live_hz_); }
  [[nodiscard]] std::uint64_t tail_frames() const noexcept override {
    return feedback_on_ ? static_cast<std::uint64_t>(std::ceil(tail_seconds * rate_)) : 0U;
  }

private:
  double base_;
  double rate_ = 0.0;
  std::uint32_t tick_ = 0;
  bool primed_ = false;
  bool ring_ = false;
  bool feedback_on_ = false;
  double live_hz_ = 0.0;
  Glide shift_;
  Glide feedback_;
  Glide spread_;
  Glide ring_weight_;
  std::array<Channel, 2> channels_{};
};

} // namespace

std::unique_ptr<FxModule> make_shifter(double base_rate, std::size_t max_frames) {
  return std::make_unique<ShifterModule>(base_rate, max_frames);
}

} // namespace oscillate::native_dsp::synth::nova::fx_detail
