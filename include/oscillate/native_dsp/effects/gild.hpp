#pragma once

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace oscillate::native_dsp::effects::gild {

// Gild (ADR-0122): a multiband exciter. The input splits into up to four
// bands, by Linkwitz-Riley crossovers (Phase: Minimum) or the linear-phase
// FIR bank (Linear), as Strike's. Each band runs at the oversampled rate
// (Edge's linear-phase half-bands) through one of Forge's curves, driven by
// Drive and biased by Even, and normalised to unit slope at its bias, so
// small signals pass at their own level and only the curve's bend is
// added. That bend (the residue: the curve's output less its input) is
// what goes back down, so a band at zero drive adds exactly nothing.
// From 8x the curve is antiderivative-antialiased. Level Match scales each
// excited band to the dry band's power over match_seconds; Dynamic drives
// a band up to dynamic_range_db harder as it falls under
// dynamic_reference_db; Mix blends each band with its dry signal. Delta
// plays only what was added. Phase and Oversampling fix the latency.
[[nodiscard]] const EffectEngine& engine();

inline constexpr std::size_t max_bands = 4;
inline constexpr std::size_t max_crossovers = max_bands - 1;

// The gain into the curve for Drive (0..1): max_drive_db at 100%, going as
// Drive's cube, so the upper travel runs evenly in decibels (25% is
// 0 dB, 50% +18 dB) and 0% is no drive at all.
inline constexpr double max_drive_db = 36.0;
inline constexpr double drive_exponent = 3.0;
[[nodiscard]] inline double drive_gain(double drive) noexcept {
  return dsp::db_to_gain(max_drive_db) * std::pow(drive, drive_exponent);
}
// Even at 100% biases the curve this far (curve units).
inline constexpr double max_bias = 0.5;
// Level Match follows both powers over this long, within +-match_limit_db.
inline constexpr double match_seconds = 0.4;
inline constexpr double match_limit_db = 36.0;
// Dynamic: the band's power over dynamic_seconds against this level (RMS,
// dBFS); at 100% a band 12 dB under it is driven 12 dB harder, up to
// dynamic_range_db.
inline constexpr double dynamic_reference_db = -18.0;
inline constexpr double dynamic_range_db = 12.0;
inline constexpr double dynamic_seconds = 0.08;

// The latency in frames for a linear phase and gild.oversampling's choice
// at `sample_rate` (the engine's latency_frames).
[[nodiscard]] std::uint32_t latency_frames(bool linear, std::size_t oversampling,
                                           double sample_rate);

// The spectrum of what is added: spectrum_bins bands spaced evenly in log
// frequency from spectrum_low_hz to spectrum_high_hz, from a
// spectrum_window-point Hann transform every spectrum_hop frames of the
// mono sums, smoothed over spectrum_seconds.
inline constexpr std::size_t spectrum_bins = 96;
inline constexpr double spectrum_low_hz = 20.0;
inline constexpr double spectrum_high_hz = 20'000.0;
inline constexpr std::size_t spectrum_window = 4'096;
inline constexpr std::size_t spectrum_hop = 2'048;
inline constexpr double spectrum_seconds = 0.3;

// Telemetry: input and output peak (dBFS, falling 60 dB a second) per
// channel; the latency in milliseconds; the band count; the crossovers as
// they play (Hz, 0 when unused); per band its level and what it added
// (RMS dBFS of its share of the delta), both over match_seconds, and the
// drive into the curve with Dynamic's lift (dB, -120 at zero drive); then
// the dry input's and the added signal's spectra (dBFS per bin).
namespace telemetry {
inline constexpr std::size_t input_peak = 0;  // left, right
inline constexpr std::size_t output_peak = 2; // left, right
inline constexpr std::size_t latency_ms = 4;
inline constexpr std::size_t band_count = 5;
inline constexpr std::size_t crossovers = 6;
inline constexpr std::size_t band_level = crossovers + max_crossovers;
inline constexpr std::size_t band_added = band_level + max_bands;
inline constexpr std::size_t band_drive = band_added + max_bands;
inline constexpr std::size_t input_spectrum = band_drive + max_bands;
inline constexpr std::size_t added_spectrum = input_spectrum + spectrum_bins;
inline constexpr std::size_t size = added_spectrum + spectrum_bins;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::gild
