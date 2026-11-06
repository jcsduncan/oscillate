#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"

#include <cstddef>

namespace oscillate::native_dsp::effects::contour {

// Contour (ADR-0040): a multimode filter effect from the synths' filter
// models (SVF, ladder, OTA, SEM, comb), 2x oversampled with a phase-aligned
// dry path, an LFO with tempo sync and stereo offset, and an envelope
// follower on the input or the sidechain. Defaults are "Warm Low-Pass".
[[nodiscard]] const EffectEngine& engine();

// Telemetry: the left channel's cutoff (Hz), the envelope follower's level
// (linear) and the LFO's cutoff offset (semitones).
namespace telemetry {
inline constexpr std::size_t cutoff_hz = 0;
inline constexpr std::size_t envelope = 1;
inline constexpr std::size_t lfo = 2;
inline constexpr std::size_t size = 3;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::contour
