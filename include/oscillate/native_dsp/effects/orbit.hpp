#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"

#include <cstddef>

namespace oscillate::native_dsp::effects::orbit {

// Orbit (ADR-0040): chorus (1-4 voices), string ensemble, dimension,
// flanger, phaser (2-12 stages), vibrato and tremolo/auto-pan from one
// per-sample LFO with tempo sync and a stereo offset. Delays read through
// third-order Lagrange interpolation; every delay mode keeps its lines fed,
// so a live mode switch crossfades without a gap.
[[nodiscard]] const EffectEngine& engine();

// Telemetry: each channel's bipolar LFO value and its sweep (delay in ms,
// the phaser's centre in Hz, or the tremolo's gain).
namespace telemetry {
inline constexpr std::size_t lfo_left = 0;
inline constexpr std::size_t lfo_right = 1;
inline constexpr std::size_t sweep_left = 2;
inline constexpr std::size_t sweep_right = 3;
inline constexpr std::size_t size = 4;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::orbit
