#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Cast v1: a convolution reverb (ADR-0115). The IR group shapes the impulse
// response itself: the engine compiles it with its document off the callback
// (EffectEngine::document_parameters), so those parameters are not
// automatable. Everything after the IR is live. Ids are persisted and
// automated: never rename or reuse one.
namespace oscillate::native_dsp::effects::cast_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array off_on{"Off"sv, "On"sv};
inline constexpr std::array keys{"Input"sv, "Sidechain"sv};
enum Key : std::uint8_t { input, sidechain };

inline constexpr double g = effect_glide_seconds;
// Pre-delay glides slowly, so a move bends the pitch of the tail no more
// than a tape delay's time knob would.
inline constexpr double predelay_glide = 0.2;
// Low Cut at its minimum and High Cut at its maximum are off.
inline constexpr double low_cut_off_hz = 20.0;
inline constexpr double high_cut_off_hz = 20'000.0;

inline constexpr std::array<ParameterDescriptor, 18> parameters{{
    // The IR (compiled with the document).
    {"cast.start", "Start", "IR", "ms", S::linear, 0, 500, 0, {}, T::live, 0, false, 2},
    {"cast.length", "Length", "IR", "%", S::linear, 0.05, 1, 1, {}, T::live, 0, false},
    {"cast.stretch", "Stretch", "IR", "%", S::logarithmic, 0.5, 2, 1, {}, T::live, 0, false},
    {"cast.decay", "Decay", "IR", "%", S::linear, 0.1, 1, 1, {}, T::live, 0, false},
    {"cast.fade_in", "Fade In", "IR", "ms", S::linear, 0, 500, 0, {}, T::live, 0, false, 2},
    {"cast.reverse", "Reverse", "IR", "", S::choice, 0, 1, 0, off_on, T::live, 0, false},
    {"cast.normalise", "Normalise", "IR", "", S::choice, 0, 1, 1, off_on, T::live, 0, false},
    // The wet path, live.
    {"cast.predelay",
     "Pre-Delay",
     "Space",
     "ms",
     S::linear,
     0,
     250,
     0,
     {},
     T::live,
     predelay_glide,
     true,
     2},
    {"cast.width", "Width", "Space", "%", S::linear, 0, 2, 1, {}, T::live, g},
    {"cast.low_cut",
     "Low Cut",
     "Tone",
     "Hz",
     S::logarithmic,
     low_cut_off_hz,
     2'000,
     low_cut_off_hz,
     {},
     T::live,
     g},
    {"cast.high_cut",
     "High Cut",
     "Tone",
     "Hz",
     S::logarithmic,
     1'000,
     high_cut_off_hz,
     high_cut_off_hz,
     {},
     T::live,
     g},
    {"cast.tilt", "Tilt", "Tone", "dB", S::linear, -6, 6, 0, {}, T::live, g},
    {"cast.duck", "Duck", "Duck", "dB", S::linear, 0, 30, 0, {}, T::live, g},
    {"cast.duck_threshold", "Threshold", "Duck", "dB", S::linear, -60, 0, -30, {}, T::live, g},
    {"cast.duck_release", "Release", "Duck", "ms", S::logarithmic, 20, 2'000, 250, {}, T::live, g},
    {"cast.duck_key", "Key", "Duck", "", S::choice, 0, 1, Key::input, keys, T::live, 0, false},
    {"cast.gain", "Wet Gain", "Mix", "dB", S::linear, -24, 12, 0, {}, T::live, g},
    {"cast.mix", "Mix", "Mix", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
}};

consteval std::size_t index_of(std::string_view id) { return parameter_index(parameters, id); }

// The IR group, compiled with the document.
inline constexpr std::array<std::size_t, 7> document_parameters{
    index_of("cast.start"),    index_of("cast.length"),  index_of("cast.stretch"),
    index_of("cast.decay"),    index_of("cast.fade_in"), index_of("cast.reverse"),
    index_of("cast.normalise")};

} // namespace oscillate::native_dsp::effects::cast_table
