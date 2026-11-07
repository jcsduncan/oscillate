# ADR-0087 — Nova voices on the GPU

**Status:** Rejected. Nova voices render on the CPU ([ADR-0085](0085-nested-voice-rendering.md)); there is no GPU path. **Date:** 5 November 2026.
**Builds on:** ADR-0057 (Nova), ADR-0061 (performance tracing), ADR-0085 (voices across helpers).

## Context

Requirement: run five to ten Novas at full tilt at once. Measured on one core in a Release build, an 8-note chord with 3 oscillators × 16 unison and both filters cost 55–60 % of a core on average (slowest 1 % of blocks at 95–106 %). About 60 % of that is the unison oscillator banks. A probe on an RTX 3080 rendered Nova-shaped oscillator work for 20 instances × 16 voices well inside a 64-frame block, with no batch more than 2.5 ms late while the DAW was playing.

## What was tried

- An optional CUDA voice backend, loaded at run time, off by default and switched from the Audio Output panel. The build and the app worked without CUDA.
- Nova's oscillator banks (wavetable and classic shapes, unison, BLEP, non-crossed warps, oversampling) rendered on the GPU in one launch per control tick for every Nova, with oscillator state resident on the device. Filters, FX, modulation and crossed warps stayed on the CPU.
- To hide the round trip, a Nova with the switch on reported a fixed 5 ms latency to delay compensation and ran as two pipelined halves: a front half that made voice decisions and queued GPU chunks, and a back half that rendered everything else 5 ms later.
- Parity tests held each preset's GPU render within −90 dB of its CPU render at 44.1, 48 and 96 kHz.

## Why it was reverted

- **Live playback became unusable.** The pipelined Nova ran its control side twice, the GPU server thread spun a core, and the GPU changes slowed Nova's CPU path even with the switch off.
- **The evidence measured the wrong thing.** The parity tests were bit-exact or within tolerance, and the benchmark reported the rendering thread's share of one block on one thread. Neither measured whole-process CPU time during real playback, so the regression was not visible until the app was used.

## Gate for any retry

A GPU or other offload path returns only with a performance gate in the tests, alongside parity:

- whole-process CPU time (all threads, including any server thread), not one thread's share of a block;
- a light project and a heavy one, each with the switch off and on;
- p99 block time, not only the mean;
- the switch off must cost nothing measurable against the CPU-only build.

## Consequences

- Nova renders on the CPU only. Voices across helpers (ADR-0085) remains the way a heavy Nova is spread over cores.
- The export speed-ups made alongside this work touch export only and were kept.
