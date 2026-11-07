#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace oscillate::quick {

// Contour (native effect) sketch: the filter's measured magnitude in dB at
// `points` log-spaced frequencies from 20 Hz to 20 kHz, from an impulse run
// through the real effect at 48 kHz with its LFO, envelope and dry mix off,
// so every model draws exactly as it sounds. `cutoff_hz` > 0 replaces the
// panel cutoff (the live, modulated one). Points read their exact frequency,
// so notches show their depth; the combs keep each slice's loudest bin, so
// their narrow peaks show. Empty for other engines.
[[nodiscard]] std::vector<double>
contour_response(const native_dsp::synth::EngineDescriptor& engine, std::size_t points,
                 std::span<const double> values, double cutoff_hz);

// Shape (native effect) sketch: the EQ's magnitude in dB at `points`
// log-spaced frequencies from 20 Hz to 20 kHz, evaluated on the classic EQ's
// own prepared biquad cascade at 48 kHz, so the curve is the one the EQ
// runs. `band` < 0 gives the bands that are on together (their dB sum);
// 0 to 7 gives that band alone, empty while it is off. Empty for other
// engines.
[[nodiscard]] std::vector<double> shape_response(const native_dsp::synth::EngineDescriptor& engine,
                                                 std::size_t points, std::span<const double> values,
                                                 int band);

// Press (native effect) sketch: the static transfer curve, the output level
// in dB (makeup included) at `points` steady input levels evenly spaced from
// `floor_db` (below 0) to 0 dBFS, through the classic compressor's own gain
// computer. Empty for other engines.
[[nodiscard]] std::vector<double> press_transfer(const native_dsp::synth::EngineDescriptor& engine,
                                                 double floor_db, std::span<const double> values,
                                                 std::size_t points);

// Strata (native effect) sketches, empty for other engines:
//  - strata_transfer: band `band`'s static curve, output dB (makeup
//    included) at `points` steady levels evenly spaced from `floor_db` to 0;
//  - strata_crossovers: the crossovers as they play at `sample_rate`;
//  - strata_band_shapes: per playing band, its magnitude (linear) at
//    `points` log-spaced frequencies from 20 Hz to 20 kHz;
//  - strata_ott_values: the plain values (id, value) that write OTT mode
//    out as bands, for a panel taking a band over.
[[nodiscard]] std::vector<double> strata_transfer(const native_dsp::synth::EngineDescriptor& engine,
                                                  std::span<const double> values, int band,
                                                  double floor_db, std::size_t points);
[[nodiscard]] std::vector<double>
strata_crossovers(const native_dsp::synth::EngineDescriptor& engine, std::span<const double> values,
                  double sample_rate);
[[nodiscard]] std::vector<std::vector<double>>
strata_band_shapes(const native_dsp::synth::EngineDescriptor& engine,
                   std::span<const double> values, double sample_rate, std::size_t points);
[[nodiscard]] std::vector<native_dsp::synth::NamedValue>
strata_ott_values(const native_dsp::synth::EngineDescriptor& engine,
                  std::span<const double> values);

} // namespace oscillate::quick
