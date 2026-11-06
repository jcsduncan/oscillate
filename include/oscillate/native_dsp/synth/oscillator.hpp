#pragma once

#include "oscillate/native_dsp/synth/shaping.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

namespace oscillate::native_dsp::synth {

// A band-limited step (BLEP) kernel: a Kaiser-windowed sinc half_width
// frames each side of the step, with its cutoff a fraction of the sample
// rate. A corrected oscillator is linear phase with a fixed delay of
// half_width - 1 frames.
struct BlepKernel {
  std::uint32_t half_width;
  double cutoff;
  double beta;
};
// The engines' kernel: short, and its transition band runs past Nyquist, so
// the top harmonics fold back under it (about -30 dB just below Nyquist).
inline constexpr BlepKernel standard_blep{12, 0.45, 10.0};
// Nova's: 64 taps with the stopband (about -100 dB) starting below Nyquist,
// so nothing folds back, even into a non-linear filter. It passes to about
// 0.39 of the rate (18.6 kHz at 48 kHz).
inline constexpr BlepKernel clean_blep{32, 0.44, 10.5};
// The same kernel at 2x and 4x oversampling: as long in time, its cutoff
// scaled down, so an oversampled oscillator still stops at the output's
// Nyquist and nothing ultrasonic reaches a non-linear stage after it.
inline constexpr BlepKernel clean_blep_2x{2 * clean_blep.half_width, clean_blep.cutoff / 2,
                                          clean_blep.beta};
inline constexpr BlepKernel clean_blep_4x{4 * clean_blep.half_width, clean_blep.cutoff / 4,
                                          clean_blep.beta};

inline constexpr std::uint32_t blep_half_width = standard_blep.half_width;
inline constexpr std::uint32_t blep_table_resolution = 256; // Table points per frame.
inline constexpr double blep_cutoff = standard_blep.cutoff;

// Band-limited step (BLEP) and ramp (BLAMP) residual tables for a kernel.
template <BlepKernel Kernel> class BasicBlepTables {
public:
  static constexpr std::uint32_t half_width = Kernel.half_width;

  // The first call builds both tables. Call it from control, before the
  // first callback; BasicBlepOscillator's constructor does.
  [[nodiscard]] static const BasicBlepTables& instance();

  // t is the time from the discontinuity to the corrected frame, in frames.
  // Residuals are band-limited minus naive, and zero outside
  // (-blep_half_width, blep_half_width). A frame exactly at t = 0 is post-step.
  [[nodiscard]] double step(double t) const noexcept {
    if (!(t > -static_cast<double>(half_width)) || t >= half_width) {
      return 0.0;
    }
    return lookup(step_, t) - (t >= 0.0 ? 1.0 : 0.0);
  }
  [[nodiscard]] double ramp(double t) const noexcept {
    if (!(t > -static_cast<double>(half_width)) || t >= half_width) {
      return 0.0;
    }
    return lookup(ramp_, t) - (t > 0.0 ? t : 0.0);
  }

private:
  BasicBlepTables();
  // Tables hold the continuous integrals of the kernel, so interpolation never
  // straddles the naive discontinuity.
  [[nodiscard]] static double lookup(const std::vector<double>& table, double t) noexcept {
    const double position = (t + half_width) * blep_table_resolution;
    const auto index = static_cast<std::size_t>(position);
    if (index + 1 >= table.size()) {
      return table.back();
    }
    const double fraction = position - static_cast<double>(index);
    return table[index] + fraction * (table[index + 1] - table[index]);
  }

  std::vector<double> step_;
  std::vector<double> ramp_;
};

using BlepTables = BasicBlepTables<standard_blep>;

enum class Waveform : std::uint8_t { saw, pulse, triangle, sine };

// A master wrap inside the current frame, as the time from the wrap to the
// frame, in [0, 1). A slave oscillator restarts its phase at that instant.
struct SyncPoint {
  bool active = false;
  double since = 0.0;
};

// Phase-accumulating oscillator with residual correction of every value step
// and slope corner, including hard-sync resets and pulse-width sweeps. The
// output is delayed by latency_frames. Increments are cycles per frame and are
// clamped to [0, max_increment]; linear FM must not drive them negative.
template <BlepKernel Kernel> class BasicBlepOscillator {
public:
  static constexpr std::uint32_t latency_frames = Kernel.half_width - 1;
  static constexpr double max_increment = 0.49;

  BasicBlepOscillator() : tables_(&BasicBlepTables<Kernel>::instance()) {}

  void reset(double phase, Waveform waveform, double pulse_width = 0.5) noexcept {
    ring_.fill(0.0F);
    position_ = 0;
    phase_ = phase - std::floor(phase);
    waveform_ = waveform;
    pending_step_ = 0.0;
    pulse_high_ = phase_ < pulse_width;
    last_width_ = pulse_width;
  }

  // A live shape change is corrected as a step at the next frame.
  void set_waveform(Waveform waveform, double pulse_width) noexcept {
    if (waveform == waveform_) {
      return;
    }
    pending_step_ += naive(phase_, waveform, pulse_width) - naive(phase_, waveform_, pulse_width);
    waveform_ = waveform;
    pulse_high_ = phase_ < pulse_width;
    last_width_ = pulse_width;
  }

  [[nodiscard]] Waveform waveform() const noexcept { return waveform_; }
  [[nodiscard]] double phase() const noexcept { return phase_; }

  // Advances one frame and returns the delayed corrected sample. wrap reports
  // this oscillator's own wrap for a hard-sync slave.
  float next(double increment, double pulse_width, SyncPoint sync, SyncPoint* wrap) noexcept {
    increment = increment > 0.0 ? (increment < max_increment ? increment : max_increment) : 0.0;
    pulse_width = pulse_width > 0.02 ? (pulse_width < 0.98 ? pulse_width : 0.98) : 0.02;
    // Fast path: nothing to correct in this frame. It performs exactly the
    // general path's arithmetic for that case.
    if (!sync.active && pending_step_ == 0.0) {
      const double event = next_event(pulse_width);
      if (event - phase_ > increment) {
        const double phase = phase_ + increment;
        if (waveform_ != Waveform::pulse || (phase < pulse_width) == pulse_high_) {
          if (wrap != nullptr) {
            *wrap = {};
          }
          phase_ = phase;
          last_width_ = pulse_width;
          return emit(naive(phase, waveform_, pulse_width));
        }
      }
    }
    return next_general(increment, pulse_width, sync, wrap);
  }

  // Naive waveform in [-1, 1]. The pulse is DC-free at every width.
  [[nodiscard]] static double naive(double phase, Waveform waveform, double width) noexcept {
    switch (waveform) {
    case Waveform::saw:
      return 2.0 * phase - 1.0;
    case Waveform::pulse:
      return (phase < width ? 1.0 : -1.0) - (2.0 * width - 1.0);
    case Waveform::triangle:
      return phase < 0.5 ? 4.0 * phase - 1.0 : 3.0 - 4.0 * phase;
    case Waveform::sine:
      return sin_cycle(phase);
    }
    return 0.0;
  }

private:
  static constexpr std::size_t ring_size = std::bit_ceil(std::size_t{2} * Kernel.half_width);
  static constexpr std::size_t ring_mask = ring_size - 1;

  // The next phase at which the waveform has a step or corner.
  [[nodiscard]] double next_event(double width) const noexcept {
    if (waveform_ == Waveform::pulse && pulse_high_ && phase_ < width) {
      return width;
    }
    if (waveform_ == Waveform::triangle && phase_ < 0.5) {
      return 0.5;
    }
    return 1.0;
  }

  float emit(double value) noexcept {
    ring_[position_ & ring_mask] += static_cast<float>(value);
    const std::size_t out = (position_ - latency_frames) & ring_mask;
    const float result = ring_[out];
    ring_[out] = 0.0F;
    ++position_;
    return result;
  }

  // Every frame with a discontinuity, sync, pending switch or width sweep.
  float next_general(double increment, double pulse_width, SyncPoint sync,
                     SyncPoint* wrap) noexcept;
  // Slope in waveform units per cycle.
  [[nodiscard]] static double slope(double phase, Waveform waveform) noexcept;
  // Walks the phase over [start, start + length) of this frame, corrects each
  // natural discontinuity on the way and returns whether the phase wrapped.
  bool advance(double& phase, double increment, double width, double start, double length,
               SyncPoint* wrap) noexcept;
  // The discontinuity happened `since` frames before the current frame. Frames
  // from the oldest unreleased one to latency_frames + 1 ahead are corrected.
  void apply_step(double height, double since) noexcept;
  void apply_ramp(double slope_change, double since) noexcept;

  const BasicBlepTables<Kernel>* tables_;
  std::array<float, ring_size> ring_{};
  std::size_t position_ = 0;
  double phase_ = 0.0;
  Waveform waveform_ = Waveform::saw;
  double pending_step_ = 0.0;
  double last_width_ = 0.5;
  bool pulse_high_ = true;
};

// Built in oscillator.cpp for these kernels only.
extern template class BasicBlepTables<standard_blep>;
extern template class BasicBlepTables<clean_blep>;
extern template class BasicBlepTables<clean_blep_2x>;
extern template class BasicBlepTables<clean_blep_4x>;
extern template class BasicBlepOscillator<standard_blep>;
extern template class BasicBlepOscillator<clean_blep>;
extern template class BasicBlepOscillator<clean_blep_2x>;
extern template class BasicBlepOscillator<clean_blep_4x>;

using BlepOscillator = BasicBlepOscillator<standard_blep>;

} // namespace oscillate::native_dsp::synth
