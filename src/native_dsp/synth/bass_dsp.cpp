#include "oscillate/native_dsp/synth/bass_dsp.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>

namespace oscillate::native_dsp::synth {

DiodeLadderCoefficients DiodeLadderCoefficients::make(double cutoff_hz, double rate,
                                                      double resonance) noexcept {
  DiodeLadderCoefficients c;
  const double wc = cutoff_hz / diode_ladder_resonant_ratio;
  const double normalised = std::clamp(wc / rate, 1e-6, 0.49);
  c.g = std::tan(std::numbers::pi * normalised);
  const double r = std::clamp(resonance, 0.0, 1.0);
  c.feedback = 1.02 * diode_ladder_max_feedback * r;
  c.makeup = 1.0 + 0.3 * c.feedback;
  // T = I - g A: diagonal 1 + g; super-diagonal -g, -g/2, -g/2; sub-diagonal -g/2.
  const double g = c.g;
  const std::array<double, 3> super{-g, -0.5 * g, -0.5 * g};
  const double sub = -0.5 * g;
  double pivot = 1.0 + g;
  c.inverse[0] = 1.0 / pivot;
  for (std::size_t i = 0; i < 3; ++i) {
    c.upper[i] = super[i] * c.inverse[i];
    pivot = 1.0 + g - sub * c.upper[i];
    c.inverse[i + 1] = 1.0 / pivot;
  }
  c.q = {g, 0.0, 0.0, 0.0};
  c.solve(c.q);
  c.solve_scale = 1.0 / (1.0 + c.feedback * c.q[3]);
  return c;
}

// ---- Distortion ---------------------------------------------------------------

void BassDistortion::prepare(double rate) noexcept {
  rate_ = rate;
  dc_.prepare(rate);
}

void BassDistortion::reset() noexcept {
  held_ = 0.0;
  held_for_ = 0;
  low_ = 0.0;
  fade_ = 0.0;
  primed_ = false;
  dc_.reset();
}

void BassDistortion::set(BassDistortionMode mode, double drive, double tone, double mix) noexcept {
  if (mode != mode_) {
    // The first mode after a reset is adopted at once; later changes fade.
    fade_ = primed_ ? 1.0 : 0.0;
    faded_ = mode_;
    mode_ = mode;
  }
  primed_ = true;
  // Every mode's settings are kept current, so a crossfade can run both.
  const double d = std::clamp(drive, 0.0, 1.0);
  tube_gain_ = 1.0 + 24.0 * d * d;
  fuzz_gain_ = 1.0 + 199.0 * d * d;
  makeup_ = 1.0 / (1.0 + 0.5 * std::log10(fuzz_gain_));
  levels_ = std::exp2(15.0 - 12.0 * d); // 16 bits down to 4.
  hold_ = 1U + static_cast<std::uint32_t>(std::floor(d * 23.0));
  // Tone 0..1 sweeps the low-pass from 700 Hz up; fully open bypasses it.
  const double corner = 700.0 * std::pow(30.0, std::clamp(tone, 0.0, 1.0));
  tone_ = tone >= 1.0 ? 1.0
                      : native_dsp::dsp::one_pole_coefficient(
                            std::min(corner, native_dsp::dsp::nyquist_guard * rate_), rate_);
  mix_ = std::clamp(mix, 0.0, 1.0);
}

double BassDistortion::shape(BassDistortionMode mode, double x) noexcept {
  switch (mode) {
  case BassDistortionMode::tube:
    return dc_.process(tube(x, tube_gain_));
  case BassDistortionMode::fuzz:
    return soft_clip(fuzz_gain_ * x) * makeup_;
  case BassDistortionMode::crush:
    if (held_for_ == 0) {
      held_ = std::round(x * levels_) / levels_;
    }
    held_for_ = (held_for_ + 1) % hold_;
    return held_;
  }
  return x;
}

double BassDistortion::process(double x) noexcept {
  if (mix_ <= 0.0) {
    return x;
  }
  double y = shape(mode_, x);
  if (fade_ > 0.0) {
    y += fade_ * (shape(faded_, x) - y);
    const double step = 1.0 / (mode_fade_seconds * rate_);
    fade_ = fade_ > step ? fade_ - step : 0.0;
  }
  low_ += (y - low_) * tone_;
  return x + mix_ * (low_ - x);
}

// ---- Step sequencer ------------------------------------------------------------

void StepSequencer::start() noexcept {
  running_ = true;
  elapsed_ = 0;
  base_frame_ = 0;
  base_step_ = 0.0;
  next_step_ = 0;
  sounding_ = false;
  off_pending_ = false;
}

void StepSequencer::set_timing(double frames_per_step, double swing, double gate) noexcept {
  if (frames_per_step != frames_per_step_ && frames_per_step > 0.0) {
    // Rebase: keep the musical position at the current frame.
    base_step_ += static_cast<double>(elapsed_ - base_frame_) / frames_per_step_;
    base_frame_ = elapsed_;
    frames_per_step_ = frames_per_step;
  }
  swing_ = std::clamp(swing, 0.0, 0.75);
  gate_ = std::clamp(gate, 0.05, 1.0);
}

std::uint64_t StepSequencer::onset_frame(std::int64_t step) const noexcept {
  const double swing = (step % 2 != 0) ? 0.5 * swing_ : 0.0;
  const double steps = static_cast<double>(step) + swing - base_step_;
  const double frames = std::ceil(std::max(0.0, steps * frames_per_step_));
  return base_frame_ + static_cast<std::uint64_t>(frames);
}

bool StepSequencer::holds_into_next(std::span<const SequencerStep> pattern,
                                    std::int64_t step) const noexcept {
  const auto& next = at(pattern, step + 1);
  return next.gate == StepGate::tie || (at(pattern, step).slide && next.gate == StepGate::on);
}

std::optional<SequencerEvent>
StepSequencer::due(std::span<const SequencerStep> pattern) const noexcept {
  // The release of the previous note comes before the next onset.
  const std::uint64_t onset = onset_frame(next_step_);
  if (off_pending_ && off_frame_ <= onset) {
    SequencerEvent event;
    event.kind = SequencerEvent::Kind::note_off;
    event.offset = static_cast<std::uint32_t>(off_frame_ > elapsed_ ? off_frame_ - elapsed_ : 0);
    return event;
  }
  const auto& step = at(pattern, next_step_);
  SequencerEvent event;
  event.offset = static_cast<std::uint32_t>(onset > elapsed_ ? onset - elapsed_ : 0);
  event.semitones = step.note + 12 * step.octave;
  event.accent = step.accent;
  const bool continued = sounding_ && next_step_ > 0 && holds_into_next(pattern, next_step_ - 1);
  switch (step.gate) {
  case StepGate::off:
    // A rest: nothing sounds, so there is no event; consume() still moves on.
    event.kind = SequencerEvent::Kind::note_off;
    break;
  case StepGate::tie:
    event.kind = continued ? SequencerEvent::Kind::glide : SequencerEvent::Kind::note_off;
    if (continued) {
      // A tie keeps the previous pitch: report the sounding note again.
      const auto& previous = at(pattern, next_step_ - 1);
      event.semitones = previous.note + 12 * previous.octave;
      event.accent = previous.accent;
    }
    break;
  case StepGate::on:
    event.kind = continued ? SequencerEvent::Kind::glide : SequencerEvent::Kind::note_on;
    break;
  }
  return event;
}

std::optional<SequencerEvent> StepSequencer::peek(std::span<const SequencerStep> pattern,
                                                  std::uint32_t window) const noexcept {
  if (!running_ || pattern.empty()) {
    return std::nullopt;
  }
  auto event = due(pattern);
  if (!event || event->offset >= window) {
    return std::nullopt;
  }
  return event;
}

void StepSequencer::consume(std::span<const SequencerStep> pattern) noexcept {
  const std::uint64_t onset = onset_frame(next_step_);
  if (off_pending_ && off_frame_ <= onset) {
    off_pending_ = false;
    sounding_ = false;
    return;
  }
  const std::int64_t step = next_step_++;
  const auto& current = at(pattern, step);
  const bool continued = sounding_ && step > 0 && holds_into_next(pattern, step - 1);
  if (current.gate == StepGate::off || (current.gate == StepGate::tie && !continued)) {
    sounding_ = false;
    off_pending_ = false;
    return;
  }
  sounding_ = true;
  if (holds_into_next(pattern, step)) {
    off_pending_ = false;
    return;
  }
  const double length = std::max(1.0, std::ceil(gate_ * frames_per_step_));
  off_frame_ = std::min(onset + static_cast<std::uint64_t>(length), onset_frame(step + 1));
  off_pending_ = true;
}

} // namespace oscillate::native_dsp::synth
