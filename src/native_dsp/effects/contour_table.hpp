#pragma once

#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/transport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Contour v1: a multimode filter effect built from the synths' filter models
// (ADR-0040). Defaults are the "Warm Low-Pass" preset. Ids are persisted and
// automated: never rename or reuse one.
namespace oscillate::native_dsp::effects::contour_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array models{"LP 12"sv, "LP 24"sv,  "BP 12"sv,  "HP 12"sv,
                                   "HP 24"sv, "Notch"sv,  "Ladder"sv, "OTA"sv,
                                   "SEM"sv,   "Comb +"sv, "Comb -"sv};
enum Model : std::uint8_t {
  lp12,
  lp24,
  bp12,
  hp12,
  hp24,
  notch,
  ladder,
  ota,
  sem,
  comb_plus,
  comb_minus
};
inline constexpr std::array lfo_shapes{"Sine"sv,     "Triangle"sv, "Saw Up"sv,
                                       "Saw Down"sv, "Square"sv,   "S&H"sv};
inline constexpr std::array off_on{"Off"sv, "On"sv};
inline constexpr std::array sources{"Input"sv, "Sidechain"sv};
inline constexpr const auto& divisions = sync_divisions;

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = effect_glide_seconds;

inline constexpr std::array<ParameterDescriptor, 17> parameters{{
    {"contour.model", "Model", "Filter", "", S::choice, 0, last(models.size()), 1, models, T::live,
     0},
    {"contour.cutoff", "Cutoff", "Filter", "Hz", S::logarithmic, 20, 20'000, 1'200, {}, T::live, g},
    {"contour.resonance", "Resonance", "Filter", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
    {"contour.drive", "Drive", "Filter", "dB", S::linear, 0, 24, 0, {}, T::live, g},
    {"contour.morph", "Morph", "Filter", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    {"contour.lfo.shape", "Shape", "LFO", "", S::choice, 0, last(lfo_shapes.size()), 0, lfo_shapes,
     T::live, 0},
    {"contour.lfo.rate", "Rate", "LFO", "Hz", S::logarithmic, 0.01, 20, 0.5, {}, T::live, g},
    {"contour.lfo.sync", "Sync", "LFO", "", S::choice, 0, 1, 0, off_on, T::live, 0},
    {"contour.lfo.division", "Division", "LFO", "", S::choice, 0, last(divisions.size()), 8,
     divisions, T::live, 0},
    {"contour.lfo.depth", "Depth", "LFO", "st", S::linear, 0, 48, 0, {}, T::live, g, true, 2},
    {"contour.lfo.stereo", "Stereo", "LFO", "deg", S::linear, 0, 180, 0, {}, T::live, g},
    {"contour.env.amount", "Amount", "Envelope", "st", S::linear, -48, 48, 0, {}, T::live, g},
    {"contour.env.attack", "Attack", "Envelope", "ms", S::logarithmic, 0.1, 100, 5, {}, T::live, g},
    {"contour.env.release",
     "Release",
     "Envelope",
     "ms",
     S::logarithmic,
     5,
     2'000,
     120,
     {},
     T::live,
     g},
    {"contour.env.source", "Source", "Envelope", "", S::choice, 0, 1, 0, sources, T::live, 0},
    {"contour.mix", "Mix", "Output", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"contour.output", "Output", "Output", "dB", S::linear, -24, 12, 0, {}, T::live, g},
}};

consteval std::size_t index_of(std::string_view id) { return parameter_index(parameters, id); }

} // namespace oscillate::native_dsp::effects::contour_table
