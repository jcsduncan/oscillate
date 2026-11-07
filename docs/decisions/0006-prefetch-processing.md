# ADR-0006 — Media prefetch and adaptive playback processing

**Status:** Accepted; implemented. The clip prefetch worker with priming status, per-clip rings and transport-generation tags is `src/application/clip_prefetch_worker.cpp` and `prefetch_control.cpp`.
**Date:** 14 October 2026
**Requirements:** PRD §§7–8, 11–12; FS-02 ER-05/ER-11; FS-05 PA-05/PA-07.

## Context

The callback cannot perform disk I/O or invoke a decoder. Original WAV, AIFF, FLAC and MP3 media may differ from the project's nominal rate. A missing processed cache still needs an adaptive playback path for stretch and pitch. Calling arbitrary DSP libraries in the callback would impose an unverified real-time safety requirement and make underruns unbounded.

## Decision

`media` owns a lookahead prefetch service. Worker threads decode source media, convert source rate to the project's nominal rate, apply cache-miss stretch/pitch processing, and fill preallocated per-clip single-producer/single-consumer rings. The callback consumes ready frames only. Rings and requests carry clip, settings and transport generations. Unanticipated seek and edit/rate changes discard stale output; anticipated loop wraps have next-iteration frames ready without discarding current frames. Playback-critical prefetch has priority over speculative jobs.

Transport start or unanticipated seek enters visible priming. The control side waits up to a measured bounded deadline for minimum ring occupancy; if it times out, playback remains stopped/paused and reports the reason without counting an underrun. Once steady-state playback begins, a worker that falls behind changes to an approved lower-quality mode at a bounded decision point and reports the mode. If ready frames still run out, the callback writes silence for the missing interval, increments a bounded underrun counter and sends a compact event for off-thread diagnostics. It never waits or performs fallback DSP itself. Offline export uses separate workers and the highest supported quality.

The engine renders at the project's nominal rate. PipeWire requests that client rate and may convert to a different graph rate; device status reports the graph rate and PipeWire-managed conversion. JACK requires its fixed server rate to match the project or playback remains stopped with instructions. The engine performs no whole-mix resampling. An explicit project-rate-change command preserves elapsed anchors by rational conversion, is undoable, persists the new rate, invalidates rate-dependent cache/prefetch generations and renegotiates the adapter.

## Alternatives considered

- In-callback stretch and resampling could avoid lookahead latency but would require every algorithm and codec path to satisfy strict allocation, blocking and worst-case-time contracts that are not established.
- Fully pre-rendering every edit would simplify playback but delay audition and violate the cache-miss fallback requirement.
- Engine-side whole-mix device-rate conversion would add a real-time path; v1 uses PipeWire-managed conversion when needed and makes JACK mismatch explicit.

## Consequences and evidence

Workers and rings add bounded memory, lead time and generation-cancellation complexity. M2 must demonstrate WAV decode-ahead, priming success/timeout, planned loop-wrap continuity, unanticipated seek stale-frame rejection, callback I/O probes and a steady-state forced worker-stall underrun with silence, exact count and report. Later codec and stretch tests repeat the same contract, including quality step-down and source-rate conversion. The DSP/resampler selection and redistribution review must close before dependent implementation.
