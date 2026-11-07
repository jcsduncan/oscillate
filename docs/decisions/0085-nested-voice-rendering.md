# ADR-0085 — One heavy synth's voices on the free audio helpers

**Status:** Accepted; implemented. **Date:** 2 November 2026.
**Builds on:** ADR-0048 (audio worker pool), ADR-0061 (performance tracing), ADR-0062 (denormals flushed).

## Context

ADR-0048 renders every track's source as one task and every graph level in parallel, so a block costs its heaviest single chain. Its consequences named the next lever: one heavy instrument needs parallelism of its own.

That is now the common case. Chord clips with pad layers hold five- and six-note chords on Nova tracks, and long releases overlap the next chord, so a pad track sounds ten or more voices. A dense test project (chord clips with Nova pad layers, 1024-frame buffer, 21.3 ms deadline) showed it:

- One Nova pad track (Classic unison, noise through a Formant and a Ladder filter, chorus and reverb) took 7.1 ms of an average block and up to 16.6 ms of the worst. The other sources had finished long before; the callback waited on that one task.
- 19 of 1593 blocks overran the deadline, the worst by 31% (28 ms).

## Decision

### Nested batches in the worker pool
`AudioWorkerPool::run_nested(count, task, context)` lets a task, or the dispatching thread, hand out a batch of its own.

- **Slots.** Up to `nested_slots` (4) nested batches run at once, each in a slot with the same packed claim word, completion count and lanes as the outer batch. A dispatcher takes a free slot with an acquire compare-and-swap on a busy mask and releases it after its batch finishes.
- **Helpers.** Publishing a nested batch bumps the same futex word as an outer one and wakes as many helpers as it can use. A worker with nothing left of the outer batch claims from the outer batch, then from each nested slot.
- **Waiting.** A dispatcher works on its own batch first. While it spins for helpers, it helps other nested batches but never takes an outer task, because a whole track would hold up its own batch's caller. Waiting is bounded as before: a spin, then a futex sleep.
- **No help.** With no attached worker, or no free slot, the batch runs inline in task order on lane 0, so callers need no second path.
- **Lanes.** Lanes are unique within the nested batch and below both its count and `max_lanes` (every worker plus the dispatching thread).

### Synths may spread their own work
- `SynthKernel::use_workers(pool)` is called before each block.
- `NativeSynthRuntime::use_workers` and `NativeSynthPlayer::render(…, workers)` pass the pool from the source bank, live (`RoutedAudioSource::fill_slot`) and in export (`offline_master`).
- Engines that do all their work alone ignore it.

### Nova renders its sounding voices apart
- **When.** At each render (at most one control tick, 32 frames), with two or more sounding voices and attached helpers.
- **How.** Each voice is one nested task. It renders into a buffer of its own, using its lane's `VoiceScratch`.
- **Summing.** Each buffer starts at −0, the additive identity of float addition, so it holds exactly what the voice would have added to a shared buffer. The buffers are then added into the output in voice order. Every frame sums the same values in the same order as rendering the voices one after another, so the result is bit-identical whichever helper took which voice. The kernel's control, note allocation, global modulation and effect rack stay serial, between the renders.

## Measured

The same test project, timed through `RoutedPlayback::prepare_and_start` with a timing output paced like a 1024-frame device, 7 helpers, on an i7-10700K. The full 34 s mix is bit-identical before and after.

| | Before | After this ADR alone |
|---|---|---|
| Mean block | 14.6 ms (69%) | 10.4 ms (49%) |
| p99 block | 20.7 ms | 12.8 ms |
| Worst block | 28.0 ms (131%) | 17.5 ms (82%) |
| Blocks over 21.3 ms | 11–19 of 1593 | 0 |
| The pad track's source, mean (worst) | 7.1 ms (16.6 ms) | 3.8 ms (5.6 ms) |

The worst blocks are now set by a bass track's chain (its Nova source, then a Forge saturator insert, about 5 ms), not by one synth's voices.

## Consequences

- **Critical path.** A synth with many voices no longer sets the block's critical path on its own while other cores are free. When every core is busy, the voices run on the synth's own thread as before.
- **Isolation rule (ADR-0048) refined.** A voice may run on any thread from one tick to the next, but never on two at once. A voice's render touches only its own state, its lane's scratch and read-only tables. Anything shared across voices stays in the kernel's serial part.
- **Overhead.** Each tick costs a publish, at most one futex wake per helper, and a summing pass over the voice buffers. Each Nova instance holds 16 voice scratch areas (about 320 KB) and a 32-frame buffer per voice slot.
- **Open.** Other engines with many voices (Prism, Concert's string bank) can follow the same pattern. Cost-ordered claims (ADR-0048) would still help when there are more tracks than cores.
