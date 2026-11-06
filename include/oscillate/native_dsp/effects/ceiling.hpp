#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/limiter.hpp"

#include <cstddef>
#include <span>

namespace oscillate::native_dsp::effects::ceiling {

// Ceiling (ADR-0047): the classic native limiter as an engine. Ceiling,
// lookahead and release run through the classic LimiterRuntime, so a patch
// renders bit for bit as the classic limiter with the same settings. Its
// latency is the lookahead, fixed per instance: a lookahead edit that
// changes it takes the route's rebuild path, and is not automatable.
[[nodiscard]] const EffectEngine& engine();

// The classic settings these values run.
[[nodiscard]] LimiterSettings settings_for(std::span<const double> values) noexcept;

// Telemetry: input and output peak (dBFS, held 1 s) per channel, and the
// gain reduction (dB, positive, held 1 s).
namespace telemetry {
inline constexpr std::size_t input_peak = 0;  // left, right
inline constexpr std::size_t output_peak = 2; // left, right
inline constexpr std::size_t reduction = 4;
inline constexpr std::size_t size = 5;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::ceiling
