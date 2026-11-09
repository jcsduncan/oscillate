#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Temper v1 (ADR-0064): a resonance suppressor with dynamic EQ bands. The
// suppressor's controls, a six-node sensitivity curve that says where it
// listens, and eight dynamic bands (off by default). Phase sets the latency,
// so it is not automatable: a change rebuilds the route. Ids are persisted
// and automated: never rename or reuse one.
namespace oscillate::native_dsp::effects::temper_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array off_on{"Off"sv, "On"sv};
// Soft compares each band with the power mean of its neighbourhood (an
// adaptive threshold: only what truly stands out is cut); Hard with the
// mean level, so it cuts more, and harder.
inline constexpr std::array modes{"Soft"sv, "Hard"sv};
enum Mode : std::uint8_t { soft, hard };
inline constexpr std::array phases{"Low Latency"sv, "Natural"sv, "Linear"sv};
enum Phase : std::uint8_t { low_latency, natural, linear };
// What is processed; the rest passes untouched (delayed by the latency).
inline constexpr std::array stereo_modes{"Stereo"sv, "Mid"sv, "Side"sv, "Left"sv, "Right"sv};
enum StereoMode : std::uint8_t { stereo, mid, side, left, right };
inline constexpr std::array kinds{"Bell"sv, "Low Shelf"sv, "High Shelf"sv};
enum Kind : std::uint8_t { bell, low_shelf, high_shelf };

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = effect_glide_seconds;

inline constexpr std::size_t sensitivity_nodes = 6;
inline constexpr std::size_t bands = 8;
inline constexpr std::size_t per_node = 2;
inline constexpr std::size_t per_band = 7;
inline constexpr std::size_t globals = 19;

inline constexpr std::array<ParameterDescriptor,
                            globals + sensitivity_nodes * per_node + bands * per_band>
    parameters{{
    {"temper.depth", "Depth", "Suppress", "", S::linear, 0, 20, 5, {}, T::live, g},
    {"temper.detail", "Detail", "Suppress", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    {"temper.mode", "Mode", "Suppress", "", S::choice, 0, last(modes.size()), Mode::soft, modes,
     T::live, 0},
    {"temper.max_cut", "Max Cut", "Suppress", "dB", S::linear, 1, 24, 12, {}, T::live, g},
    {"temper.tilt", "Tilt", "Suppress", "%", S::linear, -1, 1, 0, {}, T::live, g},
    {"temper.attack", "Attack", "Time", "ms", S::logarithmic, 0.5, 50, 5, {}, T::live, g},
    {"temper.release", "Release", "Time", "ms", S::logarithmic, 5, 500, 80, {}, T::live, g},
    {"temper.attacks", "Attacks", "Split", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"temper.sustains", "Sustains", "Split", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"temper.separate", "Separate", "Split", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    {"temper.stereo", "Channels", "Stereo", "", S::choice, 0, last(stereo_modes.size()),
     StereoMode::stereo, stereo_modes, T::live, 0},
    {"temper.link", "Link", "Stereo", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"temper.phase", "Phase", "Quality", "", S::choice, 0, last(phases.size()), Phase::natural,
     phases, T::live, 0, false},
    {"temper.unmask", "Unmask", "Unmask", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"temper.unmask_amount", "Amount", "Unmask", "%", S::linear, 0, 1, 0.6, {}, T::live, g},
    {"temper.unmask_focus", "Focus", "Unmask", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    {"temper.mix", "Mix", "Output", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"temper.trim", "Trim", "Output", "dB", S::linear, -12, 12, 0, {}, T::live, g},
    {"temper.delta", "Delta", "Output", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"temper.sens_freq_1", "Frequency", "Listen 1", "Hz", S::logarithmic, 20, 20'000, 60, {}, T::live, g},
    {"temper.sens_gain_1", "Sensitivity", "Listen 1", "dB", S::linear, -12, 12, 0, {}, T::live, g},
    {"temper.sens_freq_2", "Frequency", "Listen 2", "Hz", S::logarithmic, 20, 20'000, 250, {}, T::live, g},
    {"temper.sens_gain_2", "Sensitivity", "Listen 2", "dB", S::linear, -12, 12, 0, {}, T::live, g},
    {"temper.sens_freq_3", "Frequency", "Listen 3", "Hz", S::logarithmic, 20, 20'000, 1000, {}, T::live, g},
    {"temper.sens_gain_3", "Sensitivity", "Listen 3", "dB", S::linear, -12, 12, 0, {}, T::live, g},
    {"temper.sens_freq_4", "Frequency", "Listen 4", "Hz", S::logarithmic, 20, 20'000, 3200, {}, T::live, g},
    {"temper.sens_gain_4", "Sensitivity", "Listen 4", "dB", S::linear, -12, 12, 0, {}, T::live, g},
    {"temper.sens_freq_5", "Frequency", "Listen 5", "Hz", S::logarithmic, 20, 20'000, 7500, {}, T::live, g},
    {"temper.sens_gain_5", "Sensitivity", "Listen 5", "dB", S::linear, -12, 12, 0, {}, T::live, g},
    {"temper.sens_freq_6", "Frequency", "Listen 6", "Hz", S::logarithmic, 20, 20'000, 15000, {}, T::live, g},
    {"temper.sens_gain_6", "Sensitivity", "Listen 6", "dB", S::linear, -12, 12, 0, {}, T::live, g},
    {"temper.band_on_1", "On", "Band 1", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"temper.band_kind_1", "Kind", "Band 1", "", S::choice, 0, last(kinds.size()), Kind::bell, kinds, T::live, 0},
    {"temper.band_freq_1", "Frequency", "Band 1", "Hz", S::logarithmic, 20, 20'000, 80, {}, T::live, g},
    {"temper.band_q_1", "Q", "Band 1", "", S::logarithmic, 0.1, 18, 1, {}, T::live, g},
    {"temper.band_range_1", "Range", "Band 1", "dB", S::linear, -24, 24, -6, {}, T::live, g},
    {"temper.band_threshold_1", "Threshold", "Band 1", "dB", S::linear, -80, 0, -30, {}, T::live, g},
    {"temper.band_ratio_1", "Ratio", "Band 1", "", S::logarithmic, 1, 20, 3, {}, T::live, g},
    {"temper.band_on_2", "On", "Band 2", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"temper.band_kind_2", "Kind", "Band 2", "", S::choice, 0, last(kinds.size()), Kind::bell, kinds, T::live, 0},
    {"temper.band_freq_2", "Frequency", "Band 2", "Hz", S::logarithmic, 20, 20'000, 200, {}, T::live, g},
    {"temper.band_q_2", "Q", "Band 2", "", S::logarithmic, 0.1, 18, 1, {}, T::live, g},
    {"temper.band_range_2", "Range", "Band 2", "dB", S::linear, -24, 24, -6, {}, T::live, g},
    {"temper.band_threshold_2", "Threshold", "Band 2", "dB", S::linear, -80, 0, -30, {}, T::live, g},
    {"temper.band_ratio_2", "Ratio", "Band 2", "", S::logarithmic, 1, 20, 3, {}, T::live, g},
    {"temper.band_on_3", "On", "Band 3", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"temper.band_kind_3", "Kind", "Band 3", "", S::choice, 0, last(kinds.size()), Kind::bell, kinds, T::live, 0},
    {"temper.band_freq_3", "Frequency", "Band 3", "Hz", S::logarithmic, 20, 20'000, 500, {}, T::live, g},
    {"temper.band_q_3", "Q", "Band 3", "", S::logarithmic, 0.1, 18, 1, {}, T::live, g},
    {"temper.band_range_3", "Range", "Band 3", "dB", S::linear, -24, 24, -6, {}, T::live, g},
    {"temper.band_threshold_3", "Threshold", "Band 3", "dB", S::linear, -80, 0, -30, {}, T::live, g},
    {"temper.band_ratio_3", "Ratio", "Band 3", "", S::logarithmic, 1, 20, 3, {}, T::live, g},
    {"temper.band_on_4", "On", "Band 4", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"temper.band_kind_4", "Kind", "Band 4", "", S::choice, 0, last(kinds.size()), Kind::bell, kinds, T::live, 0},
    {"temper.band_freq_4", "Frequency", "Band 4", "Hz", S::logarithmic, 20, 20'000, 1000, {}, T::live, g},
    {"temper.band_q_4", "Q", "Band 4", "", S::logarithmic, 0.1, 18, 1, {}, T::live, g},
    {"temper.band_range_4", "Range", "Band 4", "dB", S::linear, -24, 24, -6, {}, T::live, g},
    {"temper.band_threshold_4", "Threshold", "Band 4", "dB", S::linear, -80, 0, -30, {}, T::live, g},
    {"temper.band_ratio_4", "Ratio", "Band 4", "", S::logarithmic, 1, 20, 3, {}, T::live, g},
    {"temper.band_on_5", "On", "Band 5", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"temper.band_kind_5", "Kind", "Band 5", "", S::choice, 0, last(kinds.size()), Kind::bell, kinds, T::live, 0},
    {"temper.band_freq_5", "Frequency", "Band 5", "Hz", S::logarithmic, 20, 20'000, 2500, {}, T::live, g},
    {"temper.band_q_5", "Q", "Band 5", "", S::logarithmic, 0.1, 18, 1, {}, T::live, g},
    {"temper.band_range_5", "Range", "Band 5", "dB", S::linear, -24, 24, -6, {}, T::live, g},
    {"temper.band_threshold_5", "Threshold", "Band 5", "dB", S::linear, -80, 0, -30, {}, T::live, g},
    {"temper.band_ratio_5", "Ratio", "Band 5", "", S::logarithmic, 1, 20, 3, {}, T::live, g},
    {"temper.band_on_6", "On", "Band 6", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"temper.band_kind_6", "Kind", "Band 6", "", S::choice, 0, last(kinds.size()), Kind::bell, kinds, T::live, 0},
    {"temper.band_freq_6", "Frequency", "Band 6", "Hz", S::logarithmic, 20, 20'000, 5000, {}, T::live, g},
    {"temper.band_q_6", "Q", "Band 6", "", S::logarithmic, 0.1, 18, 1, {}, T::live, g},
    {"temper.band_range_6", "Range", "Band 6", "dB", S::linear, -24, 24, -6, {}, T::live, g},
    {"temper.band_threshold_6", "Threshold", "Band 6", "dB", S::linear, -80, 0, -30, {}, T::live, g},
    {"temper.band_ratio_6", "Ratio", "Band 6", "", S::logarithmic, 1, 20, 3, {}, T::live, g},
    {"temper.band_on_7", "On", "Band 7", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"temper.band_kind_7", "Kind", "Band 7", "", S::choice, 0, last(kinds.size()), Kind::bell, kinds, T::live, 0},
    {"temper.band_freq_7", "Frequency", "Band 7", "Hz", S::logarithmic, 20, 20'000, 8000, {}, T::live, g},
    {"temper.band_q_7", "Q", "Band 7", "", S::logarithmic, 0.1, 18, 1, {}, T::live, g},
    {"temper.band_range_7", "Range", "Band 7", "dB", S::linear, -24, 24, -6, {}, T::live, g},
    {"temper.band_threshold_7", "Threshold", "Band 7", "dB", S::linear, -80, 0, -30, {}, T::live, g},
    {"temper.band_ratio_7", "Ratio", "Band 7", "", S::logarithmic, 1, 20, 3, {}, T::live, g},
    {"temper.band_on_8", "On", "Band 8", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"temper.band_kind_8", "Kind", "Band 8", "", S::choice, 0, last(kinds.size()), Kind::bell, kinds, T::live, 0},
    {"temper.band_freq_8", "Frequency", "Band 8", "Hz", S::logarithmic, 20, 20'000, 12000, {}, T::live, g},
    {"temper.band_q_8", "Q", "Band 8", "", S::logarithmic, 0.1, 18, 1, {}, T::live, g},
    {"temper.band_range_8", "Range", "Band 8", "dB", S::linear, -24, 24, -6, {}, T::live, g},
    {"temper.band_threshold_8", "Threshold", "Band 8", "dB", S::linear, -80, 0, -30, {}, T::live, g},
    {"temper.band_ratio_8", "Ratio", "Band 8", "", S::logarithmic, 1, 20, 3, {}, T::live, g},
}};

consteval std::size_t index_of(std::string_view id) {
  return parameter_index(parameters, id);
}

// Node n (from 0) is frequency then sensitivity, from sensitivity_base +
// n * per_node; band b (from 0) is the seven parameters from band_base +
// b * per_band, in this order.
inline constexpr std::size_t sensitivity_base = index_of("temper.sens_freq_1");
inline constexpr std::size_t node_freq = 0;
inline constexpr std::size_t node_gain = 1;
inline constexpr std::size_t band_base = index_of("temper.band_on_1");
inline constexpr std::size_t on = 0;
inline constexpr std::size_t kind = 1;
inline constexpr std::size_t freq = 2;
inline constexpr std::size_t q = 3;
inline constexpr std::size_t range = 4;
inline constexpr std::size_t threshold = 5;
inline constexpr std::size_t ratio = 6;
static_assert(sensitivity_base == globals &&
              index_of("temper.sens_gain_6") == sensitivity_base + 5 * per_node + node_gain &&
              band_base == sensitivity_base + sensitivity_nodes * per_node &&
              index_of("temper.band_ratio_8") == band_base + 7 * per_band + ratio);

} // namespace oscillate::native_dsp::effects::temper_table
// clang-format on
