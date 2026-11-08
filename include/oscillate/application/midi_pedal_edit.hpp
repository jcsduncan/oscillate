#pragma once

#include "oscillate/project/model.hpp"

#include <cstdint>
#include <span>
#include <vector>

// Pedal editing as a pianist sees it (ADR-0035, the pedal strip): a lane is
// a step function of pedal depth, shown as spans where the pedal is down.
// Pure functions over a clip's points, so editing rules are testable apart
// from the UI and every edit is one SetMidiPedalLane (one undo step). Points
// at or after the pattern's end are silent (kept by a shortened clip): they
// make no spans and edits pass them through untouched.
namespace oscillate::application {

// A run where a pedal is down at one depth. Adjacent spans with no gap are
// a depth change without a lift; a span with no lift before the pattern
// ends runs to the pattern's end (the clip end releases it).
struct PedalSpan {
  std::int64_t start = 0;
  std::int64_t end = 0;
  double depth = 1.0;
  bool operator==(const PedalSpan&) const = default;
};

[[nodiscard]] std::vector<PedalSpan> pedal_spans(std::span<const project::MidiPedalPoint> points,
                                                 std::int64_t pattern_length);

// The lane's value at a tick (0 before its first point).
[[nodiscard]] double pedal_value_at(std::span<const project::MidiPedalPoint> points,
                                    std::int64_t tick) noexcept;

// Paints [start, end) at `depth` (0 lifts), keeping whatever the lane did
// after `end`, and drops redundant points. Clamped to the pattern.
[[nodiscard]] std::vector<project::MidiPedalPoint>
paint_pedal(std::span<const project::MidiPedalPoint> points, std::int64_t start, std::int64_t end,
            double depth, std::int64_t pattern_length);

// Lifts the span containing `tick`; unchanged when the pedal is up there.
[[nodiscard]] std::vector<project::MidiPedalPoint>
remove_pedal_span(std::span<const project::MidiPedalPoint> points, timeline::TickPosition tick,
                  std::int64_t pattern_length);

// Legato pedalling: for each chord (distinct note starts), press `repress`
// ticks after it lands and lift exactly as the next chord lands, so each
// harmony rings without blurring into the next. The last chord holds to the
// pattern's end.
struct ChordPedalling {
  std::int64_t repress = 0;
  double depth = 1.0;
};
[[nodiscard]] std::vector<project::MidiPedalPoint>
pedal_per_chord(std::span<const project::MidiNote> notes, std::int64_t pattern_length,
                ChordPedalling pedalling);

// When a note actually stops sounding: its own end, or, if the sustain pedal
// is down (half or more) as it is released, the lift; or, if sostenuto went
// down while it was held, sostenuto's lift. Capped at the pattern's end.
[[nodiscard]] std::int64_t sounding_end(const project::MidiNote& note,
                                        const project::MidiClip& clip) noexcept;

} // namespace oscillate::application
