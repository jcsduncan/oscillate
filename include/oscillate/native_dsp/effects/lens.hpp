#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"

#include <cstddef>

namespace oscillate::native_dsp::effects::lens {

// Lens (ADR-0042): a spectrum analyser that passes audio untouched.
// - Three window sizes (Fast 4096/1024/256, Balanced 16384/4096/1024,
//   Precise 32768/8192/2048, Blackman-Harris) are analysed every hop
//   (10.7 ms) on the absolute frame grid. Each display point uses the
//   shortest window no more than twice as coarse as its band, crossfaded
//   between sizes, so lows resolve finely while highs follow transients.
// - Each point is the power in a fractional-octave band, normalised so a
//   sine reads its level and pink noise is flat with no tilt; then
//   power-averaged, tilted about 1 kHz and peak-held. Where a window cannot
//   resolve the band (Balanced below about 80 Hz at a third of an octave)
//   the band widens to its main lobe: sines still read true, noise reads
//   higher.
// - Two curves follow the view (Mid, Side, Left, Right, or a pair), and the
//   sidechain can be overlaid for reference matching.
[[nodiscard]] const EffectEngine& engine();

inline constexpr std::size_t points = 400;
inline constexpr double lowest_hz = 20.0;
inline constexpr double highest_hz = 20'000.0;
inline constexpr std::size_t peak_count = 8;
// Display point frequencies, log-spaced from lowest_hz to highest_hz.
[[nodiscard]] double point_hz(std::size_t point) noexcept;

// Telemetry, in dB: the first and second curves (the second is silent,
// -200, for a single view), the sidechain overlay (-200 when off), the
// first curve's peak hold; then the strongest resonances of the first
// curve as (hz, dB) pairs, strongest first (0 when fewer); then the count
// of hops analysed.
namespace telemetry {
inline constexpr std::size_t curve_a = 0;
inline constexpr std::size_t curve_b = curve_a + points;
inline constexpr std::size_t overlay = curve_b + points;
inline constexpr std::size_t hold = overlay + points;
inline constexpr std::size_t peaks = hold + points;
inline constexpr std::size_t hops = peaks + 2 * peak_count;
inline constexpr std::size_t size = hops + 1;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::lens
