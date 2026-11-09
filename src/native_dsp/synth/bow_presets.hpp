#pragma once

#include "bow_table.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/bow.hpp"

#include <array>
#include <string_view>
#include <utility>

// Bow's factory presets (ADR-0111): only values that differ from the table
// defaults (the "Sul Tasto Pad": violas, Section, Legato, mp, Tone toward
// sul tasto). Each sits under a pad: restrained dynamics, slow releases,
// modest vibrato. Choices are 0-based, in the table's order.
namespace oscillate::native_dsp::synth::bow_presets {

using V = std::pair<std::string_view, double>;
using namespace std::string_view_literals;

constexpr double section(bow::Section s) { return static_cast<double>(s); }
constexpr double mode(bow::Mode m) { return static_cast<double>(m); }
constexpr double articulation(bow::Articulation a) { return static_cast<double>(a); }

// Violins and violas together, warm and close to ordinario.
inline constexpr std::array warm_section{
    V{"bow.section", section(bow::Section::ensemble)},
    V{"bow.articulation", articulation(bow::Articulation::sustain)},
    V{"bow.perf.wheel", 0.5},
    V{"bow.vibrato", 0.45},
    V{"bow.tone", 0.42},
    V{"bow.players", 12.0},
    V{"bow.release", 1.1},
};

// Short, light plucks under a groove.
inline constexpr std::array pizzicato_ostinato{
    V{"bow.section", section(bow::Section::violins_2)},
    V{"bow.articulation", articulation(bow::Articulation::pizzicato)},
    V{"bow.tone", 0.5},
    V{"bow.players", 4.0},
    V{"bow.humanise", 0.35},
    V{"bow.space", 0.32},
};

// Measured tremolo that swells from the wheel's bottom.
inline constexpr std::array tremolo_swell{
    V{"bow.section", section(bow::Section::violins_1)},
    V{"bow.articulation", articulation(bow::Articulation::tremolo)},
    V{"bow.perf.wheel", 0.3},
    V{"bow.tone", 0.35},
    V{"bow.release", 2.0},
    V{"bow.space", 0.35},
};

// One cello, singing but not sugary.
inline constexpr std::array solo_cello{
    V{"bow.section", section(bow::Section::cellos)},
    V{"bow.mode", mode(bow::Mode::solo)},
    V{"bow.perf.wheel", 0.55},
    V{"bow.vibrato", 0.3},
    V{"bow.tone", 0.48},
    V{"bow.players", 1.0},
    V{"bow.humanise", 0.15},
    V{"bow.legato", 90.0},
    V{"bow.release", 0.8},
    V{"bow.space", 0.3},
};

// The whole ensemble, joined.
inline constexpr std::array ensemble_legato{
    V{"bow.section", section(bow::Section::ensemble)},
    V{"bow.perf.wheel", 0.48},
    V{"bow.vibrato", 0.3},
    V{"bow.tone", 0.45},
    V{"bow.players", 16.0},
    V{"bow.legato", 160.0},
    V{"bow.release", 1.6},
};

inline constexpr std::array pad_tags{"strings"sv, "pad"sv, "soft"sv};
inline constexpr std::array warm_tags{"strings"sv, "section"sv, "warm"sv};
inline constexpr std::array pizzicato_tags{"strings"sv, "pizzicato"sv, "short"sv};
inline constexpr std::array tremolo_tags{"strings"sv, "tremolo"sv, "swell"sv};
inline constexpr std::array cello_tags{"strings"sv, "solo"sv, "cello"sv};
inline constexpr std::array legato_tags{"strings"sv, "legato"sv, "ensemble"sv};

inline constexpr std::array<Preset, 6> presets{{
    {"Sul Tasto Pad", "Strings", {}, pad_tags},
    {"Warm Section", "Strings", warm_section, warm_tags},
    {"Pizzicato Ostinato", "Strings", pizzicato_ostinato, pizzicato_tags},
    {"Tremolo Swell", "Strings", tremolo_swell, tremolo_tags},
    {"Solo Cello", "Strings", solo_cello, cello_tags},
    {"Ensemble Legato", "Strings", ensemble_legato, legato_tags},
}};

} // namespace oscillate::native_dsp::synth::bow_presets
