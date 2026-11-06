#pragma once

#include "oscillate/native_dsp/compressor.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"

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

// Telemetry: input and output peak (dBFS, held 1 s) per channel, and the
// gain reduction (dB, positive, held 1 s).
namespace telemetry {
inline constexpr std::size_t input_peak = 0;  // left, right
inline constexpr std::size_t output_peak = 2; // left, right
inline constexpr std::size_t reduction = 4;
inline constexpr std::size_t size = 5;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::press
