#pragma once

#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/project/model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>

// Pump's building blocks (ADR-0058): the drawn shape, the anti-click gain
// smoother, the kick detector, the low/high split and the kick capture the
// panel draws the shape against. Callback safe and allocation-free.
namespace oscillate::native_dsp::effects::pump {

// The volume over one cycle: 0 (silent) to 1 (untouched), sampled from the
// document's curve on control and read with linear interpolation.
inline constexpr std::size_t shape_resolution = 4'096;
struct Shape {
  std::array<float, shape_resolution + 1> gain{};
  [[nodiscard]] double at(double phase) const noexcept {
    phase = std::clamp(phase, 0.0, 1.0);
    const double x = phase * shape_resolution;
    const auto i = std::min(static_cast<std::size_t>(x), shape_resolution - 1);
    const double t = x - static_cast<double>(i);
    return gain[i] + t * (gain[i + 1] - gain[i]);
  }
};
// Samples a curve's points (x and y 0..1, with segment tension and steps, as
// for synth curves) into a shape.
[[nodiscard]] Shape sample_shape(std::span<const project::NativeCurvePoint> points);
// The factory shape (the classic pump) when a document has none.
[[nodiscard]] const Shape& default_shape();

// Anti-click: the gain runs through two cascaded one-pole low-passes, so a
// drawn step becomes a smooth S-curve and the gain's own spectrum, which
// the product with the audio carries up into the treble as a click, falls
// at 12 dB per octave above 1 / (2 pi tau). The cascade rises 10 to 90 % in
// 3.36 tau; `set` takes that rise time. It delays the gain by 2 tau, so
// callers that know the future read the shape that much early.
class GainSmoother {
public:
  void prepare(double sample_rate) noexcept { rate_ = sample_rate; }
  void set(double rise_seconds) noexcept {
    tau_ = std::max(rise_seconds / 3.357, 1.0 / rate_);
    coefficient_ = 1.0 - std::exp(-1.0 / (tau_ * rate_));
  }
  // The gain's delay through the smoother, in seconds.
  [[nodiscard]] double delay_seconds() const noexcept { return 2.0 * tau_; }
  [[nodiscard]] double tau_seconds() const noexcept { return tau_; }
  void reset(double gain) noexcept { a_ = b_ = gain; }
  [[nodiscard]] double next(double target) noexcept {
    a_ += coefficient_ * (target - a_);
    b_ += coefficient_ * (a_ - b_);
    return b_;
  }
  [[nodiscard]] double value() const noexcept { return b_; }

private:
  double rate_ = 48'000.0;
  double tau_ = 0.75e-3;
  double coefficient_ = 1.0;
  double a_ = 1.0, b_ = 1.0;
};

// The kick detector for the audio trigger: a fast peak follower against a
// slow one, firing when the fast one rises above the threshold and past the
// slow one by 6 dB, then holding off for 60 ms so a kick fires once.
class KickDetector {
public:
  void prepare(double sample_rate, double hold_seconds = 0.060) noexcept {
    fast_release_ = std::exp(-1.0 / (0.005 * sample_rate));
    slow_ = std::exp(-1.0 / (0.120 * sample_rate));
    hold_frames_ = static_cast<std::uint32_t>(hold_seconds * sample_rate);
  }
  void set_threshold_db(double db) noexcept {
    threshold_ = oscillate::native_dsp::dsp::db_to_gain(db);
  }
  void reset() noexcept {
    fast_ = slow_level_ = 0.0;
    hold_ = 0;
  }
  // One sidechain frame; true on the frame a kick starts.
  [[nodiscard]] bool next(const engine::StereoFrame& frame) noexcept {
    const double level = std::max(std::fabs(frame.left), std::fabs(frame.right));
    fast_ = level > fast_ ? level : fast_ * fast_release_;
    slow_level_ = slow_level_ * slow_ + (1.0 - slow_) * level;
    if (hold_ > 0) {
      --hold_;
      return false;
    }
    if (fast_ >= threshold_ && fast_ > 2.0 * slow_level_ + 1e-6) {
      hold_ = hold_frames_;
      return true;
    }
    return false;
  }

private:
  double fast_release_ = 0.999;
  double slow_ = 0.9999;
  double threshold_ = 0.1;
  double fast_ = 0.0, slow_level_ = 0.0;
  std::uint32_t hold_ = 0, hold_frames_ = 2'880;
};

// Butterworth (Q = 1/sqrt 2) sections: two in cascade make the
// Linkwitz-Riley slopes the crossover and the detector use.
struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};
struct BiquadMemory {
  double z1 = 0, z2 = 0;
};
[[nodiscard]] inline double run(const Biquad& c, BiquadMemory& m, double x) noexcept {
  const double y = c.b0 * x + m.z1;
  m.z1 = c.b1 * x - c.a1 * y + m.z2;
  m.z2 = c.b2 * x - c.a2 * y;
  return y;
}
[[nodiscard]] inline double prewarp(double hz, double sample_rate) noexcept {
  return std::tan(std::numbers::pi *
                  std::clamp(hz, 10.0, native_dsp::dsp::nyquist_guard * sample_rate) / sample_rate);
}
[[nodiscard]] inline Biquad butterworth_low(double hz, double sample_rate) noexcept {
  const double k = prewarp(hz, sample_rate);
  const double norm = 1.0 / (1.0 + std::numbers::sqrt2 * k + k * k);
  return {k * k * norm, 2.0 * k * k * norm, k * k * norm, 2.0 * (k * k - 1.0) * norm,
          (1.0 - std::numbers::sqrt2 * k + k * k) * norm};
}
[[nodiscard]] inline Biquad butterworth_high(double hz, double sample_rate) noexcept {
  const auto low = butterworth_low(hz, sample_rate);
  const double k = prewarp(hz, sample_rate);
  const double norm = 1.0 / (1.0 + std::numbers::sqrt2 * k + k * k);
  return {norm, -2.0 * norm, norm, low.a1, low.a2};
}

// A Linkwitz-Riley (fourth-order) split: low + high sums to an allpass of
// the input, so a band with no reduction changes only phase, never level.
class Crossover {
public:
  void set(double hz, double sample_rate) noexcept {
    low_ = butterworth_low(hz, sample_rate);
    high_ = butterworth_high(hz, sample_rate);
  }
  void reset() noexcept { state_ = {}; }
  // Splits one channel's sample.
  void split(std::size_t channel, double x, double& low, double& high) noexcept {
    auto& s = state_[channel];
    low = run(low_, s[1], run(low_, s[0], x));
    high = run(high_, s[3], run(high_, s[2], x));
  }

private:
  Biquad low_{}, high_{};
  std::array<std::array<BiquadMemory, 4>, 2> state_{};
};

// The detector's ears: a 24 dB/octave band-pass (Linkwitz-Riley high-pass
// at `low`, low-pass at `high`) on each channel, so only the kick's band
// can restart the cycle. Listen plays its output.
class DetectFilter {
public:
  void set(double low_hz, double high_hz, double sample_rate) noexcept {
    high_pass_ = butterworth_high(low_hz, sample_rate);
    low_pass_ = butterworth_low(std::max(high_hz, low_hz * 1.25), sample_rate);
  }
  void reset() noexcept { state_ = {}; }
  [[nodiscard]] engine::StereoFrame next(const engine::StereoFrame& frame) noexcept {
    return {static_cast<float>(channel(0, frame.left)),
            static_cast<float>(channel(1, frame.right))};
  }

private:
  double channel(std::size_t c, double x) noexcept {
    auto& s = state_[c];
    x = run(high_pass_, s[1], run(high_pass_, s[0], x));
    return run(low_pass_, s[3], run(low_pass_, s[2], x));
  }
  Biquad high_pass_{}, low_pass_{};
  std::array<std::array<BiquadMemory, 4>, 2> state_{};
};

// The MIDI trigger's place in the keying notes at a moving time: the last
// note in the filter's range at or before it, the next one after it, and
// how many came before (the pattern step). Time mostly moves forward a
// frame at a time, so each note is passed once; a seek rebuilds.
class NoteCursor {
public:
  static constexpr std::size_t none = static_cast<std::size_t>(-1);

  void set_range(int low, int high) noexcept {
    low_ = low;
    high_ = high;
  }
  // From scratch at time t (after a seek, new notes or a new range).
  void rebuild(std::span<const KeyNote> notes, double t) noexcept {
    notes_ = notes;
    last_ = next_ = none;
    count_ = 0;
    for (std::size_t i = 0; i < notes_.size(); ++i) {
      if (!matches(i))
        continue;
      if (static_cast<double>(notes_[i].frame) <= t) {
        last_ = i;
        ++count_;
      } else {
        next_ = i;
        break;
      }
    }
    t_ = t;
  }
  void advance(double t) noexcept {
    if (t < t_) {
      // A nudge or anti-click change moved time back across notes.
      while (last_ != none && static_cast<double>(notes_[last_].frame) > t) {
        next_ = last_;
        last_ = previous_match(last_);
        --count_;
      }
    }
    while (next_ != none && static_cast<double>(notes_[next_].frame) <= t) {
      last_ = next_;
      ++count_;
      next_ = next_match(next_ + 1);
    }
    t_ = t;
  }
  [[nodiscard]] const KeyNote* last() const noexcept {
    return last_ == none ? nullptr : &notes_[last_];
  }
  [[nodiscard]] const KeyNote* next() const noexcept {
    return next_ == none ? nullptr : &notes_[next_];
  }
  // Notes up to and including last().
  [[nodiscard]] std::int64_t count() const noexcept { return count_; }

private:
  [[nodiscard]] bool matches(std::size_t i) const noexcept {
    return notes_[i].key >= low_ && notes_[i].key <= high_;
  }
  [[nodiscard]] std::size_t next_match(std::size_t from) const noexcept {
    for (std::size_t i = from; i < notes_.size(); ++i)
      if (matches(i))
        return i;
    return none;
  }
  [[nodiscard]] std::size_t previous_match(std::size_t before) const noexcept {
    for (std::size_t i = before; i-- > 0;)
      if (matches(i))
        return i;
    return none;
  }
  std::span<const KeyNote> notes_;
  int low_ = 0, high_ = 127;
  std::size_t last_ = none, next_ = none;
  std::int64_t count_ = 0;
  double t_ = 0.0;
};

// The kick the panel draws the shape against: the sidechain's (or, with no
// key, the input's) peak level in each of capture_points slices of the
// cycle, the last whole cycle kept.
inline constexpr std::size_t capture_points = 256;
class KickCapture {
public:
  void add(double phase, const engine::StereoFrame& frame) noexcept {
    const auto slot =
        std::min(static_cast<std::size_t>(phase * capture_points), capture_points - 1);
    if (slot < last_slot_) {
      shown_ = building_;
      building_.fill(0.0F);
      any_ = true;
    }
    last_slot_ = slot;
    building_[slot] =
        std::max(building_[slot], std::max(std::fabs(frame.left), std::fabs(frame.right)));
  }
  void reset() noexcept {
    building_.fill(0.0F);
    shown_.fill(0.0F);
    last_slot_ = 0;
    any_ = false;
  }
  [[nodiscard]] const std::array<float, capture_points>& shown() const noexcept { return shown_; }
  [[nodiscard]] bool any() const noexcept { return any_; }

private:
  std::array<float, capture_points> building_{};
  std::array<float, capture_points> shown_{};
  std::size_t last_slot_ = 0;
  bool any_ = false;
};

} // namespace oscillate::native_dsp::effects::pump
