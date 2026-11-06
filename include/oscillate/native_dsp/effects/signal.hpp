#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"

#include <cstddef>

namespace oscillate::native_dsp::effects::signal {

// Signal (ADR-0042): a test generator. Sine (phase-continuous), a
// logarithmic sweep 20 Hz to 20 kHz (Farina's exponential law), white
// noise, pink noise (first-order corners every half octave from 2.5 Hz,
// the top six refitted per rate so it follows 1/f to Nyquist within
// 0.1 dB; its RMS set exactly), and impulse trains. It replaces or adds to the
// input on both channels, one, alternating sides each second, or with the
// right inverted for polarity checks. Off passes the input bit-exact;
// switching fades over 10 ms. Noise is seeded, so renders repeat.
[[nodiscard]] const EffectEngine& engine();

inline constexpr double sweep_low_hz = 20.0;
inline constexpr double sweep_high_hz = 20'000.0;

// Telemetry: the frequency now (Hz; the sweep's instantaneous one, 0 for
// noise) and the sweep's progress (0..1).
namespace telemetry {
inline constexpr std::size_t frequency = 0;
inline constexpr std::size_t progress = 1;
inline constexpr std::size_t size = 2;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::signal
