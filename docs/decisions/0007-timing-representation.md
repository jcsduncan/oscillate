# ADR-0007 — Musical timing and meter representation

**Status:** Accepted; implemented (reviewed 20 October 2026). 960,000,000 ticks per quarter note (`include/oscillate/timeline/timing.hpp`); tempo and meter maps and the shared conversions live in `src/timeline`.
**Date:** 14 October 2026
**Requirements:** PRD §§7, 9–10, 13–14, 17; FS-01 TA-01–04/08–10.

## Context

Bars, MIDI, automation, warp and export need one musical coordinate and conversion contract. The PRD requires instantaneous, linear-BPM and simple ease tempo changes and an editable time-signature map, but leaves tick scale, ramp interpretation, bar numbering and rounding open. Cross-component code must not each choose its own formula.

## Decision

`timeline` uses nonnegative signed 64-bit project ticks at **960,000,000 ticks per quarter note**. Public `TickPosition` and `SamplePosition` are distinct strong types. Valid project rates are 44,100, 48,000, 88,200 and 96,000 Hz. Immutable `TempoMap` and `MeterMap` each require an initial event at tick zero; events are strictly increasing and unique. Invalid input and overflow return the foundation `Result` error contract. The project serializes the tick scale and timing-map version.

A tempo event stores tick, positive finite BPM and the interpolation shape to the next event. `Step` holds BPM until the next event; the final event is `Step`. `LinearBpm` means BPM linear in musical quarter-note beats. For a segment of length `L` quarters, starting BPM `b0`, ending BPM `b1`, `delta=b1-b0`, and normalized position `u`, elapsed seconds are `60*L/delta*log1p(delta*u/b0)`; its inverse uses `expm1`. A constant-BPM branch handles zero or near-zero delta. `EaseIn`, `EaseOut` and `EaseInOut` use fixed piecewise-linear BPM knots, evaluated by the same closed form per piece: at segment midpoint, 25% or 75% of the total BPM change for in or out; for in-out, quarter and three-quarter positions carry 12.5% and 87.5%. A tempo-event boundary belongs to the new segment; segments are half-open.

A meter event stores numerator and power-of-two denominator. Numerator is 1–32 and denominator is 1–64. A change is valid only at a whole bar boundary under the preceding meter. Bar 1, beat 1 starts at tick zero; bar numbering continues across changes. There is no pickup bar in v1. The denominator defines beat grouping and ruler/snap labels; tempo remains quarter-note BPM.

Forward conversion accumulates continuous elapsed seconds from tick zero and rounds only the final sample coordinate to nearest integer, ties to even. The inverse returns the nearest representable tick using the same rounding rule; conversion must be monotone, though adjacent ticks may map to the same sample. Elapsed-anchor conversion on project-rate change uses exact integer quotient/remainder rational scaling with ties to even and no compiler extensions; undo restores original coordinates rather than reverse rounding. Playback, UI and export call the same evaluator.

## Alternatives considered

- A 960-tick scale is familiar for MIDI but cannot represent sample-adjacent edits at all supported rates and tempos with sufficient resolution.
- Floating-point beat positions simplify some UI calculations but make serialization, equality and boundary ordering fragile.
- Numeric quadrature for smooth tempo curves allows more shapes but makes callback/export agreement and inverse error bounds harder to qualify in the first implementation slice.
- Restart bar numbering or permit meter events mid-bar: both create ambiguous ruler and MIDI loop semantics at a change.

## Consequences and evidence

The high tick scale needs checked 64-bit arithmetic and long-session overflow tests. Piecewise ease ramps have explicit slope changes, a v1 quality tradeoff. G1 requires hand-calculated tempo and meter boundary fixtures, round-trip and monotonicity properties, malformed input and overflow cases, serialization fixtures, and playback/export sample-position agreement. The exact API remains small; the timing owner and cross-component reviewer approve it before dependent code merges.
