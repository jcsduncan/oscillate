#pragma once

#include "dsp/crossover.hpp"
#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/lock.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <numbers>
#include <span>

// Kick & Bass Lock's Nova side (ADR-0072). The output is split at the
// crossover by a fourth-order Linkwitz-Riley pair: low and high bands in
// phase, summing to an allpass, so the level is flat at every frequency and
// each band is really gone from the other (a "remainder" high band, input
// minus low, is not: the low-pass's phase shift leaves most of the low band
// in it, and a duck there boosts the lows near the crossover). The low band
// dips under the followed kicks' own amplitude (known ahead from the
// project, so it starts on the hit's frame with no detector or latency) and
// can fold to mono. With Lock off nothing runs: the input is untouched.
namespace oscillate::native_dsp::synth::nova {

class LowEndLock {
public:
  // Lock on/off fades.
  static constexpr double switch_seconds = native_dsp::dsp::switch_fade_seconds;
  static constexpr double duck_attack_seconds = 0.0005;
  static constexpr double duck_release_seconds = 0.002;

  void prepare(double sample_rate) noexcept {
    rate_ = sample_rate;
    switch_step_ = 1.0 / std::max(1.0, switch_seconds * rate_);
    attack_ = 1.0 - std::exp(-1.0 / (duck_attack_seconds * rate_));
    release_ = 1.0 - std::exp(-1.0 / (duck_release_seconds * rate_));
    reset();
  }

  void reset() noexcept {
    clear_filters();
    gain_ = 1.0;
    seek_ = true;
  }

  // Control: the switches and the crossover.
  void set(bool on, double duck, double crossover_hz, bool mono) noexcept {
    target_ = on ? 1.0 : 0.0;
    duck_ = std::clamp(duck, 0.0, 1.0);
    mono_ = mono;
    if (crossover_hz != crossover_) {
      crossover_ = crossover_hz;
      // RBJ low- and high-pass at the Butterworth Q; two of each in series
      // make the LR4 pair.
      const double hz = std::min(crossover_hz, native_dsp::dsp::nyquist_guard * rate_);
      low_coefficients_ = native_dsp::dsp::rbj_lowpass(hz, native_dsp::dsp::butterworth_q, rate_);
      high_coefficients_ = native_dsp::dsp::rbj_highpass(hz, native_dsp::dsp::butterworth_q, rate_);
    }
  }

  void adopt(const LockState& state) noexcept {
    state_ = &state;
    seek_ = true;
  }

  // In place, for the frames starting at project frame `first`.
  void process(std::span<engine::StereoFrame> audio, std::uint64_t first) noexcept {
    if (amount_ == 0.0) {
      if (target_ == 0.0)
        return;
      clear_filters(); // Switching on: no state left from an earlier pass.
    }
    for (std::size_t i = 0; i < audio.size(); ++i) {
      amount_ = amount_ < target_ ? std::min(target_, amount_ + switch_step_)
                                  : std::max(target_, amount_ - switch_step_);
      const double kick = kick_level(first + i);
      const double want = 1.0 - duck_ * kick;
      gain_ += (want < gain_ ? attack_ : release_) * (want - gain_);
      const double left = audio[i].left;
      const double right = audio[i].right;
      double low_left = run(low_[0], low_coefficients_, left);
      double low_right = run(low_[1], low_coefficients_, right);
      const double high_left = run(high_[0], high_coefficients_, left);
      const double high_right = run(high_[1], high_coefficients_, right);
      if (mono_)
        low_left = low_right = 0.5 * (low_left + low_right);
      // The locked bands, faded in from the untouched input.
      const double locked_left = gain_ * low_left + high_left;
      const double locked_right = gain_ * low_right + high_right;
      audio[i].left = static_cast<float>(left + amount_ * (locked_left - left));
      audio[i].right = static_cast<float>(right + amount_ * (locked_right - right));
    }
    next_frame_ = first + audio.size();
  }

private:
  struct Stage {
    double z1 = 0.0, z2 = 0.0;
  };
  using Coefficients = native_dsp::dsp::BiquadCoefficients;

  using Cascade = std::array<Stage, 2>; // Two biquads: one LR4 band.

  void clear_filters() noexcept {
    for (auto* bands : {&low_, &high_})
      for (auto& channel : *bands)
        for (auto& stage : channel)
          stage = {};
  }

  [[nodiscard]] static double run(Cascade& cascade, const Coefficients& c, double x) noexcept {
    for (auto& s : cascade) {
      const double y = c.b0 * x + s.z1;
      s.z1 = c.b1 * x - c.a1 * y + s.z2;
      s.z2 = c.b2 * x - c.a2 * y;
      x = y;
    }
    return x;
  }

  // The loudest followed hit sounding at `frame`, 0..1.
  [[nodiscard]] double kick_level(std::uint64_t frame) noexcept {
    if (state_ == nullptr || state_->notes.empty())
      return 0.0;
    const auto& notes = state_->notes;
    if (seek_ || frame != next_frame_) {
      next_ = static_cast<std::size_t>(
          std::ranges::upper_bound(notes, frame, {}, &LockNote::start) - notes.begin());
      seek_ = false;
    }
    next_frame_ = frame + 1;
    while (next_ < notes.size() && notes[next_].start <= frame)
      ++next_;
    double level = 0.0;
    // A new hit fades the last under it; two back covers the overlap.
    for (std::size_t back = 1; back <= 2 && back <= next_; ++back) {
      const auto& note = notes[next_ - back];
      if (note.envelope >= state_->envelopes.size())
        continue;
      const auto& envelope = state_->envelopes[note.envelope];
      const double position =
          static_cast<double>(frame - note.start) / static_cast<double>(lock_envelope_frames);
      const auto index = static_cast<std::size_t>(position);
      if (index + 1 >= envelope.size())
        continue;
      const double t = position - static_cast<double>(index);
      level = std::max(level, envelope[index] + t * (envelope[index + 1] - envelope[index]));
    }
    return level;
  }

  double rate_ = 48'000.0;
  double switch_step_ = 0.0;
  double attack_ = 1.0;
  double release_ = 1.0;
  double target_ = 0.0;
  double amount_ = 0.0;
  double duck_ = 0.0;
  bool mono_ = false;
  double crossover_ = 0.0;
  Coefficients low_coefficients_{};
  Coefficients high_coefficients_{};
  std::array<Cascade, 2> low_{};
  std::array<Cascade, 2> high_{};
  double gain_ = 1.0;
  const LockState* state_ = nullptr;
  std::size_t next_ = 0;
  std::uint64_t next_frame_ = 0;
  bool seek_ = true;
};

} // namespace oscillate::native_dsp::synth::nova
