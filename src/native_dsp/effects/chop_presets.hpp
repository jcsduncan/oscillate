#pragma once

#include "chop_table.hpp"

#include "oscillate/native_dsp/synth/parameters.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Chop factory presets (ADR-0075). Each
// pattern is one bar of sixteen steps; steps 17 to 32 repeat it, so 32
// steps plays it twice. Choice values are 0-based (see chop_table).
namespace oscillate::native_dsp::effects::chop_presets {

using V = std::pair<std::string_view, double>;
namespace t = chop_table;

// One step's five values; the defaults are Init's open step.
struct Step {
  double level = 1.0;
  double length = 1.0;
  double ratchet = 1.0;
  double repeat = 0.0;
  double chance = 1.0;
};

// The bar a preset draws, repeated across all 32 steps.
inline constexpr std::size_t bar_steps = 16;
inline constexpr std::size_t step_values = chop::max_steps * t::step_fields;

// The preset's globals followed by every step's five values, drawn by
// `rule(step)` for the step's place in the bar (0..15).
template <std::size_t G, typename Rule>
consteval std::array<V, G + step_values> pattern(std::array<V, G> globals, Rule rule) {
  std::array<V, G + step_values> out{};
  std::ranges::copy(globals, out.begin());
  for (std::size_t s = 0; s < chop::max_steps; ++s) {
    const Step step = rule(s % bar_steps);
    const std::array<double, t::step_fields> fields{step.level, step.length, step.ratchet,
                                                    step.repeat, step.chance};
    for (std::size_t f = 0; f < t::step_fields; ++f) {
      const std::size_t index = t::first_step + s * t::step_fields + f;
      out[G + s * t::step_fields + f] = V{t::parameters[index].id, fields[f]};
    }
  }
  return out;
}

// Every step open: Chop is the input.
inline constexpr std::array<V, 0> init{};

// The off-beat open, the eighths between at half level, the downbeats shut.
inline constexpr auto off_beat_chop = pattern(std::array<V, 0>{}, [](std::size_t i) {
  constexpr double open = 1.0, half = 0.55, shut = 0.0, length = 0.7;
  return Step{i % 4 == 2 ? open : (i % 2 == 1 ? half : shut), length};
});

// Every sixteenth, the beat accented.
inline constexpr auto rolling_16ths = pattern(std::array<V, 0>{}, [](std::size_t i) {
  constexpr double accent = 1.0, rest = 0.7, length = 0.45;
  return Step{i % 4 == 0 ? accent : rest, length};
});

// A minimal-techno chop: the second sixteenth of each beat a coin toss,
// a triplet ratchet at the end of each half bar.
inline constexpr auto minimal_stutter = pattern(std::array<V, 0>{}, [](std::size_t i) {
  constexpr std::array<double, 4> levels{1.0, 0.0, 0.8, 0.6};
  constexpr double length = 0.5, ratchets = 3.0, coin = 0.5;
  return Step{levels[i % 4], length, i % 8 == 7 ? ratchets : 1.0, 0.0, i % 4 == 1 ? coin : 1.0};
});

// Long stabs on every off-beat eighth, silence between.
inline constexpr auto dub_stab = pattern(std::array<V, 0>{}, [](std::size_t i) {
  constexpr double length = 0.9;
  return Step{i % 8 == 2 || i % 8 == 6 ? 1.0 : 0.0, length};
});

// A triplet roll into every beat.
inline constexpr auto triplet_roll = pattern(std::array<V, 0>{}, [](std::size_t i) {
  constexpr double level = 0.85, length = 0.6, ratchets = 3.0;
  return Step{level, length, i % 4 == 3 ? ratchets : 1.0};
});

// The last beat of the bar stutters (2, 4, 4 then 8 repeats, falling),
// after a four-hit ratchet.
inline constexpr auto glitch_fill =
    pattern(std::array{V{"chop.mode", static_cast<double>(t::gate_repeat)}, V{"chop.repeat_pitch", 5.0},
                       V{"chop.repeat_decay", 0.3}},
            [](std::size_t i) {
              constexpr std::size_t fill = bar_steps - 4;
              constexpr std::array<double, 4> repeats{1.0, 2.0, 2.0, 3.0};
              constexpr double length = 0.6, ratchets = 4.0;
              if (i >= fill)
                return Step{1.0, 1.0, 1.0, repeats[i % 4]};
              return Step{1.0, length, i == fill - 1 ? ratchets : 1.0};
            });

inline constexpr std::array<synth::SynthPreset, 7> presets{{
    {"Init", "Init", init},
    {"Off-beat chop", "Gate", off_beat_chop},
    {"Rolling 16ths", "Gate", rolling_16ths},
    {"Minimal stutter", "Gate", minimal_stutter},
    {"Dub stab", "Gate", dub_stab},
    {"Triplet roll", "Ratchet", triplet_roll},
    {"Glitch fill", "Repeat", glitch_fill},
}};

} // namespace oscillate::native_dsp::effects::chop_presets
// clang-format on
