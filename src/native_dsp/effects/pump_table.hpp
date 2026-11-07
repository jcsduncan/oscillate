#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Pump v1 (ADR-0058). The shape itself lives in the document; these are
// what plays it. Defaults are Classic Pump on the quarter note, full band.
// Ids are persisted and automated: never rename or reuse one.
namespace oscillate::native_dsp::effects::pump_table {

using S = synth::ParameterScale;
using T = synth::ParameterTiming;
using namespace std::string_view_literals;

// MIDI restarts the cycle on each note-on of the tracks keying Pump, read
// from the timeline ahead of time: on the beat with no lookahead.
inline constexpr std::array triggers{"Tempo"sv, "Kick Audio"sv, "MIDI"sv};
enum Trigger : std::uint8_t { tempo, audio, midi };
inline constexpr std::array rates{"1/16"sv, "1/8"sv, "1/4"sv, "1/2"sv, "1 Bar"sv, "2 Bars"sv};
// Beats per cycle; the bar rates take the meter's beats per bar.
inline constexpr std::array<double, 6> rate_beats{0.25, 0.5, 1.0, 2.0, 0.0, 0.0};
inline constexpr std::array<double, 6> rate_bars{0.0, 0.0, 0.0, 0.0, 1.0, 2.0};
inline constexpr std::array feels{"Straight"sv, "Dotted"sv, "Triplet"sv};
inline constexpr std::array<double, 3> feel_scale{1.0, 1.5, 2.0 / 3.0};
// Three Band splits at the two crossovers and gives each band its own
// depth and, when the document draws one, its own curve.
inline constexpr std::array bands{"Full"sv, "Lows Only"sv, "Three Band"sv};
enum Band : std::uint8_t { full, lows, three };
// Which part of the stereo image pumps: both channels, the centre (the
// width stays put) or the sides.
inline constexpr std::array stereo_modes{"Stereo"sv, "Mid"sv, "Side"sv};
enum StereoMode : std::uint8_t { stereo, mid, side };
// Kick Spacing stretches each cycle to the gap until the next trigger (the
// next note, or the last gap between kicks), so half-time and fills land.
inline constexpr std::array follows{"Rate"sv, "Kick Spacing"sv};
enum Follow : std::uint8_t { rate_length, kick_spacing };
inline constexpr std::array listens{"Off"sv, "On"sv};
// Up to four shapes, A to D, each a curve in the document. Slot picks one
// to play, steps through the pattern, or morphs A into B.
inline constexpr std::size_t slot_count = 4;
inline constexpr std::array slot_names{"A"sv, "B"sv, "C"sv, "D"sv};
inline constexpr std::array slot_modes{"Pattern"sv, "A"sv, "B"sv, "C"sv, "D"sv, "Morph A-B"sv};
enum SlotMode : std::uint8_t { pattern, slot_a, slot_b, slot_c, slot_d, morph };
inline constexpr std::size_t max_pattern = 8;
inline constexpr std::array lookaheads{"Off"sv, "5 ms"sv, "10 ms"sv, "20 ms"sv};
inline constexpr std::array<double, 4> lookahead_ms{0, 5, 10, 20};

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = synth::effect_glide_seconds;

inline constexpr std::array<synth::ParameterDescriptor, 35> parameters{{
    {"pump.trigger", "Trigger", "Trigger", "", S::choice, 0, last(triggers.size()), 0, triggers,
     T::live, 0},
    {"pump.rate", "Rate", "Trigger", "", S::choice, 0, last(rates.size()), 2, rates, T::live, 0},
    {"pump.feel", "Feel", "Trigger", "", S::choice, 0, last(feels.size()), 0, feels, T::live, 0},
    // Kick Audio: how loud a kick must be to restart the cycle.
    {"pump.threshold", "Threshold", "Trigger", "dB", S::linear, -48, 0, -24, {}, T::live, g},
    // Lookahead is a latency: fixed per instance, so a change rebuilds the
    // route (ADR-0040) and it is not automatable. Kick Audio needs it to
    // duck on the kick rather than just after it.
    {"pump.lookahead", "Lookahead", "Trigger", "", S::choice, 0, last(lookaheads.size()), 0,
     lookaheads, T::live, 0, false},
    // Kick Audio listens only between these, so hats and snares in a drum
    // bus never restart the cycle; Listen plays what it hears.
    {"pump.detect_low", "Detect Low", "Trigger", "Hz", S::logarithmic, 20, 400, 30, {}, T::live, g},
    {"pump.detect_high", "Detect High", "Trigger", "Hz", S::logarithmic, 60, 4000, 160, {},
     T::live, g},
    {"pump.listen", "Listen", "Trigger", "", S::choice, 0, last(listens.size()), 0, listens,
     T::live, 0},
    // MIDI listens only to notes in this range: the kick of a drum rack.
    {"pump.note_low", "Note Low", "Trigger", "", S::stepped, 0, 127, 0, {}, T::live, 0},
    {"pump.note_high", "Note High", "Trigger", "", S::stepped, 0, 127, 127, {}, T::live, 0},
    // MIDI: how far a soft note ducks less (0: every note full depth).
    {"pump.velocity", "Velocity", "Trigger", "%", S::linear, 0, 1, 0, {}, T::live, g},
    {"pump.follow", "Length", "Trigger", "", S::choice, 0, last(follows.size()), 0, follows,
     T::live, 0},
    {"pump.depth", "Depth", "Shape", "%", S::linear, 0, 1, 1, {}, T::live, g},
    // Tempo: the second cycle of each pair starts late (50 % is straight).
    {"pump.swing", "Swing", "Shape", "%", S::linear, 0.5, 0.75, 0.5, {}, T::live, g},
    {"pump.slot", "Slot", "Shape", "", S::choice, 0, last(slot_modes.size()), 0, slot_modes,
     T::live, 0},
    {"pump.morph", "Morph", "Shape", "%", S::linear, 0, 1, 0, {}, T::live, g},
    // The pattern: this many cycles, each playing its step's slot.
    {"pump.pattern", "Pattern", "Shape", "", S::stepped, 1, max_pattern, 1, {}, T::live, 0},
    {"pump.step.1", "Step 1", "Pattern", "", S::choice, 0, 3, 0, slot_names, T::live, 0},
    {"pump.step.2", "Step 2", "Pattern", "", S::choice, 0, 3, 0, slot_names, T::live, 0},
    {"pump.step.3", "Step 3", "Pattern", "", S::choice, 0, 3, 0, slot_names, T::live, 0},
    {"pump.step.4", "Step 4", "Pattern", "", S::choice, 0, 3, 0, slot_names, T::live, 0},
    {"pump.step.5", "Step 5", "Pattern", "", S::choice, 0, 3, 0, slot_names, T::live, 0},
    {"pump.step.6", "Step 6", "Pattern", "", S::choice, 0, 3, 0, slot_names, T::live, 0},
    {"pump.step.7", "Step 7", "Pattern", "", S::choice, 0, 3, 0, slot_names, T::live, 0},
    {"pump.step.8", "Step 8", "Pattern", "", S::choice, 0, 3, 0, slot_names, T::live, 0},
    {"pump.nudge", "Nudge", "Shape", "ms", S::linear, -20, 20, 0, {}, T::live, g},
    // The shortest edge, 10 to 90 %, any drawn step becomes.
    {"pump.anticlick", "Anti-click", "Shape", "ms", S::logarithmic, 0.5, 10, 2, {}, T::live, g},
    {"pump.band", "Band", "Band", "", S::choice, 0, last(bands.size()), 0, bands, T::live, 0},
    {"pump.crossover", "Crossover", "Band", "Hz", S::logarithmic, 40, 400, 120, {}, T::live, g},
    {"pump.crossover_high", "High Crossover", "Band", "Hz", S::logarithmic, 800, 8000, 2500, {},
     T::live, g},
    // Three Band: the mids' and highs' depth (the lows take Depth).
    {"pump.depth_mid", "Mid Depth", "Band", "%", S::linear, 0, 1, 0.5, {}, T::live, g},
    {"pump.depth_high", "High Depth", "Band", "%", S::linear, 0, 1, 0.25, {}, T::live, g},
    {"pump.stereo", "Stereo", "Band", "", S::choice, 0, last(stereo_modes.size()), 0, stereo_modes,
     T::live, 0},
    {"pump.mix", "Mix", "Output", "%", S::linear, 0, 1, 1, {}, T::live, g},
    {"pump.output", "Output", "Output", "dB", S::linear, -12, 12, 0, {}, T::live, g},
}};

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

} // namespace oscillate::native_dsp::effects::pump_table
// clang-format on
