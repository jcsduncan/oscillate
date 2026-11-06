#pragma once

#include "oscillate/native_dsp/synth/drift.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

// Ember's circuit model (ADR-0066): the ways a real vintage poly falls short
// of its schematic, as pure functions of the voice card, so they can be
// tested on their own and so export equals playback.
//
// - DCO A is counter-divided from a crystal (Juno-style): its pitch never
//   drifts, but it can only land on clock / N, so high notes are off by a
//   cent or two, and the charge DAC leaves its level rippling note to note.
// - VCO B is a saw-core VCO (Prophet-style): its exponential converter tracks
//   a little sharp or flat per octave, bulk resistance and the core's finite
//   reset time pull the top of the keyboard flat, its ramp bows as the
//   timing capacitor charges, and it wanders with temperature.
// - Every card has its own offsets: cutoff, resonance, envelope speed, pulse
//   width and VCA control offset (the thump on a fast attack).
// - Leakage: each oscillator bleeds into the mixer at its level's floor, the
//   filter lets a little input past when closed, the VCA never quite shuts.
// - The supply sags with the load of the sounding voices, flattening the
//   VCOs; the mains add hum and its rectifier buzz.
namespace oscillate::native_dsp::synth::ember_circuit {

inline constexpr std::uint32_t cards = 8;
inline constexpr std::uint32_t units = 16;

// DCO A: the counter's clock (Hz).
inline constexpr double dco_clock_hz = 8'000'000.0;
// DCO A: the level ripple at full Age (fraction).
inline constexpr double dco_ripple = 0.06;
// VCO B at full Tuning: tracking error up to this per octave from middle C,
inline constexpr double tracking_cents_per_octave = 2.5;
// a high-frequency droop of 1..2 times this per kHz,
inline constexpr double droop_cents_per_khz = 1.0;
// a core reset time of 0.8..2.2 microseconds,
inline constexpr double reset_seconds = 1.5e-6;
inline constexpr double reset_spread = 0.7e-6;
// slow drift up to this (plus age_drift_cents at full Age),
inline constexpr double tuning_drift_cents = 6.0;
inline constexpr double age_drift_cents = 3.0;
// and fast jitter up to this.
inline constexpr double jitter_cents = 0.8;
// Per-card offsets at full Age.
inline constexpr double offset_cents = 6.0;
inline constexpr double cutoff_semitones = 2.0;
inline constexpr double cutoff_drift_semitones = 1.5;
inline constexpr double resonance_spread = 0.12;
inline constexpr double envelope_spread = 0.15;
inline constexpr double pulse_spread = 0.03;
inline constexpr double max_curvature = 0.5;
// Leakage at full Bleed (linear gain): oscillators into the mixer, input
// through the closed filter, signal through the closed VCA, and the VCA's
// control offset.
inline constexpr double oscillator_leak = 0.001; // -60 dB
inline constexpr double filter_leak = 0.002;     // -54 dB
inline constexpr double vca_leak = 0.0005;       // -66 dB
inline constexpr double vca_offset = 0.02;
// The VCA leak fades out this long after a voice's envelope ends.
inline constexpr double vca_leak_seconds = 0.25;
// The supply: every card at full level flattens the VCOs this much at full Age.
inline constexpr double sag_cents = 5.0;
// Mains hum at full Hum, its harmonics (rectifier buzz) and its gate.
inline constexpr double hum_level = 0.0005; // -66 dB
inline constexpr std::array<double, 5> hum_harmonics{1.0, 0.5, 0.3, 0.0, 0.12};
inline constexpr double hum_attack_seconds = 0.05;
inline constexpr double hum_release_seconds = 0.5;

// One voice card's fixed character, scaled by the panel's controls.
struct Card {
  double tracking = 0.0;   // Cents per octave from middle C (VCO B).
  double droop = 0.0;      // Cents flat per kHz (VCO B, positive).
  double reset = 0.0;      // Core reset time, seconds (VCO B).
  double offset = 0.0;     // Cents (VCO B).
  double cutoff = 0.0;     // Semitones.
  double resonance = 1.0;  // Scale.
  double envelope = 1.0;   // Time scale.
  double pulse = 0.0;      // Width offset (both oscillators).
  double curvature = 0.0;  // VCO B ramp bow, 0 straight.
  double vca_offset = 0.0; // Linear, signed.
};

// The tolerance `index` of card `slot` in unit `unit` (1-based), in [-1, 1].
[[nodiscard]] inline double tolerance(std::uint32_t unit, std::uint32_t slot,
                                      std::uint32_t index) noexcept {
  return voice_tolerance(slot + cards * (unit > 0 ? unit - 1 : 0), index);
}

[[nodiscard]] inline Card card(std::uint32_t unit, std::uint32_t slot, double age, double tuning,
                               double bleed) noexcept {
  const auto t = [&](std::uint32_t index) { return tolerance(unit, slot, index); };
  Card c;
  c.tracking = tuning * tracking_cents_per_octave * t(3);
  c.droop = tuning * droop_cents_per_khz * (1.0 + std::fabs(t(4)));
  c.reset = tuning * (reset_seconds + reset_spread * t(5));
  c.offset = age * offset_cents * t(0);
  c.cutoff = age * cutoff_semitones * t(1);
  c.resonance = 1.0 + age * resonance_spread * t(9);
  c.envelope = 1.0 + age * envelope_spread * t(2);
  c.pulse = age * pulse_spread * t(7);
  c.curvature = age * max_curvature * (0.5 + 0.5 * t(6));
  c.vca_offset = bleed * vca_offset * t(10);
  return c;
}

// DCO A lands on the nearest clock / N.
[[nodiscard]] inline double dco_hz(double hz) noexcept {
  if (!(hz > 0.0)) {
    return hz;
  }
  const double divider = std::max(1.0, std::round(dco_clock_hz / hz));
  return dco_clock_hz / divider;
}

// DCO A's level for `key` on this unit: the charge DAC's ripple.
[[nodiscard]] inline double dco_level(std::uint32_t unit, int key, double age) noexcept {
  return 1.0 + age * dco_ripple * voice_tolerance(static_cast<std::uint32_t>(key) + 1'000U, unit);
}

// VCO B's frequency for an ideal `hz` played at `semitones` (MIDI pitch),
// before drift: tracking from middle C, droop, then the reset time, which
// adds to every period.
[[nodiscard]] inline double vco_hz(double hz, double semitones, const Card& c) noexcept {
  const double octaves = (semitones - 60.0) / 12.0;
  const double cents = c.offset + c.tracking * octaves - c.droop * hz / 1'000.0;
  const double bent = hz * std::exp2(cents / 1'200.0);
  return bent / (1.0 + bent * c.reset);
}

// The bowed ramp: a saw in [-1, 1] through the capacitor's charging curve,
// ends fixed. Curvature 0 is a straight ramp.
[[nodiscard]] inline double bow(double saw, double curvature) noexcept {
  if (curvature <= 1e-6) {
    return saw;
  }
  const double phase = 0.5 * (saw + 1.0);
  return 2.0 * (1.0 - std::exp(-curvature * phase)) / (1.0 - std::exp(-curvature)) - 1.0;
}

// Mains hum with its rectifier harmonics, peak about 1 before scaling.
class Hum {
public:
  void prepare(double rate) noexcept { rate_ = rate; }
  void reset() noexcept { phase_ = 0.0; }
  [[nodiscard]] double next(double mains_hz) noexcept {
    phase_ += mains_hz / rate_;
    phase_ -= std::floor(phase_);
    double y = 0.0;
    for (std::size_t h = 0; h < hum_harmonics.size(); ++h) {
      y +=
          hum_harmonics[h] * std::sin(2.0 * std::numbers::pi * static_cast<double>(h + 1) * phase_);
    }
    return y;
  }

private:
  double rate_ = 48'000.0;
  double phase_ = 0.0;
};

} // namespace oscillate::native_dsp::synth::ember_circuit
