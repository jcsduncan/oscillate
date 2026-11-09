#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Hush v1 (ADR-0116): a gate and downward expander. Lookahead sets the
// latency, so it is a choice and not automatable: a change rebuilds the
// route. Range at its minimum reads minus infinity (a full mute). Ids are
// persisted and automated: never rename or reuse one.
namespace oscillate::native_dsp::effects::hush_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

// Gate: closed means the range. Expander: below the threshold the level
// falls `ratio` times faster than the key, down to the range.
inline constexpr std::array modes{"Gate"sv, "Expander"sv};
enum Mode : std::uint8_t { gate, expander };
// Sidechain keys from the routed sidechain, or from the input while none
// arrives; Input always keys from the input.
inline constexpr std::array keys{"Input"sv, "Sidechain"sv};
enum Key : std::uint8_t { input, sidechain };
inline constexpr std::array lookaheads{"Off"sv,  "0.5 ms"sv, "1 ms"sv,
                                       "2 ms"sv, "5 ms"sv,   "10 ms"sv};
inline constexpr std::array<double, 6> lookahead_ms{0, 0.5, 1, 2, 5, 10};
inline constexpr std::array off_on{"Off"sv, "On"sv};

// The range's minimum: a full mute.
inline constexpr double mute_range_db = -80.0;
inline constexpr double lowest_threshold_db = -80.0;
// The key filter's ends: at these corners it passes the whole band.
inline constexpr double key_low_hz = 20.0;
inline constexpr double key_high_hz = 20'000.0;

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = effect_glide_seconds;
inline constexpr double hold_skew = 2.0;

inline constexpr std::array<ParameterDescriptor, 15> parameters{{
    {"hush.threshold",
     "Threshold",
     "Gate",
     "dB",
     S::linear,
     lowest_threshold_db,
     0,
     -40,
     {},
     T::live,
     g},
    {"hush.hysteresis", "Hysteresis", "Gate", "dB", S::linear, 0, 20, 6, {}, T::live, g},
    {"hush.range",
     "Range",
     "Gate",
     "dB",
     S::linear,
     mute_range_db,
     0,
     mute_range_db,
     {},
     T::live,
     g},
    {"hush.mode", "Mode", "Gate", "", S::choice, 0, last(modes.size()), Mode::gate, modes, T::live,
     0},
    {"hush.ratio", "Ratio", "Gate", "", S::logarithmic, 1.1, 10, 2, {}, T::live, g},
    {"hush.attack", "Attack", "Timing", "ms", S::logarithmic, 0.1, 100, 0.5, {}, T::live, g},
    {"hush.hold", "Hold", "Timing", "ms", S::linear, 0, 500, 30, {}, T::live, g, true, hold_skew},
    {"hush.release", "Release", "Timing", "ms", S::logarithmic, 1, 4'000, 120, {}, T::live, g},
    {"hush.lookahead", "Lookahead", "Timing", "", S::choice, 0, last(lookaheads.size()), 0,
     lookaheads, T::live, 0, false},
    {"hush.key", "Key", "Key", "", S::choice, 0, last(keys.size()), Key::sidechain, keys, T::live,
     0},
    {"hush.key_hp",
     "Key HP",
     "Key",
     "Hz",
     S::logarithmic,
     key_low_hz,
     key_high_hz,
     key_low_hz,
     {},
     T::live,
     g},
    {"hush.key_lp",
     "Key LP",
     "Key",
     "Hz",
     S::logarithmic,
     key_low_hz,
     key_high_hz,
     key_high_hz,
     {},
     T::live,
     g},
    {"hush.listen", "Listen", "Key", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"hush.link", "Stereo Link", "Gate", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"hush.flip", "Flip", "Gate", "", S::choice, 0, 1, 0, off_on, T::live, 0},
}};

consteval std::size_t index_of(std::string_view id) { return parameter_index(parameters, id); }

} // namespace oscillate::native_dsp::effects::hush_table
