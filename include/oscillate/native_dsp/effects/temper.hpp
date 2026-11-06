#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace oscillate::native_dsp::effects::temper {

// Temper (ADR-0064): a resonance suppressor with dynamic EQ bands. Each hop
// an STFT measures the processed channels on a 1/24-octave grid; a band
// that stands out of its neighbourhood (by Detail, leaned by Tilt) is cut
// in proportion to Depth, weighted by the drawn sensitivity curve and by
// Split (attacks against sustains), up to Max Cut. Unmask adds a cut where
// the sidechain lives, while it plays. Eight dynamic bands add their own
// bell or shelf, moved by their level against a threshold. Every gain
// follows Attack and Release (leaned by Tilt), and the curve becomes one
// filter per hop: minimum phase (Natural), linear phase (Linear) or a short
// minimum-phase filter with no latency (Low Latency). Mix blends gains, so
// a half mix never combs; Delta plays only what the filter takes away.
[[nodiscard]] const EffectEngine& engine();

// Hop, filter and lookahead for these values at this rate.
struct Frames {
  std::size_t window = 0;    // analysis
  std::size_t hop = 0;       // gains and filters change once per hop
  std::size_t filter = 0;    // FIR taps
  std::size_t lookahead = 0; // analysis ahead of the filtered block
  std::uint32_t latency = 0;
};
[[nodiscard]] Frames frames_for(std::span<const double> values, double sample_rate) noexcept;

// The panel's display: display_bands log-spaced bands from 20 Hz to 20 kHz
// (band i centred at 20 * 1000^((i + 0.5) / n)).
inline constexpr std::size_t display_bands = 120;
inline constexpr std::size_t bands = 8;

// Telemetry: input and output peak (dBFS, held 1 s) per channel; the
// deepest cut now (dB, positive, held 1 s); 1 when a sidechain key is
// connected; how much of the spectrum is in an attack now (0..1); the
// latency (ms); each dynamic band's gain now (dB) and its detected level
// (dBFS); then per display band: the input level, the output level, the
// sidechain level (dBFS, smoothed for the eye) and the gain applied now (dB;
// negative is a cut).
namespace telemetry {
inline constexpr std::size_t input_peak = 0;  // left, right
inline constexpr std::size_t output_peak = 2; // left, right
inline constexpr std::size_t max_cut = 4;
inline constexpr std::size_t keyed = 5;
inline constexpr std::size_t transient = 6;
inline constexpr std::size_t latency_ms = 7;
inline constexpr std::size_t band_gain = 8;
inline constexpr std::size_t band_level = band_gain + bands;
inline constexpr std::size_t input_spectrum = band_level + bands;
inline constexpr std::size_t output_spectrum = input_spectrum + display_bands;
inline constexpr std::size_t sidechain_spectrum = output_spectrum + display_bands;
inline constexpr std::size_t gain = sidechain_spectrum + display_bands;
inline constexpr std::size_t size = gain + display_bands;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::temper
