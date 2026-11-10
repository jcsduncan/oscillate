#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/tonal_targets.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace oscillate::native_dsp::effects::keel {

// Keel (ADR-0119): adaptive tonal balance. It tracks the programme's
// long-term spectrum in third-octave bands and steers it toward a target
// curve with slowly moving, minimum-phase EQ.
// - Tracking: a Hann-windowed STFT of both channels, summed to band powers
//   (pink noise reads flat), each hop. A hop quieter than the absolute gate
//   or more than relative_gate_db under the programme's own recent loudness
//   is ignored; one louder than it by transient_db is scaled down to that.
//   The bands then average over Speed seconds (a running mean until the
//   first Speed has passed), smoothed a third of an octave each way.
// - Comparison: the spectrum and the target are aligned by their
//   K-weighted means; the difference, times Amount and each region's
//   weight, within Max Boost and Max Cut, is the correction. Bands more
//   than absent_db under the K-weighted power mean hold no content: they
//   are left out of the means and never boosted.
// - Filtering: a bank of state-variable shelves and bells, an octave apart,
//   whose gains are fitted to the correction by least squares each hop,
//   ramp across it and are smoothed over a hop; the filters are designed
//   every coefficient_frames and their coefficients glide between, frame
//   by frame. No latency. Level Match takes out the loudness the
//   correction adds, estimated from the tracked spectrum.
// - Target: Tilt (dB/oct about 1 kHz), the 31-value Curve plus the tilt,
//   or the sidechain's tracked balance plus the tilt (Reference). With no
//   reference heard yet, Keel holds its correction at 0 dB.
// Amount 0 passes the input bit for bit.
[[nodiscard]] const EffectEngine& engine();

// The target grid: third-octave bands, 1 kHz times 2^((band - 17) / 3),
// from 19.7 Hz (nominally 20 Hz) to 20.2 kHz (nominally 20 kHz). A target
// curve is one dB value per band; only its shape counts, not its level.
inline constexpr std::size_t bands = 31;
inline constexpr std::size_t reference_band = 17;
inline constexpr double reference_hz = 1'000.0;
inline constexpr double bands_per_octave = 3.0;
using Curve = std::array<double, bands>;
[[nodiscard]] double band_hz(std::size_t band) noexcept;

// The target the values describe before any reference: the tilt alone, or
// the drawn curve plus the tilt (Tilt and Reference read only the tilt).
[[nodiscard]] Curve target_curve(std::span<const double> values) noexcept;
// A tonal target's curve (ADR-0120: its median, or either end of its
// range) on Keel's grid. The tonal grid is Keel's less its outermost band
// at each end (its band 0 is Keel's band 1); those two take their
// neighbour's level. The result is centred on its mean: only its shape
// counts.
[[nodiscard]] Curve curve_from(const tonal::Curve& levels) noexcept;
// Writes a curve (a drawn, learned or genre target) into the values' curve
// parameters, within their range.
void set_curve(std::span<double> values, const Curve& curve) noexcept;
// Learn: the curve that makes the target the balance in a telemetry frame
// (its spectrum less the values' tilt), within the curve's range.
[[nodiscard]] Curve learned_curve(std::span<const float> telemetry,
                                  std::span<const double> values) noexcept;

// The tracker's constants.
inline constexpr double absolute_gate_db = -70.0;
inline constexpr double relative_gate_db = 20.0;
inline constexpr double transient_db = 6.0;
inline constexpr double absent_db = 45.0;
// The filters are designed for the gliding gains this often; their
// coefficients move linearly between, every frame.
inline constexpr std::size_t coefficient_frames = 16;

// The correction filters: a low shelf at 20 Hz, bells an octave apart from
// 31.25 Hz to 8 kHz, a high shelf at 12 kHz.
inline constexpr std::size_t filters = 11;

// The view of the correction over time: this many slices, one each
// history_seconds.
inline constexpr std::size_t history_slices = 48;
inline constexpr double history_seconds = 0.5;

// Telemetry: input and output peak (dBFS, falling 60 dB a second) per
// channel; the latency (ms, always 0); Level Match's gain now (dB); 1 when
// the input has been tracked and when a reference has (else 0); the target
// source as it plays; then per band the target and the tracked spectrum,
// each less its K-weighted mean (dB; -200 before anything is tracked), and
// the correction now (dB); then history_slices corrections, oldest first.
namespace telemetry {
inline constexpr std::size_t input_peak = 0;  // left, right
inline constexpr std::size_t output_peak = 2; // left, right
inline constexpr std::size_t latency_ms = 4;
inline constexpr std::size_t match_db = 5;
inline constexpr std::size_t tracked = 6;
inline constexpr std::size_t referenced = 7;
inline constexpr std::size_t target_source = 8;
inline constexpr std::size_t target = 9;
inline constexpr std::size_t spectrum = target + bands;
inline constexpr std::size_t correction = spectrum + bands;
inline constexpr std::size_t history = correction + bands;
inline constexpr std::size_t size = history + history_slices * bands;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::keel
