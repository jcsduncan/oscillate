# Automation curve core (AM-02, AM-03, AM-07)

This module is an **unintegrated** evaluator for project and clip parameter curves. It has no
project schema, editing commands, graph node, smoothing, plugin parameter bridge, or UI yet.
The parent build adds `src/automation` and `tests/automation` through top-level CMake.

## Prepared contract

`PreparedAutomation::create` is called off the audio callback. It validates a finite physical
parameter range and default, points sorted at unique nonnegative absolute coordinates, values
inside the domain, finite monotone Bézier handles, an exclusive clip active interval, a supported
sample rate, and a non-null immutable `timeline::TempoMap`. An empty track curve means default
value. A clip envelope must contain at least one point. Discrete descriptors require integral
range/default/point values and always evaluate by hold. Prepared objects hold an owning shared
reference to the tempo map; graph publication/reclamation owns the prepared object lifetime.

Curve coordinates are absolute ticks for `musical` and absolute samples for `elapsed`.
Musical tick conversion always uses the authoritative `TempoMap::sample_to_tick`, including
tempo ramps. Points outside a clip's active interval are permitted so clip placement/editing
can move the interval without rewriting its stored curve. Before the first point the first value
holds; after the final point the final value holds. At a point, that point owns the value.

The starting point's `to_next` shape controls the following segment. With progress
`u=(current-start)/(end-start)`, shapes are hold = 0, linear = `u`, quadratic = `u²`, exponential
= `(exp(4u)-1)/(exp(4)-1)`, and ease-in/out = `u²(3-2u)`. Bézier handles are normalized to the
endpoints: `(x1,y1)` and `(x2,y2)` with `0≤x1≤x2≤1`, `0≤y1,y2≤1`; the evaluator solves the cubic
x coordinate by 24 fixed bisections, then reads its y coordinate. Values are interpolated in
the **physical** parameter range, clamped to that range. This fixes the evaluator's contract,
not the future UI's choice of logarithmic parameter display or copy/paste mapping.

The clip envelope fully replaces the track curve in `[active_begin, active_end)`. At
`active_end` the track curve resumes at its *current* value. No sum, implicit blend or time
shift occurs. Smoothing/de-click processing, where required, is a separate graph stage after
semantic evaluation. A missing track curve uses the descriptor default.

`fill_musical_ticks` writes a caller-owned tick buffer once per sample from the shared tempo
map. `evaluate_block(first,ticks,output)` reuses that immutable tick block for any number of
lanes at the same project revision, sample rate and start sample. The caller must ensure those
identities match; the current API does not attach runtime metadata to the span. Both functions
use bounded work, allocate no memory, take no lock and perform no I/O for valid inputs.
`evaluate_block(first,output)` is a convenience single-lane/offline path that calls the tempo
map per musical sample. Evaluation returns `false` for invalid spans/positions or a tempo
conversion failure; the output is undefined then. Control threads must validate the requested
block bounds before publication because failure creation in the timeline `Result` is not an
audio-thread diagnostic path. A future engine integration should apply the same prepared
revision to playback and export, and explicitly reject stale/revision-mismatched tick buffers.

## Local evidence

- GCC 15.2 and Clang 21.1 with warnings as errors: contract test passes.
- GCC ASan/UBSan: contract test passes. Targeted clang-tidy and clang-format pass.
- GCC line coverage for `src/automation/curve.cpp`: 127/131 = **96%**. Remaining lines are
  invalid enum/fallback branches unreachable from validated prepared curves.
- Allocation probe: 200 consecutive 128-frame blocks, shared tick generation plus musical
  Bézier evaluation, **zero heap allocations**.
- Uninstrumented release CPU baseline: Intel i7-10700K, GCC 15.2 `-O3`, 48 kHz, linear BPM
  ramp 120→90 over four quarters, three-point musical Bézier curve, 20,000 block iterations
  after warmup. Medians of five consecutive runs: 64 frames, tick generation 6.39 µs/block
  and curve evaluation 4.74 µs/block; 128 frames, 12.69 µs/block and 13.53 µs/block.
  CPU scaling was not pinned, so these are comparative local observations, not an audio
  deadline guarantee. This is a single-lane microbenchmark;
  graph CPU and underrun qualification remain open. The tick cost is shared across lanes.

## Follow-on timing optimization

The current `TempoMap::sample_to_tick` performs a map search and transcendental conversion for
each sample. To reduce whole-graph CPU, the **timeline** owner should add a bounded real-time
block cursor with the same rounding semantics, then replace the implementation of
`fill_musical_ticks`. Do not approximate musical curves by interpolating between converted
sample endpoints: doing so changes the requested curve under tempo ramps.
