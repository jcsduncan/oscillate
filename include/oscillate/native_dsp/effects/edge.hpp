#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace oscillate::native_dsp::effects::edge {

// Edge (ADR-0108): a clipper under a fixed 0 dBFS ceiling. Input gain
// drives the signal into one curve that runs from a hard flat top (shape 0)
// to a soft knee (shape 1), oversampled 1x to 16x through linear-phase
// half-band filters (or shorter, lighter ones), with antiderivative
// antialiasing from 8x. A true-peak guard after the filters keeps the
// output at or under 0 dBTP as the BS.1770 meter reads it; output gain
// only attenuates. Delta plays the input (gain- and latency-aligned) less
// the output; Compare plays the aligned input, level-matched to the output
// when Level Match is on. Oversampling and the filters fix the latency.
[[nodiscard]] const EffectEngine& engine();

// The oversampling factors, indexed by edge.oversampling.
inline constexpr std::array<std::uint32_t, 5> factors{1, 2, 4, 8, 16};

// The static curve: the output for input `x` (linear, the ceiling at 1) at
// `shape` (0 hard, 1 soft). Odd, monotonic, never above 1 in magnitude, and
// exactly x up to the knee.
[[nodiscard]] double clip(double x, double shape) noexcept;

// The latency in frames for a factor index and filter choice at `sample_rate`
// (the engine's latency_frames for those values).
[[nodiscard]] std::uint32_t latency_frames(std::size_t oversampling, std::size_t filters,
                                           double sample_rate);

// Beats of clipping history in the telemetry: four bars of 4/4.
inline constexpr std::size_t history_beats = 16;

// Telemetry: the input after Input gain (peak dBFS, held 1 s) and the output
// before Delta and Compare (true peak dBTP, held 1 s) per channel; how far
// the loudest peak was clipped (dB, positive, held 1 s); the transfer dot
// (the input's recent peak in dBFS and the curve's output for it); the
// latency in milliseconds; the oversampling factor; then the most clipping
// in each of the last history_beats beats (dB, oldest first; half a second
// each while the transport is stopped).
namespace telemetry {
inline constexpr std::size_t input_peak = 0;  // left, right
inline constexpr std::size_t output_peak = 2; // left, right
inline constexpr std::size_t clipped = 4;
inline constexpr std::size_t dot_input = 5;
inline constexpr std::size_t dot_output = 6;
inline constexpr std::size_t latency_ms = 7;
inline constexpr std::size_t factor = 8;
inline constexpr std::size_t history = 9;
inline constexpr std::size_t size = history + history_beats;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::edge
