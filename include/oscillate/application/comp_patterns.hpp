#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/timeline/timing.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// Comp (ADR-0083, ADR-0084): its patterns as the panel offers them, and a
// rhythm applied to notes.
namespace oscillate::application::comp {

inline constexpr std::string_view engine_id = "comp";

// One of Comp's patterns: the engine preset of the same name, with the line
// the panel shows under it.
struct CompPattern {
  std::string_view name;
  std::string_view summary;
};
// In the engine's preset order.
[[nodiscard]] std::span<const CompPattern> patterns() noexcept;

// `current` (a Comp's saved parameters; empty for a new Comp) with pattern
// `index` applied: its lanes, lengths, top length, push and groove; every
// other setting (velocity, humanise, seed, octave) is kept. Sparse, as saved.
[[nodiscard]] foundation::Result<std::vector<project::NativeInstrumentParameter>>
pattern_parameters(std::size_t index, std::span<const project::NativeInstrumentParameter> current);

// The notes Comp with `parameters` (sparse, as saved) plays over `held`
// (clip-local notes, played as held input) between `start` and `end`
// (clip-local ticks), for a clip whose tick 0 is song tick `clip_start`:
// clip-local, sorted, ids numbered from 1 in order, a note still sounding
// at `end` cut there.
// The same kernel, clock and lookahead as playback, so Apply == Play live
// from where the range's notes begin (Comp starts there: a push that would
// land before it is not seen).
[[nodiscard]] foundation::Result<std::vector<project::MidiNote>>
render_notes(std::span<const project::NativeInstrumentParameter> parameters,
             std::span<const project::MidiNote> held, const timeline::TempoMap& tempo,
             std::uint32_t sample_rate, timeline::TickPosition clip_start,
             timeline::TickPosition start, timeline::TickPosition end);

} // namespace oscillate::application::comp
