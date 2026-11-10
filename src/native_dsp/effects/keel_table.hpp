#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Keel v1: adaptive tonal balance (ADR-0119). The drawn curve is 31
// values, one per third-octave band of keel::band_hz; Learn and genre
// targets write them too. The target source, the switches and the curve
// change no latency (Keel has none) but are not automatable.
// Ids are persisted and automated: never rename or reuse one.
namespace oscillate::native_dsp::effects::keel_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array off_on{"Off"sv, "On"sv};
// Tilt: the target is the tilt alone (flat at 0 dB/oct). Curve: the drawn,
// learned or genre curve, tilted. Reference: the sidechain's own tracked
// balance, tilted.
inline constexpr std::array targets{"Tilt"sv, "Curve"sv, "Reference"sv};
enum Target : std::uint8_t { tilt, curve, reference };

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = effect_glide_seconds;

inline constexpr double speed_min = 0.5;
inline constexpr double speed_max = 30.0;
inline constexpr double speed_default = 4.0;
inline constexpr double limit_max = 18.0;
inline constexpr double max_boost_default = 4.0;
inline constexpr double max_cut_default = 4.0;
inline constexpr double tilt_limit = 6.0;
// Masters fall against pink noise: the eight-track corpus averages
// -2.7 dB/oct from 63 Hz to 12.5 kHz (-1.9 to -4.6).
inline constexpr double tilt_default = -2.5;
inline constexpr double low_split_min = 60.0;
inline constexpr double low_split_max = 600.0;
inline constexpr double low_split_default = 200.0;
inline constexpr double high_split_min = 1'500.0;
inline constexpr double high_split_max = 10'000.0;
inline constexpr double high_split_default = 4'000.0;
inline constexpr double output_limit = 12.0;
inline constexpr double curve_limit = 24.0;
inline constexpr double amount_default = 0.5;

inline constexpr std::size_t global_count = 14;
inline constexpr std::size_t curve_count = 31;
inline constexpr std::size_t first_curve = global_count;

inline constexpr std::array<ParameterDescriptor, global_count + curve_count> parameters{{
    {"keel.amount", "Amount", "Balance", "%", S::linear, 0, 1, amount_default, {}, T::live, g},
    {"keel.speed", "Speed", "Balance", "s", S::logarithmic, speed_min, speed_max, speed_default, {}, T::live, g},
    {"keel.max_boost", "Max Boost", "Balance", "dB", S::linear, 0, limit_max, max_boost_default, {}, T::live, g},
    {"keel.max_cut", "Max Cut", "Balance", "dB", S::linear, 0, limit_max, max_cut_default, {}, T::live, g},
    {"keel.target", "Target", "Target", "", S::choice, 0, last(targets.size()), Target::tilt, targets, T::live, 0, false},
    {"keel.tilt", "Tilt", "Target", "dB/oct", S::linear, -tilt_limit, tilt_limit, tilt_default, {}, T::live, g},
    {"keel.low", "Low", "Regions", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"keel.mid", "Mid", "Regions", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"keel.high", "High", "Regions", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"keel.low_split", "Low Split", "Regions", "Hz", S::logarithmic, low_split_min, low_split_max, low_split_default, {}, T::live, g},
    {"keel.high_split", "High Split", "Regions", "Hz", S::logarithmic, high_split_min, high_split_max, high_split_default, {}, T::live, g},
    {"keel.match", "Level Match", "Output", "", S::choice, 0, 1, 1, off_on, T::live, 0, false},
    {"keel.output", "Output", "Output", "dB", S::linear, -output_limit, output_limit, 0, {}, T::live, g},
    {"keel.delta", "Delta", "Output", "", S::choice, 0, 1, 0, off_on, T::live, 0, false},
    {"keel.curve_01", "20 Hz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_02", "25 Hz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_03", "31.5 Hz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_04", "40 Hz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_05", "50 Hz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_06", "63 Hz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_07", "80 Hz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_08", "100 Hz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_09", "125 Hz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_10", "160 Hz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_11", "200 Hz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_12", "250 Hz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_13", "315 Hz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_14", "400 Hz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_15", "500 Hz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_16", "630 Hz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_17", "800 Hz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_18", "1 kHz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_19", "1.25 kHz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_20", "1.6 kHz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_21", "2 kHz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_22", "2.5 kHz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_23", "3.15 kHz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_24", "4 kHz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_25", "5 kHz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_26", "6.3 kHz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_27", "8 kHz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_28", "10 kHz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_29", "12.5 kHz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_30", "16 kHz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
    {"keel.curve_31", "20 kHz", "Curve", "dB", S::linear, -curve_limit, curve_limit, 0, {}, T::live, 0, false},
}};

consteval std::size_t index_of(std::string_view id) { return parameter_index(parameters, id); }

} // namespace oscillate::native_dsp::effects::keel_table

// clang-format on
