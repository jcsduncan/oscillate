#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Deck presets: only values that differ from the (flat) defaults.
namespace oscillate::native_dsp::effects::deck_presets {

using V = std::pair<std::string_view, double>;

inline constexpr std::array bass_swap{V{"deck.low_kill", 1}};
inline constexpr std::array mids_out{V{"deck.mid_kill", 1}};
inline constexpr std::array highs_out{V{"deck.high_kill", 1}};
inline constexpr std::array bass_only{V{"deck.mid_kill", 1}, V{"deck.high_kill", 1}};
inline constexpr std::array warm{V{"deck.low", 2}, V{"deck.high", -3}};
inline constexpr std::array crisp{V{"deck.low", -2}, V{"deck.high", 3}};

inline constexpr std::array<synth::SynthPreset, 7> presets{{
    {"Flat", "Mixer", {}},
    {"Bass Swap (Low Killed)", "Transition", bass_swap},
    {"Mids Out", "Transition", mids_out},
    {"Highs Out", "Transition", highs_out},
    {"Bass Only", "Transition", bass_only},
    {"Warm", "Tone", warm},
    {"Crisp", "Tone", crisp},
}};

} // namespace oscillate::native_dsp::effects::deck_presets
