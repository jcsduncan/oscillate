#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"

#include <cstddef>

namespace oscillate::native_dsp::effects::trim {

// Trim (ADR-0042): gain; pan as balance or with a -3, -4.5 or -6 dB law
// (centre always unity); width; mono; swap; polarity; solo left, right,
// mid or side; a 5 Hz DC block; and a phase rotator (two second-order
// allpasses at 150 Hz) that lowers the peaks of lopsided waveforms without
// changing the magnitude. At its defaults every stage is bypassed exactly,
// so the output is the input, bit for bit; toggles glide over 20 ms.
[[nodiscard]] const EffectEngine& engine();

// Telemetry: input and output peak (dBFS, held 1 s) and RMS (dBFS, 300 ms)
// per channel.
namespace telemetry {
inline constexpr std::size_t input_peak = 0;  // left, right
inline constexpr std::size_t output_peak = 2; // left, right
inline constexpr std::size_t input_rms = 4;   // left, right
inline constexpr std::size_t output_rms = 6;  // left, right
inline constexpr std::size_t size = 8;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::trim
