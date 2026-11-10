#pragma once

#include "dsp/bs1770.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Edge's DSP (ADR-0108): the clip curve and its antiderivative, the
// linear-phase half-band oversampler, and the true-peak guard.
namespace oscillate::native_dsp::effects::edge_dsp {

// ---- The curve ---------------------------------------------------------------------------

// At shape 1 the curve leaves the line this far under the ceiling.
inline constexpr double max_knee = 0.6;

// log(cosh(u)) without overflow.
[[nodiscard]] double log_cosh(double u) noexcept;

// Linear up to the threshold (1 - knee), then a tanh knee of width `knee`
// that reaches the ceiling (1) only at infinity; a flat top at knee 0.
struct Curve {
  double knee = 0.0;
  double threshold = 1.0;

  [[nodiscard]] static Curve with_knee(double knee) noexcept { return {knee, 1.0 - knee}; }
  // For a magnitude.
  [[nodiscard]] double magnitude(double a) const noexcept {
    if (a <= threshold) {
      return a;
    }
    return knee > 0.0 ? threshold + knee * std::tanh((a - threshold) / knee) : 1.0;
  }
  [[nodiscard]] double operator()(double x) const noexcept {
    return std::copysign(magnitude(std::fabs(x)), x);
  }
  // The antiderivative, zero at 0 and even.
  [[nodiscard]] double integral(double x) const noexcept {
    const double a = std::fabs(x);
    if (a <= threshold) {
      return 0.5 * a * a;
    }
    const double over = a - threshold;
    const double rounded = knee > 0.0 ? knee * knee * log_cosh(over / knee) : 0.0;
    return 0.5 * threshold * threshold + threshold * over + rounded;
  }
};

// First-order antiderivative antialiasing of one stream: the curve's mean
// over the segment from the previous input, half a sample late. Inside the
// linear part it is exactly the mean of the two inputs.
class Antialiased {
public:
  [[nodiscard]] double process(double u, const Curve& curve) noexcept;
  // Takes an input without computing an output, so a path that fades in
  // later starts from the right history.
  void skip(double u) noexcept {
    previous_ = u;
    integral_valid_ = false;
  }
  void reset() noexcept {
    previous_ = 0.0;
    integral_valid_ = false;
  }
  [[nodiscard]] double previous() const noexcept { return previous_; }

private:
  double previous_ = 0.0;
  double previous_integral_ = 0.0;
  double integral_knee_ = 0.0;
  bool integral_valid_ = false;
};

// ---- Oversampling ------------------------------------------------------------------------

inline constexpr std::size_t max_stages = 4;
inline constexpr std::size_t max_factor = std::size_t{1} << max_stages;
// The longest filter history any design keeps.
inline constexpr std::size_t max_history = 128;
// Antiderivative antialiasing from this factor up, where its half-sample
// average rolls 20 kHz off by at most 0.12 dB; below it, it would dull the
// top octave audibly.
inline constexpr std::uint32_t antialiased_factor = 8;

// One linear-phase half-band low-pass of 4K + 3 taps, Kaiser-windowed: the
// taps at even offsets from its start (2K + 2 of them; the odd ones are zero
// but the centre, which is 1/2).
struct Halfband {
  std::vector<double> even;
  std::size_t centre = 0; // K
};

struct Design {
  std::uint32_t factor = 1;
  bool antialiased = false;
  std::size_t stages = 0;
  std::array<Halfband, max_stages> halfbands{};
  // With antialiasing, the top stage decimates through this even-length
  // low-pass instead, whose half-sample delay squares the antialiasing's.
  std::vector<double> top;
  // Top-rate samples of delay that make the round trip whole frames.
  std::uint32_t pad = 0;
  // The round trip in frames, and how long its output rings after it.
  std::uint32_t latency = 0;
  std::uint32_t tail = 0;
};

// Control only (built once, on first use): the design for a factor index
// (edge_table::Oversampling) and filter choice (edge_table::Filters).
[[nodiscard]] const Design& design(std::size_t oversampling, std::size_t filters);

// A history of the last `size()` samples, newest at 0, read contiguously.
class Ring {
public:
  void resize(std::size_t size) noexcept {
    size_ = size < max_history ? size : max_history;
    reset();
  }
  void reset() noexcept {
    data_.fill(0.0);
    position_ = 0;
  }
  void push(double x) noexcept {
    position_ = (position_ == 0 ? size_ : position_) - 1;
    data_[position_] = x;
    data_[position_ + size_] = x;
  }
  // Newest first; valid for size() samples.
  [[nodiscard]] const double* newest() const noexcept { return data_.data() + position_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
  std::array<double, 2 * max_history> data_{};
  std::size_t size_ = 0;
  std::size_t position_ = 0;
};

// Base-rate frames a resampler takes in one call.
inline constexpr std::size_t block_frames = 32;
// The most samples one stage's stream holds in a call.
inline constexpr std::size_t max_stage_samples = block_frames * max_factor / 2;

// y[k] = sum over i of taps[i] * x[k - i] for k below n, summed in tap order
// (so each output is the plain dot product's, bit for bit); x must hold
// taps.size() - 1 samples of history before x[0].
void convolve(std::span<const double> taps, const double* x, std::size_t n, double* y) noexcept;

// One stream's filter input: its history, then the samples of this call.
class Line {
public:
  void resize(std::size_t history) noexcept {
    history_ = history < max_history ? history : max_history;
    reset();
  }
  void reset() noexcept { data_.fill(0.0); }
  // Where this call's samples go; history precedes it.
  [[nodiscard]] double* fresh() noexcept { return data_.data() + history_; }
  // After a call of n samples: the newest history moves to the front.
  void advance(std::size_t n) noexcept;

private:
  std::array<double, max_history + max_stage_samples> data_{};
  std::size_t history_ = 0;
};

// One channel's resampler for a design, a block at a time: up() writes
// `factor` top-rate samples per frame, down() takes them back to frames.
// Every output is the same arithmetic in the same order whatever the
// block sizes, so any split renders alike.
class Resampler {
public:
  void prepare(const Design& design) noexcept;
  void reset() noexcept;
  // Up to block_frames frames in; frames * factor samples into `top`.
  void up(std::span<const double> frames, double* top) noexcept;
  // out.size() * factor samples of `top` (used as scratch) back to frames.
  void down(double* top, std::span<double> out) noexcept;

private:
  struct Down {
    Line firsts;
    Line seconds;
  };
  const Design* design_ = nullptr;
  std::array<Line, max_stages> ups_{};
  std::array<Down, max_stages> downs_{};
  // With antialiasing, the top decimator's stream in pairs.
  Line top_firsts_;
  Line top_seconds_;
  // The last `pad` top-rate samples, oldest first.
  std::array<double, max_factor> pad_{};
  std::array<double, max_stage_samples> sums_{};
};

// ---- The true-peak guard -----------------------------------------------------------------

// A lookahead gain that keeps every 4x-interpolated point of its output (the
// BS.1770 estimate the true-peak meter reads, bs1770::TruePeak) at or
// under `ceiling`: for each interval between two frames, the gain the
// interval needs is held over every frame its interpolation reads and then
// averaged over the attack, so the gain there is flat and under it. It
// releases exponentially. It holds the output 0.01 dB under a ceiling of 1
// (the kernel scales its signal by Edge's ceiling around it). Both channels
// share the gain.
class TruePeakGuard {
public:
  // Control only.
  void prepare(double sample_rate);
  void reset() noexcept;
  // Takes a frame; replaces it with the frame `delay()` earlier times the
  // gain, and returns the gain.
  double process(double& left, double& right) noexcept;
  [[nodiscard]] std::uint32_t delay() const noexcept { return delay_; }
  // The delay these settings give at `sample_rate`.
  [[nodiscard]] static std::uint32_t delay_at(double sample_rate) noexcept;

private:
  static constexpr std::size_t radius = native_dsp::dsp::bs1770::TruePeak::radius;
  std::array<native_dsp::dsp::bs1770::TruePeak, 2> peaks_{};
  // The louder channel's magnitude over the last radius + 1 frames.
  Ring magnitudes_;
  struct Held {
    std::uint64_t frame = 0;
    double gain = 1.0;
  };
  // Running minimum of the needed gain: a monotonic queue.
  std::vector<Held> queue_;
  std::size_t head_ = 0;
  std::size_t count_ = 0;
  std::vector<double> attack_;
  std::size_t attack_position_ = 0;
  double attack_sum_ = 0.0;
  std::vector<std::array<double, 2>> delayed_;
  std::size_t delayed_position_ = 0;
  std::uint64_t frame_ = 0;
  double envelope_ = 1.0;
  double release_ = 0.0;
  double ceiling_ = 1.0;
  std::uint32_t window_ = 0;
  std::uint32_t delay_ = 0;
};

} // namespace oscillate::native_dsp::effects::edge_dsp
