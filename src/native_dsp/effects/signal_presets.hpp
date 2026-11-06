#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Signal presets: only values that differ from the defaults (a 1 kHz sine
// at -18 dBFS on both channels, replacing the input).
namespace oscillate::native_dsp::effects::signal_presets {

using V = std::pair<std::string_view, double>;

inline constexpr std::array alignment{V{"signal.level", -20}};
inline constexpr std::array sweep{V{"signal.wave", 1}, V{"signal.level", -12}};
inline constexpr std::array pink{V{"signal.wave", 2}, V{"signal.level", -20}};
inline constexpr std::array white{V{"signal.wave", 3}, V{"signal.level", -20}};
inline constexpr std::array impulse{V{"signal.wave", 4}, V{"signal.level", -6}};
inline constexpr std::array channel_check{V{"signal.wave", 2}, V{"signal.level", -20},
                                          V{"signal.output", 3}};
inline constexpr std::array polarity_check{V{"signal.wave", 0}, V{"signal.frequency", 100},
                                           V{"signal.output", 4}};
inline constexpr std::array sub_check{V{"signal.frequency", 40}, V{"signal.level", -12}};

inline constexpr std::array<synth::SynthPreset, 9> presets{{
    {"1 kHz Reference", "Tone", {}},
    {"Alignment Tone -20", "Tone", alignment},
    {"Log Sweep", "Sweep", sweep},
    {"Pink Noise", "Noise", pink},
    {"White Noise", "Noise", white},
    {"Impulse Train", "Impulse", impulse},
    {"Channel Check", "Check", channel_check},
    {"Polarity Check", "Check", polarity_check},
    {"40 Hz Sub Check", "Tone", sub_check},
}};

} // namespace oscillate::native_dsp::effects::signal_presets
