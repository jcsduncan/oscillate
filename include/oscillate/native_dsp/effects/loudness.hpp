#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"

#include <cstddef>

namespace oscillate::native_dsp::effects::loudness {

// Loudness (ADR-0042): an EBU R128 / ITU-R BS.1770-4 meter that passes
// audio untouched. It runs the project's one BS.1770 core, the same as the
// export analyser and the master true-peak meter, so all three agree.
// - K-weighting (the BS.1770 shelf and RLB high-pass), designed per rate
//   from the analog prototype, summed over 100 ms steps.
// - Momentary (400 ms) and short-term (3 s) loudness.
// - Integrated loudness gated as BS.1770-4 specifies: 400 ms blocks every
//   100 ms, an absolute gate at -70 LUFS and a relative gate 10 LU below,
//   exactly over every block for two hours, then from running 0.01 LU
//   histograms.
// - Loudness range (EBU Tech 3342): short-term values every 100 ms, gated
//   at -70 LUFS and 20 LU below, the 10th to 95th percentile.
// - True peak: a 4x, 33-tap windowed-sinc interpolator (within 0.006 dB on
//   sines to 20 kHz), overall and over the last three seconds, with PSR and
//   PLR. It reads 16 samples late.
[[nodiscard]] const EffectEngine& engine();

inline constexpr std::size_t history = 300; // 30 s at 10 Hz.
inline constexpr double silent = -200.0;

// Telemetry: loudness in LUFS (silent when not yet measurable), ranges in
// LU, peaks in dBTP / dBFS, durations in seconds; then the momentary and
// short-term histories, oldest first.
namespace telemetry {
inline constexpr std::size_t momentary = 0;
inline constexpr std::size_t short_term = 1;
inline constexpr std::size_t integrated = 2;
inline constexpr std::size_t range = 3;
inline constexpr std::size_t momentary_max = 4;
inline constexpr std::size_t short_term_max = 5;
inline constexpr std::size_t true_peak = 6; // Overall, both channels.
inline constexpr std::size_t true_peak_left = 7;
inline constexpr std::size_t true_peak_right = 8;
inline constexpr std::size_t true_peak_recent = 9; // The last three seconds.
inline constexpr std::size_t sample_peak = 10;
inline constexpr std::size_t psr = 11;      // Recent true peak over short-term.
inline constexpr std::size_t plr = 12;      // True peak over integrated.
inline constexpr std::size_t duration = 13; // Seconds integrated.
inline constexpr std::size_t momentary_history = 14;
inline constexpr std::size_t short_term_history = momentary_history + history;
inline constexpr std::size_t size = short_term_history + history;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::loudness
