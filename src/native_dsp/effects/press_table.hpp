#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <cstddef>
#include <string_view>

// Press v1: the classic native compressor as an engine (ADR-0047), with its
// ranges and defaults. The classic detector high-pass is 0 (off) or 20 Hz
// to 2 kHz: here a switch and a frequency. Ids are persisted and automated:
// never rename or reuse one.
namespace oscillate::native_dsp::effects::press_table {

using S = synth::ParameterScale;
using T = synth::ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array off_on{"Off"sv, "On"sv};

inline constexpr double g = synth::effect_glide_seconds;

inline constexpr std::array<synth::ParameterDescriptor, 8> parameters{{
    {"press.threshold", "Threshold", "Dynamics", "dB", S::linear, -80, 0, -18, {}, T::live, g},
    {"press.ratio", "Ratio", "Dynamics", "", S::logarithmic, 1, 20, 4, {}, T::live, g},
    {"press.knee", "Knee", "Dynamics", "dB", S::linear, 0, 24, 6, {}, T::live, g},
    {"press.attack", "Attack", "Timing", "ms", S::logarithmic, 0.1, 200, 10, {}, T::live, g},
    {"press.release", "Release", "Timing", "ms", S::logarithmic, 1, 2'000, 100, {}, T::live, g},
    {"press.makeup", "Makeup", "Output", "dB", S::linear, -24, 24, 0, {}, T::live, g},
    {"press.detector_hp", "Detector HP", "Detector", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"press.detector_freq",
     "HP Frequency",
     "Detector",
     "Hz",
     S::logarithmic,
     20,
     2'000,
     100,
     {},
     T::live,
     g},
}};

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

} // namespace oscillate::native_dsp::effects::press_table
