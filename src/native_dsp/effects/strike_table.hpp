#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Strike v1: a transient shaper (ADR-0118). Four bands are always in the
// table; Bands says how many play (one: no split). Phase, Lookahead and
// Clip set the latency, so they are not automatable: a change rebuilds the
// route.
// The band count changes the structure, so it is not automatable either.
// Ids are persisted and automated: never rename or reuse one. The four
// bands share one pattern: keep them identical but for their number.
namespace oscillate::native_dsp::effects::strike_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array off_on{"Off"sv, "On"sv};
inline constexpr std::array band_counts{"1"sv, "2"sv, "3"sv, "4"sv};
// Minimum: Linkwitz-Riley crossovers, no latency. Linear: a linear-phase
// FIR bank whose bands sum to the input delayed (its latency).
inline constexpr std::array phases{"Minimum"sv, "Linear"sv};
enum Phase : std::uint8_t { minimum, linear };
inline constexpr std::array lookaheads{"Off"sv, "0.5 ms"sv, "1 ms"sv, "2 ms"sv, "5 ms"sv};
inline constexpr std::array<double, 5> lookahead_ms{0, 0.5, 1, 2, 5};

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = effect_glide_seconds;

// The gain range shared by Attack and Sustain, either way.
inline constexpr double shape_limit_db = 18.0;
inline constexpr double attack_time_min = 1.0;
inline constexpr double attack_time_max = 100.0;
inline constexpr double attack_time_default = 10.0;
inline constexpr double sustain_time_min = 20.0;
inline constexpr double sustain_time_max = 2'000.0;
inline constexpr double sustain_time_default = 300.0;
inline constexpr double crossover_min = 30.0;
inline constexpr double crossover_max = 16'000.0;
inline constexpr std::array<double, 3> crossover_defaults{120.0, 1'000.0, 6'000.0};
inline constexpr double output_min = -24.0;
inline constexpr double output_max = 12.0;
inline constexpr double ceiling_min = -12.0;

// Per band, in this order from the band's first index.
namespace band {
inline constexpr std::size_t attack = 0;
inline constexpr std::size_t sustain = 1;
inline constexpr std::size_t solo = 2;
inline constexpr std::size_t size = 3;
} // namespace band

inline constexpr std::size_t global_count = 13;
inline constexpr std::size_t band_count = 4;
inline constexpr std::size_t first_band = global_count;

inline constexpr std::array<ParameterDescriptor, global_count + band_count * band::size> parameters{{
    {"strike.bands", "Bands", "Bands", "", S::choice, 0, last(band_counts.size()), 0, band_counts, T::live, 0, false},
    {"strike.xover_1", "Crossover 1", "Bands", "Hz", S::logarithmic, crossover_min, crossover_max, crossover_defaults[0], {}, T::live, g},
    {"strike.xover_2", "Crossover 2", "Bands", "Hz", S::logarithmic, crossover_min, crossover_max, crossover_defaults[1], {}, T::live, g},
    {"strike.xover_3", "Crossover 3", "Bands", "Hz", S::logarithmic, crossover_min, crossover_max, crossover_defaults[2], {}, T::live, g},
    {"strike.phase", "Phase", "Bands", "", S::choice, 0, last(phases.size()), Phase::minimum, phases, T::live, 0, false},
    {"strike.attack_time", "Attack Time", "Timing", "ms", S::logarithmic, attack_time_min, attack_time_max, attack_time_default, {}, T::live, g},
    {"strike.sustain_time", "Sustain Time", "Timing", "ms", S::logarithmic, sustain_time_min, sustain_time_max, sustain_time_default, {}, T::live, g},
    {"strike.lookahead", "Lookahead", "Timing", "", S::choice, 0, last(lookaheads.size()), 0, lookaheads, T::live, 0, false},
    {"strike.link", "Stereo Link", "Output", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"strike.mix", "Mix", "Output", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"strike.output", "Output", "Output", "dB", S::linear, output_min, output_max, 0, {}, T::live, g},
    {"strike.clip", "Clip", "Output", "", S::choice, 0, 1, 0, off_on, T::live, 0, false},
    {"strike.ceiling", "Ceiling", "Output", "dB", S::linear, ceiling_min, 0, 0, {}, T::live, g},
    {"strike.band_1.attack", "Attack", "Band 1", "dB", S::linear, -shape_limit_db, shape_limit_db, 0, {}, T::live, g},
    {"strike.band_1.sustain", "Sustain", "Band 1", "dB", S::linear, -shape_limit_db, shape_limit_db, 0, {}, T::live, g},
    {"strike.band_1.solo", "Solo", "Band 1", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"strike.band_2.attack", "Attack", "Band 2", "dB", S::linear, -shape_limit_db, shape_limit_db, 0, {}, T::live, g},
    {"strike.band_2.sustain", "Sustain", "Band 2", "dB", S::linear, -shape_limit_db, shape_limit_db, 0, {}, T::live, g},
    {"strike.band_2.solo", "Solo", "Band 2", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"strike.band_3.attack", "Attack", "Band 3", "dB", S::linear, -shape_limit_db, shape_limit_db, 0, {}, T::live, g},
    {"strike.band_3.sustain", "Sustain", "Band 3", "dB", S::linear, -shape_limit_db, shape_limit_db, 0, {}, T::live, g},
    {"strike.band_3.solo", "Solo", "Band 3", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"strike.band_4.attack", "Attack", "Band 4", "dB", S::linear, -shape_limit_db, shape_limit_db, 0, {}, T::live, g},
    {"strike.band_4.sustain", "Sustain", "Band 4", "dB", S::linear, -shape_limit_db, shape_limit_db, 0, {}, T::live, g},
    {"strike.band_4.solo", "Solo", "Band 4", "", S::choice, 0, 1, 0, off_on, T::live, 0},
}};

consteval std::size_t index_of(std::string_view id) { return parameter_index(parameters, id); }

} // namespace oscillate::native_dsp::effects::strike_table

// clang-format on
