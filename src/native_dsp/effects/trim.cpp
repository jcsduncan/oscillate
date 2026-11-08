#include "oscillate/native_dsp/effects/trim.hpp"

#include "dsp/biquad.hpp"
#include "effect_dsp.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "trim_presets.hpp"
#include "trim_table.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>

namespace oscillate::native_dsp::effects::trim {
namespace {

using trim_table::index_of;
using trim_table::Law;
using trim_table::Solo;

namespace p {
constexpr std::size_t gain = index_of("trim.gain");
constexpr std::size_t pan = index_of("trim.pan");
constexpr std::size_t law = index_of("trim.law");
constexpr std::size_t width = index_of("trim.width");
constexpr std::size_t mono = index_of("trim.mono");
constexpr std::size_t swap = index_of("trim.swap");
constexpr std::size_t invert = index_of("trim.invert");
constexpr std::size_t solo = index_of("trim.solo");
constexpr std::size_t dc = index_of("trim.dc");
constexpr std::size_t rotate = index_of("trim.rotate");
} // namespace p

constexpr double toggle_seconds = native_dsp::dsp::switch_fade_seconds;
constexpr double dc_hz = 5.0;
constexpr double rotate_hz = 150.0;
constexpr double rotate_q = 0.7;
constexpr double peak_hold_seconds = 1.0;
constexpr double rms_seconds = 0.3;

// A weight gliding to 0 or 1 over toggle_seconds; exactly 0 or 1 at rest.
struct Toggle {
  double value = 0.0;
  double target = 0.0;
  double step = 1.0;
  void to(double goal, bool jump) noexcept {
    target = goal;
    if (jump) {
      value = goal;
    }
  }
  double next() noexcept {
    value += std::clamp(target - value, -step, step);
    return value;
  }
  [[nodiscard]] bool off() const noexcept { return value == 0.0 && target == 0.0; }
};

// The phase rotator's RBJ allpass stages.
using Allpass = native_dsp::dsp::Biquad;

using native_dsp::dsp::gain_to_db;

class TrimKernel final : public EffectKernel {
public:
  explicit TrimKernel(double sample_rate)
      : dc_pole_(std::exp(-2.0 * std::numbers::pi * dc_hz / sample_rate)),
        hold_(std::exp(std::log(1e-3) / (peak_hold_seconds * sample_rate))),
        rms_(1.0 - std::exp(-1.0 / (rms_seconds * sample_rate))) {
    const double step = 1.0 / (toggle_seconds * sample_rate);
    for (auto* t : {&mono_, &swap_, &invert_left_, &invert_right_, &dc_, &rotate_}) {
      t->step = step;
    }
    for (auto& t : solo_) {
      t.step = step;
    }
    for (auto& channel : rotators_) {
      for (auto& stage : channel) {
        stage.set(native_dsp::dsp::rbj_allpass({.hz = rotate_hz, .rate = sample_rate}, rotate_q));
      }
    }
    reset();
  }

  void control(std::span<const double> v, std::uint32_t frames,
               const SynthTransport&) noexcept override {
    const bool jump = !primed_;
    const double steps = std::max<double>(1.0, frames);
    gain_.settle(dsp::db_to_gain(v[p::gain]), steps, jump);
    pan_.settle(v[p::pan], steps, jump);
    width_.settle(v[p::width], steps, jump);
    law_ = static_cast<int>(v[p::law]);
    mono_.to(v[p::mono] != 0.0 ? 1.0 : 0.0, jump);
    swap_.to(v[p::swap] != 0.0 ? 1.0 : 0.0, jump);
    const int invert = static_cast<int>(v[p::invert]);
    invert_left_.to((invert & 1) != 0 ? 1.0 : 0.0, jump);
    invert_right_.to((invert & 2) != 0 ? 1.0 : 0.0, jump);
    const int solo = static_cast<int>(v[p::solo]);
    for (std::size_t s = 0; s < solo_.size(); ++s) {
      solo_[s].to(static_cast<int>(s) == solo ? 1.0 : 0.0, jump);
    }
    dc_.to(v[p::dc] != 0.0 ? 1.0 : 0.0, jump);
    rotate_.to(v[p::rotate] != 0.0 ? 1.0 : 0.0, jump);
    primed_ = true;
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame>) noexcept override {
    for (auto& frame : audio) {
      double l = frame.left;
      double r = frame.right;
      meter(input_, l, r);
      // Swap, polarity, mono: each exactly bypassed at rest.
      if (!swap_.off()) {
        const double w = swap_.next();
        const double sl = l + (r - l) * w;
        const double sr = r + (l - r) * w;
        l = sl;
        r = sr;
      }
      if (!invert_left_.off()) {
        l *= 1.0 - 2.0 * invert_left_.next();
      }
      if (!invert_right_.off()) {
        r *= 1.0 - 2.0 * invert_right_.next();
      }
      if (!mono_.off()) {
        const double w = mono_.next();
        const double m = 0.5 * (l + r);
        l += (m - l) * w;
        r += (m - r) * w;
      }
      // Solo: a blend of what each choice would send to both sides.
      if (!(solo_[Solo::none].value == 1.0 && solo_[Solo::none].target == 1.0)) {
        const std::array<std::array<double, 2>, 5> choices{{{l, r},
                                                            {l, l},
                                                            {r, r},
                                                            {0.5 * (l + r), 0.5 * (l + r)},
                                                            {0.5 * (l - r), 0.5 * (l - r)}}};
        double out_l = 0.0;
        double out_r = 0.0;
        for (std::size_t s = 0; s < solo_.size(); ++s) {
          const double w = solo_[s].next();
          out_l += w * choices[s][0];
          out_r += w * choices[s][1];
        }
        l = out_l;
        r = out_r;
      }
      // Width, exactly bypassed at 100%.
      const double width = width_.next();
      if (width != 1.0 || width_.step != 0.0) {
        const double m = 0.5 * (l + r);
        const double s = 0.5 * (l - r) * width;
        l = m + s;
        r = m - s;
      }
      // Phase rotation and the DC block run always (their state stays warm)
      // and are blended in.
      std::array<double, 2> x{l, r};
      for (std::size_t c = 0; c < 2; ++c) {
        double rotated = x[c];
        for (auto& stage : rotators_[c]) {
          rotated = stage.process(rotated);
        }
        rotations_[c] = rotated;
      }
      if (!rotate_.off()) {
        const double w = rotate_.next();
        l += (rotations_[0] - l) * w;
        r += (rotations_[1] - r) * w;
      }
      // The block applies to the signal as it now stands.
      const double bl = dc_stage(0, l);
      const double br = dc_stage(1, r);
      if (!dc_.off()) {
        const double w = dc_.next();
        l += (bl - l) * w;
        r += (br - r) * w;
      }
      // Pan, exactly unity at the centre.
      const double pan = pan_.next();
      if (pan != 0.0 || pan_.step != 0.0) {
        const auto [gl, gr] = pan_gains(pan);
        l *= gl;
        r *= gr;
      }
      const double gain = gain_.next();
      if (gain != 1.0 || gain_.step != 0.0) {
        l *= gain;
        r *= gain;
      }
      meter(output_, l, r);
      frame.left = static_cast<float>(l);
      frame.right = static_cast<float>(r);
    }
  }

  void reset() noexcept override {
    for (auto& channel : rotators_) {
      for (auto& stage : channel) {
        stage.reset();
      }
    }
    post_x_ = {};
    post_y_ = {};
    input_ = {};
    output_ = {};
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    for (std::size_t c = 0; c < 2; ++c) {
      out[telemetry::input_peak + c] = static_cast<float>(gain_to_db(input_.peak[c]));
      out[telemetry::output_peak + c] = static_cast<float>(gain_to_db(output_.peak[c]));
      out[telemetry::input_rms + c] = static_cast<float>(gain_to_db(std::sqrt(input_.power[c])));
      out[telemetry::output_rms + c] = static_cast<float>(gain_to_db(std::sqrt(output_.power[c])));
    }
    return telemetry::size;
  }

private:
  struct Meter {
    std::array<double, 2> peak{};
    std::array<double, 2> power{};
  };

  void meter(Meter& m, double l, double r) const noexcept {
    const std::array<double, 2> x{l, r};
    for (std::size_t c = 0; c < 2; ++c) {
      m.peak[c] = std::max(std::fabs(x[c]), m.peak[c] * hold_);
      m.power[c] += rms_ * (x[c] * x[c] - m.power[c]);
    }
  }

  // The DC block on the signal as it reaches it (after rotation).
  [[nodiscard]] double dc_stage(std::size_t c, double x) noexcept {
    const double y = x - post_x_[c] + dc_pole_ * post_y_[c];
    post_x_[c] = x;
    post_y_[c] = y;
    return y;
  }

  // Centre is unity for every law; the far side falls to silence.
  [[nodiscard]] std::array<double, 2> pan_gains(double pan) const noexcept {
    const double x = 0.5 * (pan + 1.0); // 0 left .. 1 right
    const double theta = 0.5 * std::numbers::pi * x;
    switch (law_) {
    case Law::constant_power:
      return {std::numbers::sqrt2 * std::cos(theta), std::numbers::sqrt2 * std::sin(theta)};
    case Law::compromise:
      return {std::sqrt(std::numbers::sqrt2 * std::cos(theta) * 2.0 * (1.0 - x)),
              std::sqrt(std::numbers::sqrt2 * std::sin(theta) * 2.0 * x)};
    case Law::linear:
      return {2.0 * (1.0 - x), 2.0 * x};
    default:
      return {pan > 0.0 ? 1.0 - pan : 1.0, pan < 0.0 ? 1.0 + pan : 1.0};
    }
  }

  double dc_pole_;
  double hold_;
  double rms_;
  dsp::Ramp gain_;
  dsp::Ramp pan_;
  dsp::Ramp width_;
  int law_ = Law::balance;
  Toggle mono_;
  Toggle swap_;
  Toggle invert_left_;
  Toggle invert_right_;
  std::array<Toggle, 5> solo_{};
  Toggle dc_;
  Toggle rotate_;
  std::array<std::array<Allpass, 2>, 2> rotators_{};
  std::array<double, 2> rotations_{};
  std::array<double, 2> post_x_{};
  std::array<double, 2> post_y_{};
  Meter input_{};
  Meter output_{};
  bool primed_ = false;
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double>, double sample_rate) {
  return std::make_unique<TrimKernel>(sample_rate);
}

// The DC block's settling dominates: 60 dB in 6.9 time constants.
std::uint64_t tail_frames(std::span<const double>, double sample_rate) {
  return static_cast<std::uint64_t>(
      std::ceil((6.9 / (2.0 * std::numbers::pi * dc_hz) + 0.05) * sample_rate));
}
std::uint32_t latency_frames(std::span<const double>, double) { return 0; }

const synth::EngineDescriptor descriptor{"trim", "Trim", 1, trim_table::parameters,
                                         trim_presets::presets};
const EffectEngine trim_engine{&descriptor, &create_kernel, &tail_frames, &latency_frames, false};

} // namespace

const EffectEngine& engine() { return trim_engine; }

} // namespace oscillate::native_dsp::effects::trim
