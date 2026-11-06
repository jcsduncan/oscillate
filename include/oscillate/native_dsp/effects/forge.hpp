#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"

#include <cstddef>
#include <span>

namespace oscillate::native_dsp::effects::forge {

// Forge (ADR-0040): eight saturation curves (tape, tube, transistor, diode,
// clip, triangle fold, sine fold, rectify), each with a closed-form
// antiderivative for first-order antiderivative antialiasing, run at 16x
// through elliptic half-bands. Colour is a pre-emphasis shelf undone after
// the curve; auto gain matches a -12 dBFS sine's level at any drive. The dry
// path runs through the same resampling, so partial mixes stay aligned.
[[nodiscard]] const EffectEngine& engine();

// For panels, from a value set in table order: the auto-gain makeup in dB,
// and the static curve with that makeup at out.size() inputs evenly spaced
// over -1..1. Both are what the kernel uses; wrong-sized values do nothing.
[[nodiscard]] double makeup_db(std::span<const double> values) noexcept;
void transfer_curve(std::span<const double> values, std::span<float> out) noexcept;

// The static curve, with auto gain, as the panel draws it: `curve_points`
// outputs for inputs evenly spaced over -1..1.
inline constexpr std::size_t curve_points = 65;

// Telemetry: input and output RMS (linear, both channels, smoothed over
// 300 ms), the auto-gain makeup in dB, then the curve.
namespace telemetry {
inline constexpr std::size_t input_rms = 0;
inline constexpr std::size_t output_rms = 1;
inline constexpr std::size_t makeup_db = 2;
inline constexpr std::size_t curve = 3;
inline constexpr std::size_t size = curve + curve_points;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::forge
