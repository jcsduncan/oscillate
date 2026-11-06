#pragma once

#include "oscillate/native_dsp/synth/oscillator.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"
#include "oscillate/native_dsp/synth/vintage_effects.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

namespace oscillate::native_dsp::synth {

// Exponential decay reaching -60 dB after exactly `seconds` (to within one
// frame's rounding), as analog drum envelopes do.
[[nodiscard]] inline double decay_coefficient(double seconds, double sample_rate) noexcept {
  return std::exp(std::log(1e-3) / std::max(1.0, seconds * sample_rate));
}

// A sine body whose frequency falls exponentially from `start_hz` to
// `end_hz` with time constant `sweep_seconds`: the heart of kicks and toms.
// Phase is integrated per frame, so pitch is exact at every instant.
class ResonantBody {
public:
  void prepare(double sample_rate) noexcept { rate_ = sample_rate; }
  void trigger(double start_hz, double end_hz, double sweep_seconds) noexcept {
    phase_ = 0.0;
    end_ = end_hz;
    excess_ = std::max(0.0, start_hz - end_hz);
    sweep_ = std::exp(-1.0 / std::max(1.0, sweep_seconds * rate_));
  }
  [[nodiscard]] double frequency() const noexcept { return end_ + excess_; }
  [[nodiscard]] double next() noexcept {
    const double value = sin_cycle(phase_);
    phase_ += (end_ + excess_) / rate_;
    phase_ -= fast_floor(phase_);
    excess_ *= sweep_;
    return value;
  }

private:
  double rate_ = 48'000.0;
  double phase_ = 0.0;
  double end_ = 50.0;
  double excess_ = 0.0;
  double sweep_ = 0.999;
};

// The six detuned square oscillators of the classic analog hi-hat and cymbal
// circuit, band-limited, summed equally. Frequencies in Hz before tuning.
inline constexpr std::array<double, 6> metal_frequencies{205.3, 304.4, 369.6, 522.7, 540.0, 800.0};

class MetalSource {
public:
  void reset() noexcept {
    for (std::size_t i = 0; i < oscillators_.size(); ++i) {
      // Free-running in hardware: fixed, distinct start phases.
      oscillators_[i].reset(0.13 * static_cast<double>(i), Waveform::pulse, 0.5);
    }
  }
  // scale multiplies every frequency (tuning).
  [[nodiscard]] double next(double scale, double sample_rate) noexcept {
    double sum = 0.0;
    for (std::size_t i = 0; i < oscillators_.size(); ++i) {
      sum += oscillators_[i].next(metal_frequencies[i] * scale / sample_rate, 0.5, {}, nullptr);
    }
    return sum / 6.0;
  }

private:
  std::array<BlepOscillator, 6> oscillators_{};
};

// A train of short noise bursts followed by a tail: the hand clap envelope.
// Burst times are relative to the trigger, in frames.
class BurstEnvelope {
public:
  static constexpr std::size_t max_bursts = 4;
  // Each burst rises over this long, so its start never clicks.
  static constexpr double attack_seconds = 0.0002;
  // The tail sits about 8 dB under the first burst, so the bursts flam.
  static constexpr double tail_peak = 0.4;
  void prepare(double sample_rate) noexcept {
    rate_ = sample_rate;
    attack_ = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(attack_seconds * rate_));
  }
  void trigger(double spacing_seconds, double burst_decay_seconds, double tail_seconds) noexcept {
    age_ = 0;
    spacing_ = static_cast<std::uint64_t>(std::max(1.0, spacing_seconds * rate_));
    burst_ = decay_coefficient(burst_decay_seconds, rate_);
    tail_ = decay_coefficient(tail_seconds, rate_);
    burst_level_ = 0.0;
    tail_level_ = 0.0;
    rising_ = 0;
    tail_rises_ = false;
    active_ = true;
  }
  [[nodiscard]] double next() noexcept {
    if (!active_) {
      return 0.0;
    }
    if (age_ % spacing_ == 0 && age_ / spacing_ < max_bursts) {
      rising_ = attack_;
      // The first burst is the strongest, so the clap lands on its beat.
      peak_ = 1.0 - 0.12 * static_cast<double>(age_ / spacing_);
      // The tail starts with the first burst, as the hardware's does, and
      // the bursts ride on it: the clap's weight is on its beat.
      tail_rises_ = age_ == 0;
    }
    ++age_;
    // A burst rises to its level over the attack, then decays; the tail
    // rises with the first burst.
    if (rising_ > 0) {
      burst_level_ += (peak_ - burst_level_) / rising_;
      if (tail_rises_)
        tail_level_ += (tail_peak - tail_level_) / rising_;
      --rising_;
    }
    const double level = std::max(burst_level_, tail_level_);
    if (rising_ == 0) {
      burst_level_ *= burst_;
      tail_level_ *= tail_;
    }
    if (age_ > spacing_ * max_bursts && level < 1e-5) {
      active_ = false;
    }
    return level;
  }
  [[nodiscard]] bool active() const noexcept { return active_; }

private:
  double rate_ = 48'000.0;
  std::uint64_t age_ = 0;
  std::uint64_t spacing_ = 480;
  std::uint32_t attack_ = 10;
  std::uint32_t rising_ = 0;
  double peak_ = 1.0;
  bool tail_rises_ = false;
  double burst_ = 0.99;
  double tail_ = 0.999;
  double burst_level_ = 0.0;
  double tail_level_ = 0.0;
  bool active_ = false;
};

// Stereo-linked feed-forward "glue" compressor with a soft knee.
// amount 0..1 sets threshold (0 to -24 dB), ratio (1 to 4) and makeup.
class GlueCompressor {
public:
  void prepare(double sample_rate) noexcept;
  void reset() noexcept { envelope_ = 0.0; }
  void set(double amount, double release_seconds) noexcept;
  [[nodiscard]] StereoOut process(double left, double right) noexcept;
  [[nodiscard]] double gain_reduction_db() const noexcept { return reduction_db_; }

private:
  double rate_ = 48'000.0;
  double attack_ = 0.0;
  double release_ = 0.0;
  double envelope_ = 0.0;
  double threshold_db_ = 0.0;
  double ratio_ = 1.0;
  double makeup_ = 1.0;
  double amount_ = 0.0;
  double reduction_db_ = 0.0;
};
} // namespace oscillate::native_dsp::synth
