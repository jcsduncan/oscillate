#pragma once

#include "oscillate/native_dsp/synth/modulation.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace oscillate::native_dsp::synth {

struct StereoOut {
  double left;
  double right;
};

// Power-of-two delay line with cubic Hermite reads. Allocates in prepare only.
class DelayLine {
public:
  void prepare(std::size_t max_frames);
  void reset() noexcept;
  void write(double value) noexcept {
    buffer_[write_ & mask_] = static_cast<float>(value);
    ++write_;
  }
  // delay_frames >= 1; 1 returns the most recent write, so a read lags its
  // input by delay_frames - 1 frames.
  [[nodiscard]] double read(double delay_frames) const noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept { return mask_ - 3; }

private:
  std::vector<float> buffer_;
  std::size_t mask_ = 0;
  std::size_t write_ = 0;
};

// Feedback comb filter for synth voices: peaks (positive) or notches
// (negative feedback) at multiples of the "cutoff" frequency. The delay glides
// per control interval, so sweeps are smooth. Output is level-compensated.
class CombFilter {
public:
  // Control only. Longest delay is one period of min_hz.
  void prepare(double sample_rate, double min_hz = 20.0);
  void reset() noexcept;
  // feedback in (-0.98, 0.98).
  void set(double hz, double feedback, std::uint32_t ramp_frames) noexcept;
  [[nodiscard]] double process(double x) noexcept;

private:
  DelayLine line_;
  double rate_ = 48'000.0;
  LinearRamp delay_;
  LinearRamp feedback_;
  double last_ = 0.0;
};

// Three-phase string ensemble: one delay line, three taps swept by a slow and
// a fast sine, 120 degrees apart, spread across the stereo field.
class Ensemble {
public:
  static constexpr double tail_seconds = 0.02;
  void prepare(double sample_rate);
  void reset() noexcept;
  void set_amount(double amount, std::uint32_t ramp_frames) noexcept {
    mix_.set_target(amount > 0.0 ? (amount < 1.0 ? amount : 1.0) : 0.0, ramp_frames);
  }
  [[nodiscard]] StereoOut process(double left, double right) noexcept;

private:
  DelayLine line_;
  double rate_ = 48'000.0;
  double slow_phase_ = 0.0;
  double fast_phase_ = 0.0;
  LinearRamp mix_;
};

// Bucket-brigade flavoured stereo delay: a darkening, gently saturating
// feedback path, optional ping-pong and smoothed time changes that bend pitch
// like a real delay instead of clicking. Switching ping-pong crossfades how
// the lines are fed over 20 ms, so the switch never writes a step that would
// come back as a click one repeat later.
class BbdDelay {
public:
  static constexpr double max_seconds = 2.0;
  static constexpr double ping_pong_seconds = 0.02;
  void prepare(double sample_rate);
  void reset() noexcept;
  void set(double seconds, double feedback, double mix, bool ping_pong,
           std::uint32_t ramp_frames) noexcept;
  [[nodiscard]] StereoOut process(double left, double right) noexcept;
  // A conservative tail bound for these settings: the time for repeats to
  // fall 80 dB, capped at a minute.
  [[nodiscard]] static double tail_seconds(double seconds, double feedback) noexcept;

private:
  DelayLine left_;
  DelayLine right_;
  double rate_ = 48'000.0;
  LinearRamp time_;
  LinearRamp feedback_;
  LinearRamp mix_;
  LinearRamp ping_pong_; // 1 is ping-pong, 0 straight stereo.
  bool primed_ = false;  // The first setting applies at once.
  double tone_left_ = 0.0;
  double tone_right_ = 0.0;
  double tone_coefficient_ = 0.0;
  DcBlocker dc_left_;
  DcBlocker dc_right_;
};

// Gentle tape or console stage: asymmetric soft saturation plus wow and
// flutter from a short, slowly modulated delay. The stage has a constant
// latency of latency_seconds, identical in playback and export.
class TapeStage {
public:
  static constexpr double latency_seconds = 0.003;
  static constexpr double tail_seconds = 0.01;
  void prepare(double sample_rate);
  void reset() noexcept;
  void set(double drive, double wobble, std::uint32_t ramp_frames) noexcept {
    drive_.set_target(drive > 0.0 ? (drive < 1.0 ? drive : 1.0) : 0.0, ramp_frames);
    wobble_.set_target(wobble > 0.0 ? (wobble < 1.0 ? wobble : 1.0) : 0.0, ramp_frames);
  }
  [[nodiscard]] StereoOut process(double left, double right) noexcept;

private:
  [[nodiscard]] static double saturate(double x, double gain, double bias) noexcept;
  [[nodiscard]] double centre_frames() const noexcept {
    return std::round(latency_seconds * rate_) + 1.0;
  }

  DelayLine left_;
  DelayLine right_;
  double rate_ = 48'000.0;
  double wow_phase_ = 0.0;
  double flutter_phase_ = 0.0;
  LinearRamp drive_;
  LinearRamp wobble_;
};

} // namespace oscillate::native_dsp::synth
