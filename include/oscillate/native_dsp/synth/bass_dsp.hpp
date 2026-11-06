#pragma once

#include "oscillate/native_dsp/synth/shaping.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>

namespace oscillate::native_dsp::synth {

// ---- Diode ladder ------------------------------------------------------------

// Linear model (ADR-0032), normalised to wc = 1, with u = x - k * y4:
//   y1' = u + y2 - y1
//   y2' = (y1 + y3 - 2 y2) / 2
//   y3' = (y2 + y4 - 2 y3) / 2
//   y4' = (y3 - 2 y4) / 2
// The open loop crosses -180 degrees at 1/sqrt(2) with gain 1/17.
inline constexpr double diode_ladder_max_feedback = 17.0;
inline constexpr double diode_ladder_resonant_ratio = 0.70710678118654752; // Peak / wc.

struct DiodeLadderCoefficients {
  double g = 0.0;        // tan(pi * wc / rate), wc = cutoff / resonant ratio.
  double feedback = 0.0; // k, 0 .. ~1.02 * max.
  double makeup = 1.0;   // Partial passband compensation.
  // Thomas factors of T = I - g A and the response q = T^-1 (g e1).
  std::array<double, 4> upper{};   // Forward-eliminated super-diagonal.
  std::array<double, 4> inverse{}; // 1 / pivot.
  std::array<double, 4> q{};
  double solve_scale = 1.0; // 1 / (1 + k q4)

  // cutoff is the resonant peak in Hz; resonance 0..1.
  [[nodiscard]] static DiodeLadderCoefficients make(double cutoff_hz, double rate,
                                                    double resonance) noexcept;
  // Solves T x = r in place.
  void solve(std::array<double, 4>& r) const noexcept {
    constexpr double sub = -0.5; // Sub-diagonal over g is -1/2 in every row.
    r[0] *= inverse[0];
    for (std::size_t i = 1; i < 4; ++i) {
      r[i] = (r[i] - sub * g * r[i - 1]) * inverse[i];
    }
    for (std::size_t i = 3; i-- > 0;) {
      r[i] -= upper[i] * r[i + 1];
    }
  }
};

// Zero-delay-feedback diode ladder: trapezoidal integrators, the tridiagonal
// solve per sample and the feedback resolved exactly; the input after the
// feedback sum saturates, solved by a second pass with the saturated input.
class DiodeLadder {
public:
  void reset() noexcept { s_ = {}; }
  [[nodiscard]] double process(double x, const DiodeLadderCoefficients& c) noexcept {
    std::array<double, 4> p = s_;
    c.solve(p); // T^-1 s
    const double input = x * c.makeup;
    const double y4 = (p[3] + c.q[3] * input) * c.solve_scale;
    const double u = soft_clip(input - c.feedback * y4);
    std::array<double, 4> y{};
    for (std::size_t i = 0; i < 4; ++i) {
      y[i] = p[i] + c.q[i] * u;
      s_[i] = 2.0 * y[i] - s_[i];
    }
    return y[3];
  }

private:
  std::array<double, 4> s_{};
};

// ---- Accent sweep -------------------------------------------------------------

// The 303 accent circuit: a capacitor fed by the filter envelope while the
// note is accented. It charges faster than it discharges, so a run of
// accents climbs. Output 0..1, added to the cutoff by the voice.
class AccentSweep {
public:
  static constexpr double charge_seconds = 0.1;
  static constexpr double discharge_seconds = 0.3;
  void prepare(double rate) noexcept {
    charge_ = 1.0 - std::exp(-1.0 / (charge_seconds * rate));
    discharge_ = 1.0 - std::exp(-1.0 / (discharge_seconds * rate));
  }
  void reset() noexcept { value_ = 0.0; }
  [[nodiscard]] double next(double envelope, bool accented) noexcept {
    const double target = accented ? envelope : 0.0;
    value_ += (target - value_) * (target > value_ ? charge_ : discharge_);
    return value_;
  }
  [[nodiscard]] double value() const noexcept { return value_; }

private:
  double charge_ = 0.001;
  double discharge_ = 0.0003;
  double value_ = 0.0;
};

// ---- Acid square ----------------------------------------------------------------

// The 303 square sags: its tops tilt towards zero. A one-pole high-pass whose
// corner tracks the note (tilt x fundamental) applied to a band-limited
// square gives that shape and stays alias-free (it is linear).
class SquareTilt {
public:
  void reset() noexcept { low_ = 0.0; }
  // coefficient = 1 - exp(-2 pi corner / rate).
  [[nodiscard]] double process(double x, double coefficient) noexcept {
    low_ += (x - low_) * coefficient;
    return x - low_;
  }

private:
  double low_ = 0.0;
};

// ---- Distortion -------------------------------------------------------------------

enum class BassDistortionMode : std::uint8_t { tube, fuzz, crush };

// Tube: asymmetric soft clip (even harmonics). Fuzz: high-gain symmetric
// clip. Crush: bit depth and sample-rate reduction. Then a tone low-pass
// (tone 1 bypasses it) and a dry/wet mix. Run it at the voice's oversampled
// rate.
class BassDistortion {
public:
  void prepare(double rate) noexcept;
  void reset() noexcept;
  // drive, tone and mix 0..1.
  void set(BassDistortionMode mode, double drive, double tone, double mix) noexcept;
  [[nodiscard]] double process(double x) noexcept;

  // A mode change crossfades from the old mode to the new over this long,
  // so switching never steps the output (ADR-0034).
  static constexpr double mode_fade_seconds = 0.01;

  [[nodiscard]] static double tube(double x, double gain) noexcept {
    constexpr double bias = 0.35;
    return (soft_clip(gain * x + bias) - soft_clip(bias)) / std::sqrt(gain);
  }

private:
  [[nodiscard]] double shape(BassDistortionMode mode, double x) noexcept;

  double rate_ = 96'000.0;
  BassDistortionMode mode_ = BassDistortionMode::tube;
  BassDistortionMode faded_ = BassDistortionMode::tube;
  double fade_ = 0.0;
  bool primed_ = false;
  double tube_gain_ = 1.0;
  double fuzz_gain_ = 1.0;
  double makeup_ = 1.0;
  double levels_ = 32'768.0;
  std::uint32_t hold_ = 1;
  std::uint32_t held_for_ = 0;
  double held_ = 0.0;
  double tone_ = 1.0;
  double low_ = 0.0;
  double mix_ = 0.0;
  DcBlocker dc_{};
};

// ---- Step sequencer ------------------------------------------------------------------

enum class StepGate : std::uint8_t { off, on, tie };

struct SequencerStep {
  int note = 0;   // Semitones from the root, -12..12.
  int octave = 0; // -1, 0 or 1.
  StepGate gate = StepGate::on;
  bool accent = false;
  bool slide = false;
};

struct SequencerEvent {
  enum class Kind : std::uint8_t { note_on, glide, note_off };
  Kind kind = Kind::note_on;
  std::uint32_t offset = 0; // Frames from the current position.
  int semitones = 0;        // From the root (note + 12 * octave).
  bool accent = false;
};

// A 303-style pattern clock. Time is an integer frame count since start(),
// so any block split sees the same events (ADR-0032). Step n starts at
// ceil((n + swing_offset(n)) * frames_per_step) frames, where odd steps are
// delayed by swing / 2 of a step. A tempo change rebases the clock at the
// current frame.
class StepSequencer {
public:
  void start() noexcept;
  void stop() noexcept { running_ = false; }
  [[nodiscard]] bool running() const noexcept { return running_; }

  // Control rate. swing 0..0.75, gate 0.05..1 of a step.
  void set_timing(double frames_per_step, double swing, double gate) noexcept;

  // The next event within `window` frames, or none. pattern holds `length`
  // steps (1..16). Does not move the clock.
  [[nodiscard]] std::optional<SequencerEvent> peek(std::span<const SequencerStep> pattern,
                                                   std::uint32_t window) const noexcept;
  // Fires the event peek returned (call after advancing to its offset).
  void consume(std::span<const SequencerStep> pattern) noexcept;
  void advance(std::uint32_t frames) noexcept { elapsed_ += frames; }

  [[nodiscard]] std::int64_t current_step() const noexcept { return next_step_ - 1; }
  [[nodiscard]] std::uint64_t onset_frame(std::int64_t step) const noexcept;

private:
  [[nodiscard]] std::optional<SequencerEvent>
  due(std::span<const SequencerStep> pattern) const noexcept;
  [[nodiscard]] static const SequencerStep& at(std::span<const SequencerStep> pattern,
                                               std::int64_t step) noexcept {
    return pattern[static_cast<std::size_t>(step % static_cast<std::int64_t>(pattern.size()))];
  }
  [[nodiscard]] bool holds_into_next(std::span<const SequencerStep> pattern,
                                     std::int64_t step) const noexcept;

  bool running_ = false;
  std::uint64_t elapsed_ = 0;
  std::uint64_t base_frame_ = 0;
  double base_step_ = 0.0;
  double frames_per_step_ = 6'000.0;
  double swing_ = 0.0;
  double gate_ = 0.5;
  std::int64_t next_step_ = 0;
  bool sounding_ = false;
  bool off_pending_ = false;
  std::uint64_t off_frame_ = 0;
};

} // namespace oscillate::native_dsp::synth
