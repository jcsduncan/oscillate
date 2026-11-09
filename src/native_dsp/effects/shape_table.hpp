#pragma once

#include "oscillate/native_dsp/eq.hpp"
#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Shape v1: the classic native EQ as an engine (ADR-0047). Eight bands of
// on, kind, frequency, Q, gain and slope, with the classic EQ's ranges:
// frequency 10 Hz to 20 kHz (at most 0.45 of the rate), Q 0.1 to 10 (a
// shelf's slope, at most 1) and gain +-24 dB (pass filters have none). A pass
// filter cuts at 12 to 96 dB an octave (Slope); the other kinds ignore it. Every band is off by
// default, so the defaults pass the input bit for bit. Ids are persisted and
// automated: never rename or reuse one.
namespace oscillate::native_dsp::effects::shape_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::size_t bands = 8;
inline constexpr std::size_t per_band = 6;

inline constexpr std::array off_on{"Off"sv, "On"sv};
// In native_dsp::EqBandKind order.
inline constexpr std::array kinds{"Low Pass"sv, "High Pass"sv, "Low Shelf"sv, "High Shelf"sv,
                                  "Peak"sv};
enum Kind : std::uint8_t { low_pass, high_pass, low_shelf, high_shelf, peaking };
// A pass filter's steepness: this many 12 dB/octave stages (the others have
// one). 12 dB is the classic EQ's band.
inline constexpr std::array slopes{"12 dB"sv, "24 dB"sv, "48 dB"sv, "96 dB"sv};
inline constexpr std::array<std::size_t, slopes.size()> slope_stages{1, 2, 4, 8};
static_assert(slope_stages.back() == eq_max_pass_stages);

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = effect_glide_seconds;

inline constexpr std::array<ParameterDescriptor, bands * per_band> parameters{{
    {"shape.on_1", "On", "Band 1", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"shape.kind_1", "Kind", "Band 1", "", S::choice, 0, last(kinds.size()), Kind::high_pass, kinds,
     T::live, 0},
    {"shape.freq_1", "Frequency", "Band 1", "Hz", S::logarithmic, 10, 20'000, 30, {}, T::live, g},
    {"shape.q_1", "Q", "Band 1", "", S::logarithmic, 0.1, 10, 0.707, {}, T::live, g},
    {"shape.gain_1", "Gain", "Band 1", "dB", S::linear, -24, 24, 0, {}, T::live, g},
    {"shape.slope_1", "Slope", "Band 1", "", S::choice, 0, last(slopes.size()), 0, slopes, T::live,
     0},
    {"shape.on_2", "On", "Band 2", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"shape.kind_2", "Kind", "Band 2", "", S::choice, 0, last(kinds.size()), Kind::low_shelf, kinds,
     T::live, 0},
    {"shape.freq_2", "Frequency", "Band 2", "Hz", S::logarithmic, 10, 20'000, 100, {}, T::live, g},
    {"shape.q_2", "Q", "Band 2", "", S::logarithmic, 0.1, 10, 1, {}, T::live, g},
    {"shape.gain_2", "Gain", "Band 2", "dB", S::linear, -24, 24, 0, {}, T::live, g},
    {"shape.slope_2", "Slope", "Band 2", "", S::choice, 0, last(slopes.size()), 0, slopes, T::live,
     0},
    {"shape.on_3", "On", "Band 3", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"shape.kind_3", "Kind", "Band 3", "", S::choice, 0, last(kinds.size()), Kind::peaking, kinds,
     T::live, 0},
    {"shape.freq_3", "Frequency", "Band 3", "Hz", S::logarithmic, 10, 20'000, 250, {}, T::live, g},
    {"shape.q_3", "Q", "Band 3", "", S::logarithmic, 0.1, 10, 1, {}, T::live, g},
    {"shape.gain_3", "Gain", "Band 3", "dB", S::linear, -24, 24, 0, {}, T::live, g},
    {"shape.slope_3", "Slope", "Band 3", "", S::choice, 0, last(slopes.size()), 0, slopes, T::live,
     0},
    {"shape.on_4", "On", "Band 4", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"shape.kind_4", "Kind", "Band 4", "", S::choice, 0, last(kinds.size()), Kind::peaking, kinds,
     T::live, 0},
    {"shape.freq_4",
     "Frequency",
     "Band 4",
     "Hz",
     S::logarithmic,
     10,
     20'000,
     1'000,
     {},
     T::live,
     g},
    {"shape.q_4", "Q", "Band 4", "", S::logarithmic, 0.1, 10, 0.707, {}, T::live, g},
    {"shape.gain_4", "Gain", "Band 4", "dB", S::linear, -24, 24, 0, {}, T::live, g},
    {"shape.slope_4", "Slope", "Band 4", "", S::choice, 0, last(slopes.size()), 0, slopes, T::live,
     0},
    {"shape.on_5", "On", "Band 5", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"shape.kind_5", "Kind", "Band 5", "", S::choice, 0, last(kinds.size()), Kind::peaking, kinds,
     T::live, 0},
    {"shape.freq_5",
     "Frequency",
     "Band 5",
     "Hz",
     S::logarithmic,
     10,
     20'000,
     2'500,
     {},
     T::live,
     g},
    {"shape.q_5", "Q", "Band 5", "", S::logarithmic, 0.1, 10, 1, {}, T::live, g},
    {"shape.gain_5", "Gain", "Band 5", "dB", S::linear, -24, 24, 0, {}, T::live, g},
    {"shape.slope_5", "Slope", "Band 5", "", S::choice, 0, last(slopes.size()), 0, slopes, T::live,
     0},
    {"shape.on_6", "On", "Band 6", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"shape.kind_6", "Kind", "Band 6", "", S::choice, 0, last(kinds.size()), Kind::peaking, kinds,
     T::live, 0},
    {"shape.freq_6",
     "Frequency",
     "Band 6",
     "Hz",
     S::logarithmic,
     10,
     20'000,
     6'000,
     {},
     T::live,
     g},
    {"shape.q_6", "Q", "Band 6", "", S::logarithmic, 0.1, 10, 1, {}, T::live, g},
    {"shape.gain_6", "Gain", "Band 6", "dB", S::linear, -24, 24, 0, {}, T::live, g},
    {"shape.slope_6", "Slope", "Band 6", "", S::choice, 0, last(slopes.size()), 0, slopes, T::live,
     0},
    {"shape.on_7", "On", "Band 7", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"shape.kind_7", "Kind", "Band 7", "", S::choice, 0, last(kinds.size()), Kind::high_shelf,
     kinds, T::live, 0},
    {"shape.freq_7",
     "Frequency",
     "Band 7",
     "Hz",
     S::logarithmic,
     10,
     20'000,
     10'000,
     {},
     T::live,
     g},
    {"shape.q_7", "Q", "Band 7", "", S::logarithmic, 0.1, 10, 1, {}, T::live, g},
    {"shape.gain_7", "Gain", "Band 7", "dB", S::linear, -24, 24, 0, {}, T::live, g},
    {"shape.slope_7", "Slope", "Band 7", "", S::choice, 0, last(slopes.size()), 0, slopes, T::live,
     0},
    {"shape.on_8", "On", "Band 8", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"shape.kind_8", "Kind", "Band 8", "", S::choice, 0, last(kinds.size()), Kind::low_pass, kinds,
     T::live, 0},
    {"shape.freq_8",
     "Frequency",
     "Band 8",
     "Hz",
     S::logarithmic,
     10,
     20'000,
     18'000,
     {},
     T::live,
     g},
    {"shape.q_8", "Q", "Band 8", "", S::logarithmic, 0.1, 10, 0.707, {}, T::live, g},
    {"shape.gain_8", "Gain", "Band 8", "dB", S::linear, -24, 24, 0, {}, T::live, g},
    {"shape.slope_8", "Slope", "Band 8", "", S::choice, 0, last(slopes.size()), 0, slopes, T::live,
     0},
}};

consteval std::size_t index_of(std::string_view id) { return parameter_index(parameters, id); }

// Band b (from 0) is the six parameters from b * per_band, in this order.
inline constexpr std::size_t on = 0;
inline constexpr std::size_t kind = 1;
inline constexpr std::size_t freq = 2;
inline constexpr std::size_t q = 3;
inline constexpr std::size_t gain = 4;
inline constexpr std::size_t slope = 5;
static_assert(index_of("shape.on_1") == on && index_of("shape.slope_1") == slope &&
              index_of("shape.on_8") == 7 * per_band + on &&
              index_of("shape.slope_8") == 7 * per_band + slope);

} // namespace oscillate::native_dsp::effects::shape_table
