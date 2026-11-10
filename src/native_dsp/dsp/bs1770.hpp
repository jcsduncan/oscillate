#pragma once

#include "dsp/biquad.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <vector>

// The one ITU-R BS.1770 implementation: K-weighting, the 100 ms step
// windows behind momentary and short-term loudness, exact two-stage gating
// and the 4x true-peak interpolator. The export analyser, the master
// true-peak meter and the Loudness effect all compose these in the same
// order, so they report identical values on the same signal.
namespace oscillate::native_dsp::dsp::bs1770 {

inline constexpr double offset_lkfs = -0.691;
inline constexpr double absolute_gate_lkfs = -70.0;
inline constexpr double relative_gate_lu = -10.0;
inline constexpr std::size_t momentary_steps = 4;   // 400 ms of 100 ms steps.
inline constexpr std::size_t short_term_steps = 30; // 3 s.

// Loudness of a mean-square power: -0.691 + 10 log10(power).
[[nodiscard]] inline double lkfs(double power) noexcept {
  return offset_lkfs + 10.0 * std::log10(power);
}

// BS.1770-5 Annex 1 publishes the 48 kHz coefficients and requires an
// equivalent response at other rates. These bilinear-transform design
// constants reproduce its Tables 1 and 2 at 48 kHz (the rate-dependent
// design of libebur128).
[[nodiscard]] inline BiquadCoefficients k_shelf(double rate) noexcept {
  constexpr double frequency = 1681.974450955533;
  constexpr double gain_db = 3.999843853973347;
  constexpr double quality = 0.7071752369554196;
  constexpr double curve = 0.4996667741545416;
  const double k = std::tan(std::numbers::pi * frequency / rate);
  const double high = db_to_gain(gain_db);
  const double bend = std::pow(high, curve);
  const double denominator = 1.0 + k / quality + k * k;
  return {(high + bend * k / quality + k * k) / denominator, 2.0 * (k * k - high) / denominator,
          (high - bend * k / quality + k * k) / denominator, 2.0 * (k * k - 1.0) / denominator,
          (1.0 - k / quality + k * k) / denominator};
}

[[nodiscard]] inline BiquadCoefficients k_high_pass(double rate) noexcept {
  constexpr double frequency = 38.13547087602444;
  constexpr double quality = 0.5003270373238773;
  const double k = std::tan(std::numbers::pi * frequency / rate);
  const double denominator = 1.0 + k / quality + k * k;
  return {1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / denominator,
          (1.0 - k / quality + k * k) / denominator};
}

// One channel's K-weighting: the head shelf, then the RLB high-pass.
class KWeighting {
public:
  void prepare(double rate) noexcept {
    shelf_.set(k_shelf(rate));
    high_pass_.set(k_high_pass(rate));
    reset();
  }
  [[nodiscard]] double process(double x) noexcept { return high_pass_.process(shelf_.process(x)); }
  void reset() noexcept {
    shelf_.reset();
    high_pass_.reset();
  }

private:
  Biquad shelf_;
  Biquad high_pass_;
};

// Sums each frame's channel-weighted K-weighted power into 100 ms steps
// and keeps the last 30, for momentary (400 ms) and short-term (3 s) means.
// No rounding drifts in: every window is a fresh sum of whole steps.
class StepWindows {
public:
  void prepare(std::size_t step_frames) noexcept {
    step_frames_ = step_frames;
    reset();
  }
  void reset() noexcept {
    steps_.fill(0.0);
    running_ = 0.0;
    filled_ = 0;
    count_ = 0;
  }
  // Adds one frame's weighted power; true when it completed a step.
  bool add(double power) noexcept {
    running_ += power;
    if (++filled_ < step_frames_) {
      return false;
    }
    steps_[count_ % short_term_steps] = running_;
    ++count_;
    running_ = 0.0;
    filled_ = 0;
    return true;
  }
  [[nodiscard]] std::uint64_t steps() const noexcept { return count_; }
  // The mean power of the last `steps` complete steps (0 until there are
  // that many).
  [[nodiscard]] double mean(std::size_t steps) const noexcept {
    if (count_ < steps) {
      return 0.0;
    }
    double sum = 0.0;
    for (std::size_t k = 0; k < steps; ++k) {
      sum += steps_[(count_ - 1 - k) % short_term_steps];
    }
    return sum / static_cast<double>(steps * step_frames_);
  }
  [[nodiscard]] double momentary() const noexcept { return mean(momentary_steps); }
  [[nodiscard]] double short_term() const noexcept { return mean(short_term_steps); }

private:
  std::array<double, short_term_steps> steps_{};
  double running_ = 0.0;
  std::size_t step_frames_ = 1;
  std::size_t filled_ = 0;
  std::uint64_t count_ = 0;
};

struct GatedLoudness {
  std::optional<double> integrated_lkfs;
  std::size_t absolute_gated_blocks = 0;
  std::size_t relative_gated_blocks = 0;
};

// Integrated loudness by exact two-stage gating over every 400 ms block:
// blocks above -70 LKFS, then those above 10 LU under their mean. Each
// block's loudness is computed once, as it arrives, so a reading is a
// linear scan of comparisons and sums. Storage is reserved on construction
// (control); add() never allocates and refuses blocks past the capacity.
class Gate {
public:
  explicit Gate(std::size_t capacity) {
    powers_.reserve(capacity);
    loudness_.reserve(capacity);
  }
  [[nodiscard]] bool add(double power) noexcept {
    if (powers_.size() == powers_.capacity()) {
      return false;
    }
    powers_.push_back(power);
    loudness_.push_back(power > 0.0 ? lkfs(power) : -HUGE_VAL);
    return true;
  }
  void clear() noexcept {
    powers_.clear();
    loudness_.clear();
  }
  [[nodiscard]] std::size_t blocks() const noexcept { return powers_.size(); }
  [[nodiscard]] bool full() const noexcept { return powers_.size() == powers_.capacity(); }

  [[nodiscard]] GatedLoudness integrated() const noexcept {
    GatedLoudness result;
    double absolute_sum = 0.0;
    for (std::size_t i = 0; i < powers_.size(); ++i) {
      if (loudness_[i] > absolute_gate_lkfs) {
        absolute_sum += powers_[i];
        ++result.absolute_gated_blocks;
      }
    }
    if (result.absolute_gated_blocks == 0) {
      return result;
    }
    const double relative_gate =
        lkfs(absolute_sum / static_cast<double>(result.absolute_gated_blocks)) + relative_gate_lu;
    double relative_sum = 0.0;
    for (std::size_t i = 0; i < powers_.size(); ++i) {
      if (loudness_[i] > absolute_gate_lkfs && loudness_[i] > relative_gate) {
        relative_sum += powers_[i];
        ++result.relative_gated_blocks;
      }
    }
    if (result.relative_gated_blocks > 0) {
      result.integrated_lkfs =
          lkfs(relative_sum / static_cast<double>(result.relative_gated_blocks));
    }
    return result;
  }

private:
  std::vector<double> powers_;
  std::vector<double> loudness_;
};

// One channel's 4x intersample peak estimate: three fractional phases of a
// 33-tap Blackman-windowed sinc, normalised to unity at DC (the integer
// phase is the sample itself). Against analytic sines it reads within
// 0.006 dB from 20 Hz to 20 kHz at 44.1 to 96 kHz, where the BS.1770
// Annex 2 example filter over-reads by up to 0.17 dB. It answers `radius`
// samples late: push() returns the interpolated peak around the sample
// pushed that long ago. Allocation-free.
class TruePeak {
public:
  static constexpr std::size_t radius = 16;
  static constexpr std::size_t taps = radius * 2 + 1;

  TruePeak() noexcept {
    for (std::size_t phase = 1; phase < 4; ++phase) {
      const double fraction = static_cast<double>(phase) / 4.0;
      double sum = 0.0;
      for (std::size_t tap = 0; tap < taps; ++tap) {
        const double distance = static_cast<double>(tap) - static_cast<double>(radius);
        const double angle =
            2.0 * std::numbers::pi * static_cast<double>(tap) / static_cast<double>(taps - 1);
        const double window = 0.42 - 0.5 * std::cos(angle) + 0.08 * std::cos(2.0 * angle);
        const double x = distance - fraction;
        const double sinc =
            std::abs(x) < 1e-12 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
        kernels_[tap][phase - 1] = sinc * window;
        sum += kernels_[tap][phase - 1];
      }
      for (std::size_t tap = 0; tap < taps; ++tap) {
        kernels_[tap][phase - 1] /= sum;
      }
    }
  }

  void reset() noexcept {
    history_.fill(0.0);
    write_ = 0;
  }

  // Pushes a sample; returns the largest magnitude of the three
  // interpolated points between the samples `radius` and `radius + 1` back.
  [[nodiscard]] double push(double x) noexcept {
    // Each sample is kept twice, `taps` apart, so the window from the
    // oldest sample to the newest is always contiguous.
    history_[write_] = x;
    history_[write_ + taps] = x;
    if (++write_ == taps) {
      write_ = 0;
    }
    // write_ is now the oldest sample; walk oldest to newest.
    const double* window = history_.data() + write_;
    std::array<double, lanes> y{};
    for (std::size_t tap = 0; tap < taps; ++tap) {
      const double x = window[tap];
      for (std::size_t phase = 0; phase < lanes; ++phase) {
        y[phase] += kernels_[tap][phase] * x;
      }
    }
    double top = 0.0;
    for (std::size_t phase = 0; phase < phases; ++phase) {
      top = std::max(top, std::abs(y[phase]));
    }
    return top;
  }

private:
  // The three fractional phases, tap-major so they sum side by side (each
  // in tap order); the fourth lane is zero.
  static constexpr std::size_t phases = 3;
  static constexpr std::size_t lanes = phases + 1;
  std::array<std::array<double, lanes>, taps> kernels_{};
  std::array<double, 2 * taps> history_{};
  std::size_t write_ = 0;
};

} // namespace oscillate::native_dsp::dsp::bs1770
