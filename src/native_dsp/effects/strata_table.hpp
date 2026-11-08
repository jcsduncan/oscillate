#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Strata v1: multiband dynamics (ADR-0082). Six bands are always in the
// table; Bands says how many play. Phase and Lookahead set the latency, so
// they are not automatable: a change rebuilds the route. Mode, the band
// count, the processed channel and the key change the structure, so they are
// not automatable either. Ids are persisted and automated: never rename or
// reuse one. The six bands share one pattern: keep them identical but for
// their number.
namespace oscillate::native_dsp::effects::strata_table {

using S = synth::ParameterScale;
using T = synth::ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array off_on{"Off"sv, "On"sv};
inline constexpr std::array band_counts{"1"sv, "2"sv, "3"sv, "4"sv, "5"sv, "6"sv};
inline constexpr std::array modes{"Bands"sv, "OTT"sv};
enum Mode : std::uint8_t { bands_mode, ott_mode };
inline constexpr std::array phases{"Minimum"sv, "Linear"sv};
enum Phase : std::uint8_t { minimum, linear };
inline constexpr std::array processes{"Stereo"sv, "Mid"sv, "Side"sv, "Left"sv, "Right"sv};
enum Process : std::uint8_t { stereo, mid, side, left, right };
inline constexpr std::array keys{"Band"sv, "Input"sv, "Sidechain"sv};
enum Key : std::uint8_t { band_key, input_key, sidechain_key };
inline constexpr std::array learn_states{"Off"sv, "Learn"sv};

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = synth::effect_glide_seconds;

// Ranges shared by every band.
inline constexpr double threshold_min = -60.0;
inline constexpr double threshold_max = 0.0;
inline constexpr double threshold_default = -18.0;
inline constexpr double ratio_min = 0.5;
inline constexpr double ratio_max = 20.0;
inline constexpr double below_min = 0.25;
inline constexpr double above_default = 2.0;
inline constexpr double below_default = 1.0;
inline constexpr double knee_max = 24.0;
inline constexpr double knee_default = 6.0;
inline constexpr double range_max = 36.0;
inline constexpr double range_default = 24.0;
inline constexpr double attack_min = 0.1;
inline constexpr double attack_max = 300.0;
inline constexpr double attack_default = 10.0;
inline constexpr double release_min = 5.0;
inline constexpr double release_max = 3'000.0;
inline constexpr double release_default = 120.0;
inline constexpr double makeup_limit = 24.0;
inline constexpr double crossover_min = 20.0;
inline constexpr double crossover_max = 20'000.0;
inline constexpr double lookahead_max_ms = 20.0;
inline constexpr std::array<double, 5> crossover_defaults{120.0, 1'000.0, 6'000.0, 10'000.0,
                                                          14'000.0};
// Bands' default index: four bands.
inline constexpr double bands_default = 3.0;
inline constexpr double ott_depth_default = 0.3;
inline constexpr double ott_time_default = 0.5;

// Per band, in this order from the band's first index.
namespace band {
inline constexpr std::size_t threshold = 0;
inline constexpr std::size_t above = 1;
inline constexpr std::size_t below = 2;
inline constexpr std::size_t knee = 3;
inline constexpr std::size_t range = 4;
inline constexpr std::size_t attack = 5;
inline constexpr std::size_t release = 6;
inline constexpr std::size_t makeup = 7;
inline constexpr std::size_t process = 8;
inline constexpr std::size_t key = 9;
inline constexpr std::size_t solo = 10;
inline constexpr std::size_t mute = 11;
inline constexpr std::size_t bypass = 12;
inline constexpr std::size_t size = 13;
} // namespace band

inline constexpr std::size_t global_count = 18;
inline constexpr std::size_t band_count = 6;
inline constexpr std::size_t first_band = global_count;

inline constexpr std::array<synth::ParameterDescriptor, 96> parameters{{
    {"strata.input", "Input", "Global", "dB", S::linear, -24, 24, 0, {}, T::live, g},
    {"strata.output", "Output", "Global", "dB", S::linear, -24, 24, 0, {}, T::live, g},
    {"strata.mix", "Mix", "Global", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"strata.auto_gain", "Auto Gain", "Global", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"strata.delta", "Delta", "Global", "", S::choice, 0, 1, 0, off_on, T::live, 0, false},
    {"strata.lookahead", "Lookahead", "Global", "ms", S::linear, 0, lookahead_max_ms, 0, {}, T::live, 0, false},
    {"strata.phase", "Phase", "Global", "", S::choice, 0, last(phases.size()), Phase::minimum, phases, T::live, 0, false},
    {"strata.mode", "Mode", "Global", "", S::choice, 0, last(modes.size()), Mode::bands_mode, modes, T::live, 0, false},
    {"strata.bands", "Bands", "Bands", "", S::choice, 0, last(band_counts.size()), bands_default, band_counts, T::live, 0, false},
    {"strata.xover_1", "Crossover 1", "Bands", "Hz", S::logarithmic, crossover_min, crossover_max, crossover_defaults[0], {}, T::live, g},
    {"strata.xover_2", "Crossover 2", "Bands", "Hz", S::logarithmic, crossover_min, crossover_max, crossover_defaults[1], {}, T::live, g},
    {"strata.xover_3", "Crossover 3", "Bands", "Hz", S::logarithmic, crossover_min, crossover_max, crossover_defaults[2], {}, T::live, g},
    {"strata.xover_4", "Crossover 4", "Bands", "Hz", S::logarithmic, crossover_min, crossover_max, crossover_defaults[3], {}, T::live, g},
    {"strata.xover_5", "Crossover 5", "Bands", "Hz", S::logarithmic, crossover_min, crossover_max, crossover_defaults[4], {}, T::live, g},
    {"strata.ott.depth", "Depth", "OTT", "%", S::linear, 0, 1, ott_depth_default, {}, T::live, g},
    {"strata.ott.time", "Time", "OTT", "%", S::linear, 0, 1, ott_time_default, {}, T::live, g},
    {"strata.ott.balance", "Up / Down", "OTT", "", S::linear, -1, 1, 0, {}, T::live, g},
    {"strata.learn", "Learn", "Global", "", S::choice, 0, 1, 0, learn_states, T::live, 0, false},
    {"strata.band_1.threshold", "Threshold", "Band 1", "dB", S::linear, threshold_min, threshold_max, threshold_default, {}, T::live, g},
    {"strata.band_1.above", "Above", "Band 1", "", S::logarithmic, ratio_min, ratio_max, above_default, {}, T::live, g},
    {"strata.band_1.below", "Below", "Band 1", "", S::logarithmic, below_min, ratio_max, below_default, {}, T::live, g},
    {"strata.band_1.knee", "Knee", "Band 1", "dB", S::linear, 0, knee_max, knee_default, {}, T::live, g},
    {"strata.band_1.range", "Range", "Band 1", "dB", S::linear, 0, range_max, range_default, {}, T::live, g},
    {"strata.band_1.attack", "Attack", "Band 1", "ms", S::logarithmic, attack_min, attack_max, attack_default, {}, T::live, g},
    {"strata.band_1.release", "Release", "Band 1", "ms", S::logarithmic, release_min, release_max, release_default, {}, T::live, g},
    {"strata.band_1.makeup", "Makeup", "Band 1", "dB", S::linear, -makeup_limit, makeup_limit, 0, {}, T::live, g},
    {"strata.band_1.process", "Process", "Band 1", "", S::choice, 0, last(processes.size()), Process::stereo, processes, T::live, 0, false},
    {"strata.band_1.key", "Key", "Band 1", "", S::choice, 0, last(keys.size()), Key::band_key, keys, T::live, 0, false},
    {"strata.band_1.solo", "Solo", "Band 1", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"strata.band_1.mute", "Mute", "Band 1", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"strata.band_1.bypass", "Bypass", "Band 1", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"strata.band_2.threshold", "Threshold", "Band 2", "dB", S::linear, threshold_min, threshold_max, threshold_default, {}, T::live, g},
    {"strata.band_2.above", "Above", "Band 2", "", S::logarithmic, ratio_min, ratio_max, above_default, {}, T::live, g},
    {"strata.band_2.below", "Below", "Band 2", "", S::logarithmic, below_min, ratio_max, below_default, {}, T::live, g},
    {"strata.band_2.knee", "Knee", "Band 2", "dB", S::linear, 0, knee_max, knee_default, {}, T::live, g},
    {"strata.band_2.range", "Range", "Band 2", "dB", S::linear, 0, range_max, range_default, {}, T::live, g},
    {"strata.band_2.attack", "Attack", "Band 2", "ms", S::logarithmic, attack_min, attack_max, attack_default, {}, T::live, g},
    {"strata.band_2.release", "Release", "Band 2", "ms", S::logarithmic, release_min, release_max, release_default, {}, T::live, g},
    {"strata.band_2.makeup", "Makeup", "Band 2", "dB", S::linear, -makeup_limit, makeup_limit, 0, {}, T::live, g},
    {"strata.band_2.process", "Process", "Band 2", "", S::choice, 0, last(processes.size()), Process::stereo, processes, T::live, 0, false},
    {"strata.band_2.key", "Key", "Band 2", "", S::choice, 0, last(keys.size()), Key::band_key, keys, T::live, 0, false},
    {"strata.band_2.solo", "Solo", "Band 2", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"strata.band_2.mute", "Mute", "Band 2", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"strata.band_2.bypass", "Bypass", "Band 2", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"strata.band_3.threshold", "Threshold", "Band 3", "dB", S::linear, threshold_min, threshold_max, threshold_default, {}, T::live, g},
    {"strata.band_3.above", "Above", "Band 3", "", S::logarithmic, ratio_min, ratio_max, above_default, {}, T::live, g},
    {"strata.band_3.below", "Below", "Band 3", "", S::logarithmic, below_min, ratio_max, below_default, {}, T::live, g},
    {"strata.band_3.knee", "Knee", "Band 3", "dB", S::linear, 0, knee_max, knee_default, {}, T::live, g},
    {"strata.band_3.range", "Range", "Band 3", "dB", S::linear, 0, range_max, range_default, {}, T::live, g},
    {"strata.band_3.attack", "Attack", "Band 3", "ms", S::logarithmic, attack_min, attack_max, attack_default, {}, T::live, g},
    {"strata.band_3.release", "Release", "Band 3", "ms", S::logarithmic, release_min, release_max, release_default, {}, T::live, g},
    {"strata.band_3.makeup", "Makeup", "Band 3", "dB", S::linear, -makeup_limit, makeup_limit, 0, {}, T::live, g},
    {"strata.band_3.process", "Process", "Band 3", "", S::choice, 0, last(processes.size()), Process::stereo, processes, T::live, 0, false},
    {"strata.band_3.key", "Key", "Band 3", "", S::choice, 0, last(keys.size()), Key::band_key, keys, T::live, 0, false},
    {"strata.band_3.solo", "Solo", "Band 3", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"strata.band_3.mute", "Mute", "Band 3", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"strata.band_3.bypass", "Bypass", "Band 3", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"strata.band_4.threshold", "Threshold", "Band 4", "dB", S::linear, threshold_min, threshold_max, threshold_default, {}, T::live, g},
    {"strata.band_4.above", "Above", "Band 4", "", S::logarithmic, ratio_min, ratio_max, above_default, {}, T::live, g},
    {"strata.band_4.below", "Below", "Band 4", "", S::logarithmic, below_min, ratio_max, below_default, {}, T::live, g},
    {"strata.band_4.knee", "Knee", "Band 4", "dB", S::linear, 0, knee_max, knee_default, {}, T::live, g},
    {"strata.band_4.range", "Range", "Band 4", "dB", S::linear, 0, range_max, range_default, {}, T::live, g},
    {"strata.band_4.attack", "Attack", "Band 4", "ms", S::logarithmic, attack_min, attack_max, attack_default, {}, T::live, g},
    {"strata.band_4.release", "Release", "Band 4", "ms", S::logarithmic, release_min, release_max, release_default, {}, T::live, g},
    {"strata.band_4.makeup", "Makeup", "Band 4", "dB", S::linear, -makeup_limit, makeup_limit, 0, {}, T::live, g},
    {"strata.band_4.process", "Process", "Band 4", "", S::choice, 0, last(processes.size()), Process::stereo, processes, T::live, 0, false},
    {"strata.band_4.key", "Key", "Band 4", "", S::choice, 0, last(keys.size()), Key::band_key, keys, T::live, 0, false},
    {"strata.band_4.solo", "Solo", "Band 4", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"strata.band_4.mute", "Mute", "Band 4", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"strata.band_4.bypass", "Bypass", "Band 4", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"strata.band_5.threshold", "Threshold", "Band 5", "dB", S::linear, threshold_min, threshold_max, threshold_default, {}, T::live, g},
    {"strata.band_5.above", "Above", "Band 5", "", S::logarithmic, ratio_min, ratio_max, above_default, {}, T::live, g},
    {"strata.band_5.below", "Below", "Band 5", "", S::logarithmic, below_min, ratio_max, below_default, {}, T::live, g},
    {"strata.band_5.knee", "Knee", "Band 5", "dB", S::linear, 0, knee_max, knee_default, {}, T::live, g},
    {"strata.band_5.range", "Range", "Band 5", "dB", S::linear, 0, range_max, range_default, {}, T::live, g},
    {"strata.band_5.attack", "Attack", "Band 5", "ms", S::logarithmic, attack_min, attack_max, attack_default, {}, T::live, g},
    {"strata.band_5.release", "Release", "Band 5", "ms", S::logarithmic, release_min, release_max, release_default, {}, T::live, g},
    {"strata.band_5.makeup", "Makeup", "Band 5", "dB", S::linear, -makeup_limit, makeup_limit, 0, {}, T::live, g},
    {"strata.band_5.process", "Process", "Band 5", "", S::choice, 0, last(processes.size()), Process::stereo, processes, T::live, 0, false},
    {"strata.band_5.key", "Key", "Band 5", "", S::choice, 0, last(keys.size()), Key::band_key, keys, T::live, 0, false},
    {"strata.band_5.solo", "Solo", "Band 5", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"strata.band_5.mute", "Mute", "Band 5", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"strata.band_5.bypass", "Bypass", "Band 5", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"strata.band_6.threshold", "Threshold", "Band 6", "dB", S::linear, threshold_min, threshold_max, threshold_default, {}, T::live, g},
    {"strata.band_6.above", "Above", "Band 6", "", S::logarithmic, ratio_min, ratio_max, above_default, {}, T::live, g},
    {"strata.band_6.below", "Below", "Band 6", "", S::logarithmic, below_min, ratio_max, below_default, {}, T::live, g},
    {"strata.band_6.knee", "Knee", "Band 6", "dB", S::linear, 0, knee_max, knee_default, {}, T::live, g},
    {"strata.band_6.range", "Range", "Band 6", "dB", S::linear, 0, range_max, range_default, {}, T::live, g},
    {"strata.band_6.attack", "Attack", "Band 6", "ms", S::logarithmic, attack_min, attack_max, attack_default, {}, T::live, g},
    {"strata.band_6.release", "Release", "Band 6", "ms", S::logarithmic, release_min, release_max, release_default, {}, T::live, g},
    {"strata.band_6.makeup", "Makeup", "Band 6", "dB", S::linear, -makeup_limit, makeup_limit, 0, {}, T::live, g},
    {"strata.band_6.process", "Process", "Band 6", "", S::choice, 0, last(processes.size()), Process::stereo, processes, T::live, 0, false},
    {"strata.band_6.key", "Key", "Band 6", "", S::choice, 0, last(keys.size()), Key::band_key, keys, T::live, 0, false},
    {"strata.band_6.solo", "Solo", "Band 6", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"strata.band_6.mute", "Mute", "Band 6", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"strata.band_6.bypass", "Bypass", "Band 6", "", S::choice, 0, 1, 0, off_on, T::live, 0},
}};

static_assert(parameters.size() == global_count + band_count * band::size);

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

// Band `b`'s (from 0) parameter `field` (strata_table::band).
[[nodiscard]] constexpr std::size_t band_index(std::size_t b, std::size_t field) noexcept {
  return first_band + b * band::size + field;
}

} // namespace oscillate::native_dsp::effects::strata_table
// clang-format on
