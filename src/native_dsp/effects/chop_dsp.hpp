#pragma once

#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/chop.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

// Chop's building blocks (ADR-0075): the gate's edge and release curves,
// the gain transition that plays them, the slice buffer a repeat replays
// and the pattern capture the panel draws. Callback safe and
// allocation-free once prepared.
namespace oscillate::native_dsp::effects::chop {

// A gate edge: a raised cosine from 0 to 1. Its slope is zero at both
// ends, so the gain has no corner for the product with the audio to turn
// into a click.
using native_dsp::dsp::raised_cosine;

// The release after a gate's length: the edge's S-curve when Hard (0),
// bending towards a fast drop with a long tail as Shape goes Soft (1).
inline constexpr double soft_release_power = 3.0;
[[nodiscard]] inline double release_curve(double x, double shape) noexcept {
  const double soft = 1.0 - std::pow(1.0 - x, soft_release_power);
  return raised_cosine(x) + shape * (soft - raised_cosine(x));
}

// One move of the gate's gain, from where it was to a new level, along the
// edge or the release curve. A new move starts from the gain now, so a move
// cut short by the next edge never jumps.
class Transition {
public:
  void start(double to, double frames, double elapsed, bool release, double shape) noexcept {
    from_ = value();
    to_ = to;
    frames_ = frames;
    elapsed_ = elapsed;
    release_ = release;
    shape_ = shape;
  }
  // Settled at `level`, as after a reset.
  void hold(double level) noexcept {
    from_ = to_ = level;
    frames_ = 0.0;
    elapsed_ = 0.0;
  }
  [[nodiscard]] double value() const noexcept {
    if (from_ == to_ || elapsed_ >= frames_)
      return to_;
    const double x = std::max(elapsed_, 0.0) / frames_;
    return from_ + (to_ - from_) * (release_ ? release_curve(x, shape_) : raised_cosine(x));
  }
  void advance() noexcept { elapsed_ += 1.0; }
  [[nodiscard]] double target() const noexcept { return to_; }

private:
  double from_ = 1.0;
  double to_ = 1.0;
  double frames_ = 0.0;
  double elapsed_ = 0.0;
  bool release_ = false;
  double shape_ = 0.0;
};

// An equal-power crossfade's two gains at x (0..1) through it: the
// incoming sin, the outgoing cos. Repeats and the live input are different
// audio, so their powers, not their gains, sum to one.
struct CrossGains {
  double in = 1.0;
  double out = 0.0;
};
[[nodiscard]] inline CrossGains cross_gains(double x) noexcept {
  const double angle = 0.5 * std::numbers::pi * std::clamp(x, 0.0, 1.0);
  return {std::sin(angle), std::cos(angle)};
}

// A step's input, captured from its first frame as it plays, for its
// repeats to replay. Sized on control; written and read on the callback.
class SliceBuffer {
public:
  // Control only.
  void prepare(std::size_t frames) { frames_.assign(frames, {}); }
  [[nodiscard]] std::size_t capacity() const noexcept { return frames_.size(); }
  void restart() noexcept { written_ = 0; }
  void write(const engine::StereoFrame& frame) noexcept {
    if (written_ < frames_.size())
      frames_[written_++] = frame;
  }
  // The capture `position` frames after its first (fractional), through a
  // four-point Lagrange interpolator; clamped to what has been written.
  [[nodiscard]] engine::StereoFrame read(double position) const noexcept {
    if (written_ == 0)
      return {};
    const double last = static_cast<double>(written_ - 1);
    position = std::clamp(position, 0.0, last);
    const auto whole = static_cast<std::size_t>(position);
    const double t = position - static_cast<double>(whole);
    const auto at = [&](std::size_t i, std::ptrdiff_t offset) {
      const auto index = std::clamp<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(i) + offset, 0,
                                                    static_cast<std::ptrdiff_t>(written_ - 1));
      return frames_[static_cast<std::size_t>(index)];
    };
    const auto a = at(whole, -1), b = at(whole, 0), c = at(whole, 1), d = at(whole, 2);
    const double ca = -t * (t - 1.0) * (t - 2.0) / 6.0;
    const double cb = (t + 1.0) * (t - 1.0) * (t - 2.0) / 2.0;
    const double cc = -(t + 1.0) * t * (t - 2.0) / 2.0;
    const double cd = (t + 1.0) * t * (t - 1.0) / 6.0;
    return {static_cast<float>(ca * a.left + cb * b.left + cc * c.left + cd * d.left),
            static_cast<float>(ca * a.right + cb * b.right + cc * c.right + cd * d.right)};
  }

private:
  std::vector<engine::StereoFrame> frames_;
  std::size_t written_ = 0;
};

// The last whole pattern of the input and output (peak per point) and the
// gate's gain (mean per point), for the panel. A pattern is shown once the
// next one starts.
class PatternCapture {
public:
  void add(std::size_t point, double input, double output, double gain) noexcept {
    point = std::min(point, view_points - 1);
    if (point < last_)
      publish();
    last_ = point;
    input_[point] = std::max(input_[point], static_cast<float>(input));
    output_[point] = std::max(output_[point], static_cast<float>(output));
    gain_sum_[point] += gain;
    ++gain_count_[point];
  }
  void reset() noexcept {
    input_.fill(0.0F);
    output_.fill(0.0F);
    gain_sum_.fill(0.0);
    gain_count_.fill(0);
    shown_input_.fill(0.0F);
    shown_output_.fill(0.0F);
    shown_gain_.fill(1.0F);
    last_ = 0;
  }
  [[nodiscard]] const std::array<float, view_points>& input() const noexcept {
    return shown_input_;
  }
  [[nodiscard]] const std::array<float, view_points>& output() const noexcept {
    return shown_output_;
  }
  [[nodiscard]] const std::array<float, view_points>& gain() const noexcept { return shown_gain_; }

private:
  void publish() noexcept {
    shown_input_ = input_;
    shown_output_ = output_;
    for (std::size_t i = 0; i < view_points; ++i)
      shown_gain_[i] = gain_count_[i] > 0
                           ? static_cast<float>(gain_sum_[i] / static_cast<double>(gain_count_[i]))
                           : 1.0F;
    input_.fill(0.0F);
    output_.fill(0.0F);
    gain_sum_.fill(0.0);
    gain_count_.fill(0);
  }

  std::array<float, view_points> input_{};
  std::array<float, view_points> output_{};
  std::array<double, view_points> gain_sum_{};
  std::array<std::uint32_t, view_points> gain_count_{};
  std::array<float, view_points> shown_input_{};
  std::array<float, view_points> shown_output_{};
  std::array<float, view_points> shown_gain_{};
  std::size_t last_ = 0;
};

} // namespace oscillate::native_dsp::effects::chop
