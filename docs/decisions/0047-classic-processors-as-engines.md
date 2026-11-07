# ADR-0047 — The classic processors become engines

**Status:** Accepted and implemented for EQ, compressor, limiter and reverb (schema 1.37). Convolution remains its own kind. **Date:** 20 October 2026.
**Completes:** ADR-0043 (one native insert path).

## Context

ADR-0043 put every native processor behind one factory. It kept the five original kinds as separate model types "for file compatibility":
- `NativeEq`;
- `NativeCompressor`;
- `NativeReverb`;
- `NativeLimiter`;
- `NativeConvolution`.

The project is greenfield and the codec reads only the current schema, so that reason no longer holds. The cost of keeping them is real:
- **Code:** about 600 type-specific references remain.
- **UI:** they have their own editors, compact controls and colours in the chain. The mixer cannot open them in place, and the browser lists them as grey "classic" cards beside the ADR-0040 engines.
- **Gaps:** the platform still has no EQ, compressor or limiter engine of its own, so these kinds cannot simply be deleted.

## Decision

Each classic kind becomes an ADR-0040 engine. It is then an ordinary `NativeEffectPatch` slot, and its old model type, codec kind, route kind, editor and special cases are deleted. One kind moves at a time:

1. **The engine**, in `src/native_dsp/effects/` beside the others: a descriptor, parameters with the same ranges and units, presets, telemetry for its panel, and the existing DSP moved behind the engine interface, not rewritten.
2. **Parity first.** A render test drives the old processor and the new engine with the same settings, input and block splits, and requires a largest difference of exactly 0 before anything else changes.
3. **Cut over.** The session, browser and tests create the engine. Then delete the model alternative, codec kind, `NativeInsertKind`/route kind, the projection's per-kind fields, the dedicated editor and its compact controls, and the mixer's hand-off to the arrangement.
4. **A panel** built with the device kit, as for the other effects.

Order: EQ, compressor and limiter first, since nothing else covers them. Then reverb: the classic master reverb becomes an Expanse preset if Expanse can reproduce it exactly, and otherwise its own small engine. Convolution comes last.

## Consequences

- Every native insert is a patch, edits live through one path, automates through effect parameter lanes, and opens in place from the chain and the mixer.
- The master chain's fixed EQ/compressor/reverb order in the direct playback graph ends with the last classic kind.
- No project migration is needed: there are no saved projects in old formats to keep.

## As built

- **EQ, compressor and limiter.** Shape, Press and Ceiling are bit-exact engines for the classic DSP. The cut-over removed the classic model types, codec kinds, route and stage kinds, the classic live-edit paths, both editors, the compact controls, the browser cards and the mixer hand-off. Schema 1.37 no longer knows the classic kinds.
  - **Ceiling's Gain** (26 October 2026). Ceiling had only a ceiling, lookahead and release, so it could only turn a mix down: lowering the ceiling to get gain reduction lowered the output with it. `ceiling.gain` (0 to +24 dB, automatable) drives the signal into the limiter before it, as a mastering limiter's gain does; the IN meter reads the mix before the gain. With Gain at 0 dB it is still bit-exact with the classic limiter.
  - **Shape's Slope** (26 October 2026). A single 12 dB/octave pass filter cannot clear sub rumble or the top end, and a shelf only lowers what is past it. Each band gains `shape.slope_N` (12, 24, 48 or 96 dB/octave), which a high-pass or low-pass runs as 1, 2, 4 or 8 Butterworth stages at one corner (`eq_pass_stages`): -3 dB at the corner and maximally flat, with Q scaling the most resonant stage. The classic cascade now holds up to 64 sections. A live slope change adds or removes one stage per control tick through the classic runtime's fades, so it never clicks. At 12 dB a band is still bit-exact with the classic EQ. The Clean Ends preset cuts at 48 dB/octave below 30 Hz and above 18 kHz.
- **Reverb.** Expanse covers it; the classic reverb was deleted without a parity engine, since a bit-exact copy of a weaker reverb has no value once Expanse exists.
- **Live edits.** The general built-in chain rebuild replaces the classic-only live paths: any built-in chain edit with unchanged latency applies at a held block boundary, keeping unchanged stages running.
- **Direct playback.** The direct instrument graph has no master effects; projects with any master effect play through the routed graph.
- **Convolution** keeps its own kind and route contract for now.
