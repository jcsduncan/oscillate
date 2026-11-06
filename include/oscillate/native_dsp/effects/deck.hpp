#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"

#include <cstddef>

namespace oscillate::native_dsp::effects::deck {

// Deck (ADR-0055): a DJ mixer's three-band isolator. Two fourth-order
// Linkwitz-Riley crossovers (low 60-600 Hz, high 1-8 kHz) split the signal
// into low, mid and high, phase-compensated so the bands sum back to one
// shared allpass: flat in magnitude with every band at 0 dB. Each band runs
// from a full kill (its bottom stop, -40 dB and below is silence) to +6 dB,
// with a kill switch; gains and kills glide, so automation never clicks.
[[nodiscard]] const EffectEngine& engine();

// Telemetry: each band's level after its gain (dBFS RMS, 300 ms), then the
// output peak (dBFS, held 1 s) per channel.
namespace telemetry {
inline constexpr std::size_t band_rms = 0;    // low, mid, high
inline constexpr std::size_t output_peak = 3; // left, right
inline constexpr std::size_t size = 5;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::deck
