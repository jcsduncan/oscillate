#pragma once

#include "oscillate/native_dsp/effects/chop.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Chop v1 (ADR-0075): the global controls, then 32 steps of five each.
// Defaults are Init: every step open at full level, so Chop is the input.
// Ids are persisted and automated: never rename or reuse one.
namespace oscillate::native_dsp::effects::chop_table {

using S = synth::ParameterScale;
using T = synth::ParameterTiming;
using namespace std::string_view_literals;

// Gate shapes the level step by step; Repeat stutters the steps that have
// a repeat and passes the rest; Gate + repeat does both.
inline constexpr std::array modes{"Gate"sv, "Repeat"sv, "Gate + repeat"sv};
enum Mode : std::uint8_t { gate, repeat, gate_repeat };
inline constexpr std::array step_counts{"16"sv, "32"sv};
inline constexpr std::array<std::size_t, 2> step_count_values{16, 32};
static_assert(step_count_values.back() == chop::max_steps);
// The step's length, in quarter-note beats.
inline constexpr std::array divisions{"1/8"sv, "1/16"sv, "1/32"sv};
inline constexpr std::array<double, 3> division_beats{0.5, 0.25, 0.125};

// Per step: level, length (the gate's share of the step, or of each
// ratchet), ratchet (hits inside the step), repeat (0 off; 1 to 3 replay
// the step's first half, quarter or eighth 2, 4 or 8 times) and chance.
inline constexpr std::size_t step_fields = 5;
enum StepField : std::uint8_t { level, length, ratchet, repeat_count, chance };
inline constexpr std::size_t max_ratchet = 4;
inline constexpr std::size_t max_repeat = 3;
inline constexpr std::size_t max_repeats_per_step = std::size_t{1} << max_repeat;

// The Smooth range, in milliseconds: every edge fades over this long.
inline constexpr double min_smooth_ms = 0.5;
inline constexpr double max_smooth_ms = 10.0;
inline constexpr double default_smooth_ms = 2.9;
inline constexpr double max_pitch_drop = 12.0;
inline constexpr double max_seed = 9'999.0;
inline constexpr double default_seed = 7.0;
inline constexpr double default_shape = 0.35;

constexpr double last(std::size_t size) { return static_cast<double>(size - 1); }
inline constexpr double g = synth::effect_glide_seconds;

// One step's five parameters. Chance has no glide, so its roll threshold
// is exact the moment it is set.
#define CHOP_STEP(N)                                                                               \
  {"chop.step." N ".level", "Level", "Step " N, "%", S::linear, 0, 1, 1, {}, T::live, g},          \
      {"chop.step." N ".length", "Length", "Step " N, "%", S::linear, 0, 1, 1, {}, T::live, g},    \
      {"chop.step." N ".ratchet",                                                                  \
       "Ratchet",                                                                                  \
       "Step " N,                                                                                  \
       "",                                                                                         \
       S::stepped,                                                                                 \
       1,                                                                                          \
       static_cast<double>(max_ratchet),                                                           \
       1,                                                                                          \
       {},                                                                                         \
       T::live,                                                                                    \
       0},                                                                                         \
      {"chop.step." N ".repeat",                                                                   \
       "Repeat",                                                                                   \
       "Step " N,                                                                                  \
       "",                                                                                         \
       S::stepped,                                                                                 \
       0,                                                                                          \
       static_cast<double>(max_repeat),                                                            \
       0,                                                                                          \
       {},                                                                                         \
       T::live,                                                                                    \
       0},                                                                                         \
      {"chop.step." N ".chance", "Chance", "Step " N, "%", S::linear, 0, 1, 1, {}, T::live, 0}

inline constexpr std::size_t global_count = 12;
inline constexpr std::array<synth::ParameterDescriptor,
                            global_count + chop::max_steps * step_fields>
    parameters{{
        {"chop.mode", "Mode", "Chop", "", S::choice, 0, last(modes.size()), 0, modes, T::live, 0},
        {"chop.steps", "Steps", "Chop", "", S::choice, 0, last(step_counts.size()), 0,
         step_counts, T::live, 0},
        {"chop.division", "Division", "Chop", "", S::choice, 0, last(divisions.size()), 1,
         divisions, T::live, 0},
        // Every edge on the grid is a fade this long, so nothing clicks.
        {"chop.smooth", "Smooth", "Edges", "ms", S::linear, min_smooth_ms, max_smooth_ms,
         default_smooth_ms, {}, T::live, g},
        // Hard, Punch, Soft: how long and how gently a gate lets go after
        // its length.
        {"chop.shape", "Shape", "Edges", "%", S::linear, 0, 1, default_shape, {}, T::live, g},
        // 0 is straight (50 %); 1 delays the even steps to 75 %.
        {"chop.swing", "Swing", "Groove", "%", S::linear, 0, 1, 0, {}, T::live, g},
        {"chop.rotate", "Rotate", "Groove", "", S::stepped, 0, last(chop::max_steps), 0, {},
         T::live, 0},
        // Each repeat in a step plays lower, the last this many semitones down.
        {"chop.repeat_pitch", "Repeat Pitch", "Repeat", "st", S::linear, 0, max_pitch_drop, 0, {},
         T::live, g},
        // Each repeat in a step plays quieter, the last by this much.
        {"chop.repeat_decay", "Repeat Decay", "Repeat", "%", S::linear, 0, 1, 0, {}, T::live, g},
        // How far a closed step goes: 1 is silence.
        {"chop.depth", "Depth", "Output", "%", S::linear, 0, 1, 1, {}, T::live, g},
        {"chop.mix", "Mix", "Output", "%", S::linear, 0, 1, 1, {}, T::live, g},
        // Chance rolls from (seed, pattern, step): Re-seed for another variation.
        {"chop.seed", "Seed", "Chance", "", S::stepped, 1, max_seed, default_seed, {}, T::live, 0},
        CHOP_STEP("1"),
        CHOP_STEP("2"),
        CHOP_STEP("3"),
        CHOP_STEP("4"),
        CHOP_STEP("5"),
        CHOP_STEP("6"),
        CHOP_STEP("7"),
        CHOP_STEP("8"),
        CHOP_STEP("9"),
        CHOP_STEP("10"),
        CHOP_STEP("11"),
        CHOP_STEP("12"),
        CHOP_STEP("13"),
        CHOP_STEP("14"),
        CHOP_STEP("15"),
        CHOP_STEP("16"),
        CHOP_STEP("17"),
        CHOP_STEP("18"),
        CHOP_STEP("19"),
        CHOP_STEP("20"),
        CHOP_STEP("21"),
        CHOP_STEP("22"),
        CHOP_STEP("23"),
        CHOP_STEP("24"),
        CHOP_STEP("25"),
        CHOP_STEP("26"),
        CHOP_STEP("27"),
        CHOP_STEP("28"),
        CHOP_STEP("29"),
        CHOP_STEP("30"),
        CHOP_STEP("31"),
        CHOP_STEP("32"),
    }};

#undef CHOP_STEP

consteval std::size_t index_of(std::string_view id) {
  return synth::parameter_index(parameters, id);
}

inline constexpr std::size_t first_step = index_of("chop.step.1.level");
static_assert(first_step == global_count);
static_assert(index_of("chop.step.32.chance") ==
              first_step + chop::max_steps * step_fields - 1);
static_assert(index_of("chop.step.2.level") == first_step + step_fields);
static_assert(index_of("chop.step.1.chance") == first_step + chance);

} // namespace oscillate::native_dsp::effects::chop_table
// clang-format on
