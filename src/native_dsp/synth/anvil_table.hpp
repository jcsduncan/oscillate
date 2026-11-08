#pragma once

#include "oscillate/native_dsp/synth/lock.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/transport.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Anvil v1 parameter table (ADR-0060). Defaults are the "Warehouse Rumble"
// preset. Ids are persisted and automated: never rename or reuse one. Every
// parameter is live and automatable; choices and steps switch glide-free and
// the kernel crossfades the ones that change a sounding tail.
namespace oscillate::native_dsp::synth::anvil_table {

using S = ParameterScale;
using T = ParameterTiming;
using namespace std::string_view_literals;

inline constexpr std::array keys{"Fixed"sv, "Follow note"sv};
inline constexpr std::array waves{"Sine"sv, "Triangle"sv, "Soft Square"sv, "Harmonic"sv};
inline constexpr std::array clicks{"Tick"sv, "Noise"sv, "Beater"sv, "FM Snap"sv, "Sample"sv};
inline constexpr std::array switches{"Off"sv, "On"sv};
inline constexpr std::array drives{"Tape"sv, "Tube"sv, "Clip"sv, "Fold"sv};
// Appended only: saved values keep their meaning (lock.hpp).
inline constexpr std::array locks{"Off"sv, "Bass note"sv, "Keep tone"sv};
// How a hit's length is set: Length in milliseconds, a note value at the
// song's tempo, or the played note's own length.
inline constexpr std::array length_modes{"Time"sv, "Sync"sv, "Note"sv};
inline constexpr std::size_t length_time = 0;
inline constexpr std::size_t length_sync = 1;
inline constexpr std::size_t length_note = 2;
// Synced lengths run from 1/32 to one bar: a kick longer than a bar is a drone.
inline constexpr std::span<const std::string_view> length_divisions{sync_divisions.data(),
                                                                    first_bar_division + 1};
inline constexpr double length_division_default = 8; // "1/4"
// Every mode's hit lasts this long at least and at most (seconds); Time's
// own range sits inside it.
inline constexpr double shortest_hit_seconds = 0.02;
inline constexpr double longest_hit_seconds = 2.0;

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = 0.03;

inline constexpr std::array<ParameterDescriptor, 38> parameters{{
    // Hit
    {"anvil.length", "Length", "Hit", "ms", S::logarithmic, 50, longest_hit_seconds * 1'000, 420, {},
     T::live, g},
    {"anvil.tune", "Tune", "Hit", "st", S::linear, -24, 24, 0, {}, T::live, g},
    {"anvil.key", "Key", "Hit", "", S::choice, 0, last(keys.size()), 0, keys, T::live, 0},
    {"anvil.velocity", "Velocity", "Hit", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    {"anvil.velocity.drive", "Vel Drive", "Hit", "%", S::linear, 0, 1, 0.2, {}, T::live, g},
    // Body
    {"anvil.body.level", "Level", "Body", "%", S::linear, 0, 1, 0.9, {}, T::live, g},
    {"anvil.body.wave", "Wave", "Body", "", S::choice, 0, last(waves.size()), 0, waves, T::live, 0},
    {"anvil.body.harmonics", "Harmonics", "Body", "%", S::linear, 0, 1, 0.1, {}, T::live, g},
    {"anvil.body.phase", "Phase", "Body", "deg", S::linear, 0, 180, 90, {}, T::live, g},
    {"anvil.body.drive", "Drive", "Body", "%", S::linear, 0, 1, 0.4, {}, T::live, g},
    // Click
    {"anvil.click.type", "Type", "Click", "", S::choice, 0, last(clicks.size()), 0, clicks,
     T::live, 0},
    {"anvil.click.level", "Level", "Click", "%", S::linear, 0, 1, 0.35, {}, T::live, g},
    {"anvil.click.tone", "Tone", "Click", "%", S::linear, 0, 1, 0.6, {}, T::live, g},
    {"anvil.click.length", "Length", "Click", "ms", S::logarithmic, 1, 40, 5, {}, T::live, g},
    {"anvil.click.pitch", "Pitch", "Click", "st", S::linear, -24, 24, 0, {}, T::live, g},
    {"anvil.click.start", "Start", "Click", "ms", S::linear, 0, 50, 0, {}, T::live, g},
    // Sub
    {"anvil.sub.on", "Sub", "Sub", "", S::choice, 0, 1, 0, switches, T::live, 0},
    {"anvil.sub.level", "Level", "Sub", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
    {"anvil.sub.octave", "Octave", "Sub", "oct", S::stepped, -1, 1, 0, {}, T::live, 0},
    {"anvil.sub.attack", "Attack", "Sub", "ms", S::linear, 0, 40, 8, {}, T::live, g},
    // Rumble
    {"anvil.rumble.on", "Rumble", "Rumble", "", S::choice, 0, 1, 1, switches, T::live, 0},
    {"anvil.rumble.amount", "Amount", "Rumble", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
    {"anvil.rumble.decay", "Decay", "Rumble", "s", S::logarithmic, 0.1, 3, 0.75, {}, T::live, g},
    {"anvil.rumble.duck", "Duck", "Rumble", "%", S::linear, 0, 1, 0.9, {}, T::live, g},
    {"anvil.rumble.tone", "Tone", "Rumble", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
    {"anvil.rumble.drive", "Drive", "Rumble", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    {"anvil.rumble.space", "Space", "Rumble", "%", S::linear, 0, 1, 0.4, {}, T::live, g},
    {"anvil.rumble.width", "Width", "Rumble", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    // Master
    {"anvil.drive.type", "Drive Type", "Master", "", S::choice, 0, last(drives.size()), 0, drives,
     T::live, 0},
    {"anvil.drive.amount", "Drive", "Master", "%", S::linear, 0, 1, 0.35, {}, T::live, g},
    // Normalised: 0..1 is a low-pass from 200 Hz to 20 kHz, logarithmically.
    {"anvil.tone", "Tone", "Master", "%", S::linear, 0, 1, 0.7, {}, T::live, g},
    {"anvil.punch", "Punch", "Master", "%", S::linear, 0, 1, 0.35, {}, T::live, g},
    {"anvil.glue", "Glue", "Master", "%", S::linear, 0, 1, 0.3, {}, T::live, g},
    {"anvil.output", "Output", "Master", "dB", S::linear, -24, 6, -1, {}, T::live, g},
    // How much harder hits brighten the click (its level rises with velocity).
    {"anvil.click.velocity", "Click Velocity", "Click", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    // Kick & Bass Lock (ADR-0072): locked, Nova ducks under each hit. With
    // Bass note each hit's tail also moves to the root or fifth of the bass
    // note under it, whichever octave is nearest the drawn tail; Keep tone
    // leaves the drawn tail alone.
    {lock_kick_parameter, "Lock", "Lock", "", S::choice, 0, last(locks.size()), lock_choice_off, locks,
     T::live, 0},
    // How the hit's length is set (length_modes); Sync's note value.
    {"anvil.length.mode", "Length Mode", "Hit", "", S::choice, 0, last(length_modes.size()),
     length_time, length_modes, T::live, 0},
    {"anvil.length.division", "Length Sync", "Hit", "", S::choice, 0,
     last(length_divisions.size()), length_division_default, length_divisions, T::live, 0},
}};

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

} // namespace oscillate::native_dsp::synth::anvil_table
// clang-format on
