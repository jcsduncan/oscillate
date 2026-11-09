# ADR-0113 — Edge: an adjustable ceiling

**Status:** Accepted; implemented. **Date:** 9 November 2026.
**Supersedes in part:** [ADR-0108](0108-edge-clipper.md), where it fixes Edge's ceiling at 0 dBFS.
**Contract:** `include/oscillate/native_dsp/effects/edge.hpp`, the table `src/native_dsp/effects/edge_table.hpp`.

## Context

With its ceiling fixed at 0 dBFS, Edge on a master that feeds a limiter does nothing unless its input is driven past 0 dBFS, so the limiter does all the work. The clipper needs to clip where the limiter expects its input, a few dB down.

## Decision

1. **A Ceiling parameter,** `edge.ceiling`, −12 to 0 dBFS, default 0, live and automatable, gliding like Input and Output.
2. **One scale.** The clipper, the oversampler and the true-peak guard run as ADR-0108 describes, under a ceiling of 1: the driven input is divided by the ceiling on the way in, and the guard's output is multiplied by the ceiling it went in under, read from the aligned input's delay line. The filters are linear, so the curve, its knee (a fraction of the ceiling), the antialiasing and the guard's 0.01 dB margin all scale with it, and the output's true peak stays at or under the ceiling.
3. **What follows it.** Delta is the driven input less this output, so it still nulls. The clipped reading is measured under the scale and so is the dB over the ceiling. The input and output meters read levels, which the panel's bars mark at the ceiling. The transfer dot's output is the curve's at the ceiling, and `edge::clip(x, shape, ceiling_db)` draws the panel's curve.
4. **Presets** keep the 0 dBFS default and sound as they did.
5. **Panel:** a Ceiling knob between Shape and Output; the transfer's dashed line and its label sit at the ceiling; the input peak and output true-peak bars carry a mark at the ceiling.

## Consequences

- **Tests:** `native_edge_contract` adds a hard clip at lowered ceilings (the static curve and the kernel at 1×), the true peak under −12, −6 and −1 dBFS at 1× and 16× with both filters and both shape ends, the output meter under the ceiling, delta nulls at −6 dB, block splits and export against live under a lowered ceiling, and a ceiling change in the no-allocation callback. `quick_edge_panel_contract` checks the curve, the label and the meters' marks follow the ceiling. The screenshot shows Kick Punch under a −3 dBFS ceiling.
- A gliding ceiling changes the gain the clipper sees for the length of the round trip; the guard holds each frame to the ceiling it entered under.
