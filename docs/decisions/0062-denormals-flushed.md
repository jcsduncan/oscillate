# ADR-0062 — Denormals flushed at every render entry

**Status:** Accepted; implemented. **Date:** 26 October 2026.
**Builds on:** ADR-0048 (audio worker pool), ADR-0061 (performance tracing).

## Context

No thread set the CPU's denormal handling. As a decaying signal approaches zero (a reverb tail, a filter or resampler ringing out after the notes end), it reaches values below about 1e-308, or 1e-38 for floats. The CPU computes these subnormal values in microcode, tens of times slower than normal ones.

Nova's Wobble Growl cost 17% of a core while playing. Half a second after release it cost 170%, until its FX tail fell to exact zero. A performance trace of a demo project showed the same kind of burst. Per-device `flushed()` guards cover a few filters but no effect chain, plugin or mixer.

## Decision

`engine::DenormalsFlushed` (include/oscillate/engine/denormals.hpp) sets flush-to-zero and denormals-are-zero for its lifetime and then restores the previous mode. On x86 those are the MXCSR FTZ and DAZ bits; on AArch64 it is FPCR FZ. Every render entry holds one:
- `audio_io::render_transport_block`, the device callback for PipeWire and JACK and everything they render, VST3 plugins included;
- `AudioWorkerPool::run_worker`, for the worker's life;
- `NativeSynthRuntime::process` and `NativeEffectRuntime::process`;
- the offline master render.

Live playback, export, previews, tests and benches therefore compute alike.

## Consequences

- A silent tail costs what silence costs: Wobble Growl's tail fell from 170% of a core to 7%.
- Values that were subnormal are now exact zeros. That is a difference of at most about −757 dBFS in Nova's renders, and no existing golden or parity test changed.
- The guard costs two register writes per entry.
