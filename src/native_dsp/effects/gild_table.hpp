#pragma once

#include "edge_table.hpp"
#include "forge_table.hpp"
#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Gild v1: a multiband exciter (ADR-0122). Four bands are always in the
// table; Bands says how many play (one: no split). Phase and Oversampling
// set the latency, and the band count the structure, so none of the three
// is automatable: a change rebuilds the route. Ids are persisted and
// automated: never rename or reuse one. The four bands share one pattern:
// keep them identical but for their number.
namespace oscillate::native_dsp::effects::gild_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array off_on{"Off"sv, "On"sv};
inline constexpr std::array band_counts{"1"sv, "2"sv, "3"sv, "4"sv};
// Minimum: Linkwitz-Riley crossovers. Linear: the linear-phase FIR bank,
// whose bands sum to the input delayed (its latency).
inline constexpr std::array phases{"Minimum"sv, "Linear"sv};
enum Phase : std::uint8_t { minimum, linear };
// Edge's oversampling factors (edge_table::Oversampling).
inline constexpr std::array oversampling = edge_table::oversampling;
// Forge's four smooth curves, in Forge's order (forge_table::Model).
inline constexpr std::array curves{"Tape"sv, "Tube"sv, "Transistor"sv, "Diode"sv};
static_assert(curves[forge_table::tape] == forge_table::models[forge_table::tape]);
static_assert(curves[forge_table::tube] == forge_table::models[forge_table::tube]);
static_assert(curves[forge_table::transistor] == forge_table::models[forge_table::transistor]);
static_assert(curves[forge_table::diode] == forge_table::models[forge_table::diode]);

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = effect_glide_seconds;

inline constexpr double crossover_min = 30.0;
inline constexpr double crossover_max = 16'000.0;
inline constexpr std::array<double, 3> crossover_defaults{150.0, 1'500.0, 7'000.0};
inline constexpr double output_min = -24.0;
inline constexpr double output_max = 12.0;

// Per band, in this order from the band's first index.
namespace band {
inline constexpr std::size_t drive = 0;
inline constexpr std::size_t curve = 1;
inline constexpr std::size_t even = 2;
inline constexpr std::size_t mix = 3;
inline constexpr std::size_t solo = 4;
inline constexpr std::size_t size = 5;
} // namespace band

inline constexpr std::size_t global_count = 10;
inline constexpr std::size_t band_count = 4;
inline constexpr std::size_t first_band = global_count;

inline constexpr std::array<ParameterDescriptor, global_count + band_count * band::size> parameters{{
    {"gild.bands", "Bands", "Bands", "", S::choice, 0, last(band_counts.size()), 3, band_counts, T::live, 0, false},
    {"gild.xover_1", "Crossover 1", "Bands", "Hz", S::logarithmic, crossover_min, crossover_max, crossover_defaults[0], {}, T::live, g},
    {"gild.xover_2", "Crossover 2", "Bands", "Hz", S::logarithmic, crossover_min, crossover_max, crossover_defaults[1], {}, T::live, g},
    {"gild.xover_3", "Crossover 3", "Bands", "Hz", S::logarithmic, crossover_min, crossover_max, crossover_defaults[2], {}, T::live, g},
    {"gild.phase", "Phase", "Bands", "", S::choice, 0, last(phases.size()), Phase::minimum, phases, T::live, 0, false},
    {"gild.oversampling", "Oversampling", "Quality", "", S::choice, 0, last(oversampling.size()), edge_table::x4, oversampling, T::live, 0, false},
    {"gild.dynamic", "Dynamic", "Drive", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"gild.match", "Level Match", "Output", "", S::choice, 0, 1, 1, off_on, T::live, 0},
    {"gild.delta", "Delta", "Output", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"gild.output", "Output", "Output", "dB", S::linear, output_min, output_max, 0, {}, T::live, g},
    {"gild.band_1.drive", "Drive", "Band 1", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"gild.band_1.curve", "Curve", "Band 1", "", S::choice, 0, last(curves.size()), forge_table::tape, curves, T::live, 0},
    {"gild.band_1.even", "Even", "Band 1", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"gild.band_1.mix", "Mix", "Band 1", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"gild.band_1.solo", "Solo", "Band 1", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"gild.band_2.drive", "Drive", "Band 2", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"gild.band_2.curve", "Curve", "Band 2", "", S::choice, 0, last(curves.size()), forge_table::tape, curves, T::live, 0},
    {"gild.band_2.even", "Even", "Band 2", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"gild.band_2.mix", "Mix", "Band 2", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"gild.band_2.solo", "Solo", "Band 2", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"gild.band_3.drive", "Drive", "Band 3", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"gild.band_3.curve", "Curve", "Band 3", "", S::choice, 0, last(curves.size()), forge_table::tape, curves, T::live, 0},
    {"gild.band_3.even", "Even", "Band 3", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"gild.band_3.mix", "Mix", "Band 3", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"gild.band_3.solo", "Solo", "Band 3", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"gild.band_4.drive", "Drive", "Band 4", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"gild.band_4.curve", "Curve", "Band 4", "", S::choice, 0, last(curves.size()), forge_table::tape, curves, T::live, 0},
    {"gild.band_4.even", "Even", "Band 4", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"gild.band_4.mix", "Mix", "Band 4", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"gild.band_4.solo", "Solo", "Band 4", "", S::choice, 0, 1, 0, off_on, T::live, 0},
}};

consteval std::size_t index_of(std::string_view id) { return parameter_index(parameters, id); }

} // namespace oscillate::native_dsp::effects::gild_table

// clang-format on
