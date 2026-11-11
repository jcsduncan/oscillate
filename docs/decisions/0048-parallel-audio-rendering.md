# ADR-0048 — Parallel real-time rendering

**Status:** Superseded in part by ADR-0110 (default worker count) and ADR-0127 (the live graph's schedule); otherwise accepted and implemented. **Date:** 20 October 2026.

## Context

Live playback rendered every block on the device callback thread alone, in two phases:
- **Sources:** the source bank rendered every track's clips, sampler and synth one after another.
- **Graph:** the routed mixer then processed every node in topological order: the incoming sends with their delay compensation, the inserts, gain, pan, mute and meters.

The callback's period therefore had to cover the sum of every instrument and every effect. On a sixteen-thread workstation, one core did all the work while the rest sat idle.

Two other facts shaped the design:
- **Bit-exact parity.** Export must equal live playback exactly (largest difference 0), so parallel rendering cannot change summing order or any processor's input.
- **Real-time priority.** The development account has no `RLIMIT_RTPRIO`. PipeWire and JACK get real-time priority for the callback through RTKit or their own servers, so ordinary `pthread_setschedparam` cannot promote helper threads.

## Decision

### An audio worker pool beside the callback
`engine::AudioWorkerPool` runs batches of independent tasks for the one dispatching thread.

- **Dispatch.** `run(count, task, context)` publishes a batch with one release store of a packed claim word: batch number, task count and next task. The callback and the workers claim tasks with an acquire compare-and-swap on that word. A claim is tied to its batch, so a slow worker can never take a task from a later batch with an earlier batch's function.
- **Completion.** Each finished task counts itself with a release increment. `run()` returns only after an acquire load sees them all, so every write a task made is visible to the callback and to whichever thread touches the same state in the next batch.
- **Waiting.** Waiting is bounded: spin briefly, then sleep on a futex. Only as many sleepers are woken as the batch can use. RTKit kills a process whose real-time threads run too long without sleeping, so nothing spins unbounded.
- **Lanes.** Each participating thread takes a lane, unique within the batch and smaller than the task count, for per-thread scratch.
- **No workers.** With no attached worker the batch runs inline, in task order. Every caller therefore works unchanged with a null or idle pool, and tests compare the two paths.
- **Allocation.** Nothing is allocated on the audio path. Workers and their lists are created when playback starts.

### What runs in parallel
- **Sources:** one task per source slot, which is a track's clips followed by its sampler and synth, in the order they always mixed. Slots share nothing: each has its own scratch, prefetch ring consumers, event buffers and instrument runtime.
- **The graph:** in dependency levels. At preparation, each node is placed one level after its deepest source, sends and sidechains included. A level's nodes run in parallel, and the next level starts after they finish. A level of one node, normally the master, runs inline.
  - **What a node task writes:** only its own buffer and sidechain buffer, its incoming edges (the destination owns the edge's delay line and gain ramp), its histories, tail, processors, gain ramp and meter slot.
  - **Scratch:** the mixer's single scratch buffer becomes one lane per thread that can share the widest level.
- **Summing order:** within a node, inputs are always summed in the same order whichever thread runs it. Parallel output is therefore bit-identical to the single-thread render.

### Who starts the workers
A render source carries an optional pool (`StereoRenderSource::workers`). The output that owns the callback starts `wanted_workers()` threads on `run_worker()` while it streams, at the callback's priority:
- **PipeWire:** threads come from the stream context's `SPA_TYPE_INTERFACE_ThreadUtils` (module-rt: RTKit or the realtime portal) and are promoted with `acquire_rt(thread, -1)`, the data loop's own priority.
- **JACK:** `jack_client_create_thread` at the client's real-time priority, only when the server runs real-time.

A worker that cannot be made real-time is not kept, because the callback would otherwise wait on a thread the desktop can preempt. Stopping joins the workers before the stream or client is destroyed. A callback still in flight then finishes its batch inline.

### Configuration and reporting
- **Default worker count:** one fewer than the physical cores (hardware threads ÷ 2), at most 15. `OSCILLATE_AUDIO_WORKERS` overrides it, and 0 restores single-thread rendering.
- **Reporting:** `OutputStatus::realtime_workers` reports how many workers run. The top bar's CPU readout shows the thread count while playing (for example "CPU ×8").
- **Realtime export:** it runs through the same playback pool.

## Measured

One second of eight built-in engines, one per track, in the Debug tree:

| Workers | Render time |
|---|---|
| 0 (callback alone) | about 175 ms |
| 1 | about 117 ms |
| 3 | about 100 ms |
| 7 | about 100–112 ms |

- **The floor is the heaviest instrument.** One instrument is one task, and Concert alone takes about 75 ms of that second.
- **Correctness:** the parallel render is bit-identical with 1, 3 and 7 workers, and ThreadSanitizer reports nothing for the pool, the mixer and the full multi-engine source path.

## Consequences

- **Load:** a session's callback load is now bounded by its heaviest single chain, not by the sum of all of them.
- **Isolation rule:** a processor or instrument may run on a different thread from block to block, but never on two threads at once. New callback-side state must belong to one slot or one node, or be atomic.
- **Next lever: cost-ordered claims.** Start each block with the slots and nodes that took longest in the previous one, so the heaviest chain starts first when there are more tasks than threads. A single heavy instrument would need internal parallelism of its own (for example Concert's string bank). ADR-0085 adds nested batches for that, and Nova renders its voices on them.
- **Offline export** still renders on one thread; `start_owned_threads()` is ready for it.

## Addendum (2026-11-05): export as a pipeline

Export already ran each block's instruments, and each dependency level of the mixer, on the worker pool. But a level's nodes had to finish before the next level started, and a bus or the master path often sits alone in its level. A heavy reference project (a full house arrangement with a drum bus, a reverb return and a mastered master chain) spent 62 of its 93 s export with one thread working: the Drum Bus chain, the reverb return and the master chain, block after block.

- **Silent tracks** are no longer rendered in export.
  - A silent track is one that's muted or solo-silenced, or whose every route leads only to such tracks (`routing::silent_track_nodes`).
  - A track whose sidechain key sounds still renders, since the key taps it before its fader.
  - The mixer skips a muted node only with the opt-in `skip_muted_nodes` limit, so live playback keeps rendering muted tracks and unmuting stays seamless.
- **The mixer pipelines** (`prepare_pipeline` / `mix_step`, offline only).
  - Each node keeps one block slot per dependency level. In every step, all nodes run at once, and level L works on the block fed L steps earlier.
  - A node still sums the same inputs, in the same order and in its own block order, so the output is bit-identical. It arrives depth − 1 blocks later, and the caller drains it with silent blocks.
  - The caller's next-block instruments run in the same parallel batch, using the mixer's copy of the current inputs.
- **Mix Check's per-track taps** still use `mix_block`, because a tap reads a node's output for the block just mixed.

**Measured** on that reference project, 94 s of audio, 16 cores:
- export went from 95 s to 41 s;
- the output matches the earlier render to the bit;
- the Drum Bus chain (about 25 s on its own) is now the floor, followed by the master chain, which still runs after each emitted block.
