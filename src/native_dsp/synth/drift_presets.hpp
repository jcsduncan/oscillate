#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <string_view>
#include <utility>

// Drift factory presets: only values that differ from the table defaults
// (the "Nebula"). Table choices follow the shared table names (4 Vowel, 9 Noise
// Spectra, 10 Bell Partials, 11 Organ Drawbars, 12 Digital Stack, 13
// Hollow). Golden fingerprints pin each.
namespace oscillate::native_dsp::synth::drift_presets {

using V = std::pair<std::string_view, double>;

inline constexpr std::array glass_rain{
    V{"drift.wt.table", 10},          V{"drift.grain.size", 0.05},
    V{"drift.grain.density", 60},     V{"drift.grain.pitch", 12},
    V{"drift.grain.intervals", 3},    V{"drift.grain.interval_amount", 0.5},
    V{"drift.grain.reverse", 0.1},    V{"drift.grain.spray", 0.3},
    V{"drift.verb.shimmer", 0.5},     V{"drift.verb.decay", 8},
    V{"drift.filter.cutoff", 12'000}, V{"drift.amp.attack", 0.3},
    V{"drift.out.level", -5},
};

inline constexpr std::array frozen_choir{
    V{"drift.freeze", 1},       V{"drift.grain.size", 0.3},  V{"drift.grain.density", 18},
    V{"drift.grain.scan", 0.1}, V{"drift.grain.spray", 0.4}, V{"drift.partials.level", 0.3},
    V{"drift.verb.mix", 0.45},  V{"drift.verb.decay", 10},   V{"drift.amp.attack", 2},
};

inline constexpr std::array shimmer_halo{
    V{"drift.verb.shimmer", 0.8},          V{"drift.verb.mix", 0.55},   V{"drift.verb.decay", 14},
    V{"drift.grain.interval_amount", 0.5}, V{"drift.grain.size", 0.25}, V{"drift.out.level", -2},
};

inline constexpr std::array dust_motes{
    V{"drift.grain.size", 0.012}, V{"drift.grain.density", 120}, V{"drift.grain.chaos", 0.9},
    V{"drift.grain.spray", 0.8},  V{"drift.air.level", 0.5},     V{"drift.wt.level", 0.3},
    V{"drift.filter.type", 3},    V{"drift.filter.cutoff", 800}, V{"drift.amp.attack", 0.5},
};

inline constexpr std::array tidal_pad{
    V{"drift.lfo1.position", 0.8}, V{"drift.lfo1.rate", 0.05},   V{"drift.lfo2.filter", 0.5},
    V{"drift.grain.size", 0.35},   V{"drift.grain.density", 14}, V{"drift.grain.scan", 0.5},
    V{"drift.amp.attack", 2.5},    V{"drift.amp.release", 5},
};

inline constexpr std::array stutter_clock{
    V{"drift.sync", 1},          V{"drift.sync.division", 2}, V{"drift.grain.size", 0.06},
    V{"drift.grain.shape", 2},   V{"drift.grain.chaos", 0},   V{"drift.grain.spray", 0},
    V{"drift.grain.scan", 1},    V{"drift.blur", 0},          V{"drift.grain.reverse", 0},
    V{"drift.amp.attack", 0.01}, V{"drift.amp.sustain", 1},   V{"drift.amp.release", 0.5},
    V{"drift.verb.mix", 0.2},
};

inline constexpr std::array reverse_bloom{
    V{"drift.grain.reverse", 0.9}, V{"drift.grain.shape", 3},  V{"drift.grain.size", 0.4},
    V{"drift.grain.density", 10},  V{"drift.amp.attack", 1.5}, V{"drift.verb.mix", 0.4},
};

inline constexpr std::array granite_drone{
    V{"drift.source.octave", -1},        V{"drift.wt.table", 9},
    V{"drift.partials.stretch", 0.0015}, V{"drift.partials.tilt", 0.6},
    V{"drift.grain.size", 0.2},          V{"drift.grain.density", 30},
    V{"drift.grain.scan", -0.5},         V{"drift.filter.type", 1},
    V{"drift.filter.cutoff", 2'500},     V{"drift.filter.resonance", 0.3},
    V{"drift.amp.release", 6},
};

inline constexpr std::array aurora{
    V{"drift.wt.table", 12}, V{"drift.grain.intervals", 2}, V{"drift.grain.interval_amount", 0.4},
    V{"drift.fx.chorus", 2}, V{"drift.lfo2.filter", 0.4},   V{"drift.grain.position", 0.3},
};

inline constexpr std::array spectral_smear{
    V{"drift.blur", 0.9},       V{"drift.grain.jitter", 40},  V{"drift.grain.spray", 0.6},
    V{"drift.grain.size", 0.3}, V{"drift.grain.density", 40}, V{"drift.out.level", -2},
};

inline constexpr std::array ice_cave{
    V{"drift.freeze", 1},         V{"drift.grain.scan", -1},   V{"drift.verb.shimmer", 0.6},
    V{"drift.verb.damping", 0.1}, V{"drift.verb.decay", 20},   V{"drift.verb.mix", 0.5},
    V{"drift.wt.table", 13},      V{"drift.grain.size", 0.15},
};

inline constexpr std::array pulse_grains{
    V{"drift.sync", 1},          V{"drift.sync.division", 5},           V{"drift.grain.size", 0.15},
    V{"drift.grain.shape", 1},   V{"drift.grain.interval_amount", 0.6}, V{"drift.fx.delay", 0.3},
    V{"drift.amp.attack", 0.05},
};

inline constexpr std::array deep_field{
    V{"drift.source.octave", -2},    V{"drift.grain.size", 0.5},  V{"drift.grain.density", 6},
    V{"drift.partials.level", 0.8},  V{"drift.partials.tilt", 2}, V{"drift.wt.level", 0.2},
    V{"drift.filter.cutoff", 1'200}, V{"drift.verb.decay", 12},   V{"drift.verb.mix", 0.5},
};

inline constexpr std::array wind_organ{
    V{"drift.air.level", 0.6},    V{"drift.air.width", 0.3},  V{"drift.wt.table", 11},
    V{"drift.partials.odd", 0.6}, V{"drift.grain.size", 0.1}, V{"drift.grain.density", 40},
    V{"drift.lfo1.density", 0.5},
};

inline constexpr std::array init{
    V{"drift.blur", 0},
    V{"drift.grain.chaos", 0},
    V{"drift.grain.spray", 0},
    V{"drift.grain.jitter", 0},
    V{"drift.grain.reverse", 0},
    V{"drift.grain.intervals", 0},
    V{"drift.wt.drift", 0},
    V{"drift.air.level", 0},
    V{"drift.verb.mix", 0},
    V{"drift.fx.delay", 0},
    V{"drift.lfo1.position", 0},
    V{"drift.lfo2.filter", 0},
    V{"drift.amp.attack", 0.01},
    V{"drift.amp.sustain", 1},
    V{"drift.amp.release", 0.3},
    V{"drift.grain.scan", 1},
    V{"drift.grain.position", 0.1},
    V{"drift.grain.size", 0.1},
    V{"drift.grain.density", 20},
    V{"drift.grain.spread", 0},
    V{"drift.filter.cutoff", 20'000},
    V{"drift.filter.keytrack", 0},
    V{"drift.out.level", -5},
};

inline constexpr std::array<SynthPreset, 16> presets{{
    {"Nebula", "Pad", {}},
    {"Glass Rain", "Texture", glass_rain},
    {"Frozen Choir", "Frozen", frozen_choir},
    {"Shimmer Halo", "Pad", shimmer_halo},
    {"Dust Motes", "Texture", dust_motes},
    {"Tidal Pad", "Pad", tidal_pad},
    {"Stutter Clock", "Rhythmic", stutter_clock},
    {"Reverse Bloom", "Texture", reverse_bloom},
    {"Granite Drone", "Drone", granite_drone},
    {"Aurora", "Pad", aurora},
    {"Spectral Smear", "Texture", spectral_smear},
    {"Ice Cave", "Frozen", ice_cave},
    {"Pulse Grains", "Rhythmic", pulse_grains},
    {"Deep Field", "Drone", deep_field},
    {"Wind Organ", "Drone", wind_organ},
    {"Init", "Init", init},
}};

} // namespace oscillate::native_dsp::synth::drift_presets
