#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/eq.hpp"
#include "oscillate/native_dsp/match_eq.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace oscillate::native_dsp::effects::shape {

// Shape (ADR-0047): the classic native EQ as an engine. Eight bands of on,
// kind, frequency, Q (a shelf's slope), gain and a pass filter's slope (12
// to 96 dB an octave, as 1 to 8 stages). The stages of the bands that are on
// run in band order through the classic EqRuntime, so a patch renders bit
// for bit as the classic EQ given the same bands. Live edits use the classic
// runtime's own transitions: coefficients glide over 64 frames and a stage
// added or removed (a band switched on or off, a slope changed) fades in or
// out, one stage at a time, without resetting the others.
[[nodiscard]] const EffectEngine& engine();

inline constexpr std::size_t bands = eq_band_limit;

// A band as the classic EQ runs it: its stages, none when it is off.
struct BandStages {
  std::array<EqBandSettings, eq_max_pass_stages> stage{};
  std::size_t size = 0;
};

// Band `band` (from 0) as the classic EQ runs it. Values the classic EQ does
// not admit are brought into its range: a shelf's slope is at most 1, a pass
// filter has no gain, and the frequency is at most min(20 kHz, 0.45 * rate).
// A pass filter is its slope's stages (eq_pass_stages); the others are one.
[[nodiscard]] BandStages band_stages(std::span<const double> values, std::size_t band,
                                     double sample_rate) noexcept;

// Match EQ (ADR-0120): the values that make Shape play `bands` (at most
// `bands` of them, in order) and nothing else, as (id, value) pairs: each
// band's On, Kind, Frequency, Q (a shelf's slope) and Gain, and every band
// past the list off.
[[nodiscard]] std::vector<std::pair<std::string_view, double>>
band_values(std::span<const match_eq::Band> bands);

// Telemetry: input and output peak (dBFS, held 1 s) per channel.
// The display spectra: the mono sum's power in spectrum_bands log-spaced
// bands from 20 Hz to 20 kHz (band i centred at 20 * 1000^((i + 0.5) / n)),
// in dBFS, averaged over about a quarter second; before and after the EQ.
inline constexpr std::size_t spectrum_bands = 96;
namespace telemetry {
inline constexpr std::size_t input_peak = 0;  // left, right
inline constexpr std::size_t output_peak = 2; // left, right
inline constexpr std::size_t input_spectrum = 4;
inline constexpr std::size_t output_spectrum = input_spectrum + spectrum_bands;
inline constexpr std::size_t size = output_spectrum + spectrum_bands;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::shape
