#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Lens factory presets: only values that differ from the table defaults
// ("Mix": Mid, Balanced, Medium average, 1/12 octave, 1.5 dB/oct, hold on).
namespace oscillate::native_dsp::effects::lens_presets {

using V = std::pair<std::string_view, double>;

inline constexpr std::array mastering{V{"lens.resolution", 2}, V{"lens.average", 2},
                                      V{"lens.smoothing", 2}, V{"lens.tilt", 2}};
inline constexpr std::array low_end{V{"lens.resolution", 2}, V{"lens.smoothing", 0},
                                    V{"lens.tilt", 0}};
inline constexpr std::array stereo_field{V{"lens.view", 4}};
inline constexpr std::array reference_match{V{"lens.overlay", 1}, V{"lens.average", 2},
                                            V{"lens.smoothing", 3}, V{"lens.tilt", 2}};
inline constexpr std::array transients{V{"lens.resolution", 0}, V{"lens.average", 0},
                                       V{"lens.hold_decay", 30}};
inline constexpr std::array long_term{V{"lens.average", 3}, V{"lens.smoothing", 3},
                                      V{"lens.tilt", 2}, V{"lens.hold", 0}};

inline constexpr std::array<synth::SynthPreset, 7> presets{{
    {"Mix", "Mixing", {}},
    {"Mastering", "Mastering", mastering},
    {"Low End", "Mixing", low_end},
    {"Stereo Field", "Mixing", stereo_field},
    {"Reference Match", "Mastering", reference_match},
    {"Transients", "Mixing", transients},
    {"Long-Term Average", "Mastering", long_term},
}};

} // namespace oscillate::native_dsp::effects::lens_presets
