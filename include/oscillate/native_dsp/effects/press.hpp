#pragma once

#include "oscillate/native_dsp/compressor.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/reduction_bands.hpp"

#include <cstddef>
#include <span>

namespace oscillate::native_dsp::effects::press {

// Press (ADR-0047): the classic native compressor as an engine. Threshold,
// ratio, knee, attack, release, makeup and the detector high-pass run
// through the classic CompressorRuntime, so a patch renders bit for bit as
// the classic compressor with the same settings, keyed or not. It has no
// latency and no tail; a sidechain send keys its detector.
[[nodiscard]] const EffectEngine& engine();

// The classic settings these values run.
[[nodiscard]] CompressorSettings settings_for(std::span<const double> values) noexcept;

// Telemetry: input and output peak (dBFS, held 1 s) per channel, the gain
// reduction (dB, positive, held 1 s), then where it comes from: each
// reduction band's share of the detector signal (the sidechain when keyed,
// else the input; weighted by the detector high-pass), low to high
// (reduction_bands.hpp).
namespace telemetry {
inline constexpr std::size_t input_peak = 0;  // left, right
inline constexpr std::size_t output_peak = 2; // left, right
inline constexpr std::size_t reduction = 4;
inline constexpr std::size_t reduction_bands = 5;
inline constexpr std::size_t size = reduction_bands + effects::reduction_bands::count;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::press
