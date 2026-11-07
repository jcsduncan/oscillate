# ADR-0061 — Performance tracing

**Status:** Accepted; implemented (tracer, instrumentation, palette commands and report). **Date:** 26 October 2026.
**Builds on:** ADR-0048 (audio worker pool), ADR-0054 (UI update architecture).

## Context

Playback clicked in a four-synth project, and edits made while playing felt sluggish. The output already measured callback time, but nothing showed where a slow block's time went, what the UI thread did between frames, or how long an edit took to become audible. `perf` is blocked on the development machine (`perf_event_paranoid=4`), and ptrace cannot attach to a running process (Yama scope 1). Diagnosing therefore meant timing copies of a project by hand. The musician asked for a proper way to trace all of it.

## Decision

1. **One tracer for every thread** (`oscillate::foundation::trace`, in the foundation library so that every layer can use it). It records three kinds of event:
   - timed spans (`OSCILLATE_TRACE_SPAN(name, number)`, or `trace::span` for a span that opens and closes in different calls);
   - instants;
   - counters.

   Event names are string literals, and only their pointers are stored. A span's number tells instances apart, such as a graph node's id. `trace::label(name, number, text)` gives readable text for one instance, and labels match by text, because a literal's address differs between shared libraries.
2. **Safe on the audio callback.**
   - `trace::start` allocates a pool of per-thread rings: 2^17 events of 32 bytes each, for up to 64 threads.
   - A thread claims its ring with one atomic increment the first time it records. When a ring is full, the oldest events are overwritten, so the newest are kept.
   - Recording takes no lock and allocates nothing. While tracing is off, a span costs one relaxed load.
   - Each write is counted in and out with an atomic writer count. Stopping clears the flag and waits for that count to reach zero, so the rings are never read or freed while a thread is writing to them.
3. **Chrome trace output.** `trace::stop_and_write` writes JSON that ui.perfetto.dev and chrome://tracing open, with thread names and labelled spans.
4. **What is instrumented:**
   - **audio:**
     - `audio.render`, each callback's render;
     - the `audio.load` counter (the callback's share of its block);
     - the `audio.over_budget` and `audio.xrun_recovery` instants;
     - `graph.source`, each track's source;
     - `graph.node`, each mixer node, labelled with its track's name;
     - one span per synth and effect engine, named after the engine;
     - `pool.wait_for_helpers`, the time the callback waits for its workers.
   - **edits:**
     - `edit.committed` when the UI submits a revision;
     - `edit.apply`, `edit.submit`, `edit.submit_revision`, `edit.begin`, `edit.build_and_publish` and `edit.observe`;
     - `edit.audible` when a graph or a parameter offer reaches the callback, and `edit.live_audible` for a value from a drag.
   - **UI:**
     - `ui.busy`, each busy spell of the event loop;
     - `ui.render`, scene-graph rendering;
     - the `ui.frame_interval_ms` counter;
     - the bridge's polls (`ui.poll_playback`, `ui.poll_routed_playback`, `ui.refresh_meters`, `ui.meter_tick`);
     - projection and notification work (`ui.refresh_projection`, `ui.playback_changed`);
     - device panels (`ui.device_refresh`, `ui.device_submit`);
     - `playback.status`.
5. **Recording.** The command palette has "Start a performance trace" and "Stop and save the performance trace", which writes `$XDG_DATA_HOME/oscillate/traces/trace-<time>.json`. Setting `OSCILLATE_TRACE=path` records from launch and writes the file on quit.
6. **Reading.** `tools/trace_report/trace_report.py TRACE.json` reports:
   - audio load and blocks over budget;
   - the spans inside the worst blocks;
   - where the audio time goes, by span;
   - UI busy spells over a frame, with what ran inside them;
   - the time from committing an edit to hearing it, and each edit step's cost.

## Consequences

- Performance work starts from a trace, not a guess. The first one found Nova's pad taking 40% of every 512-frame block by itself.
- Instrumentation sits at function granularity on control paths and at node granularity on the callback. A finer span needs only a macro and a name.
- Tracing costs memory only while recording: rings are allocated by `start` and freed at the next `start`.

## Revision, 6 November 2026: budgeted pool, cheaper writers, one load source

- **Memory.** `trace::start` used to allocate 64 rings of 2^17 events, 256 MiB, whether or not the threads existed. It now allocates one pool of `OSCILLATE_TRACE_BUFFER_MB` (default 32 MiB, about a million events). Threads take 2048-event chunks from the pool as they record, so busy threads get more of it. When the pool is spent, a thread reuses its own oldest chunk. The pool is freed when the recording stops. The trace's `otherData.dropped_events` counts what was overwritten.
- **Writers.** Every write used to increment and decrement one shared `seq_cst` counter. Each thread now enters through its own cache-line guard. Stopping clears the flag and waits for every guard to read zero.
- **Output.** `stop_and_write` streams events to the file one at a time instead of building the JSON in memory.
- **Labels.** `trace::label` does nothing while tracing is off, and a new recording starts with no labels (at most 4096). A graph compiled before the recording started has no track names in the trace until its next build.
- **Instances.** Synth and effect spans are numbered by engine instance (`trace::next_instance`) instead of block size, so two Novas can be told apart.
- **Callback load.** `audio_io::DeviceTelemetry` is the one source of callback timing for PipeWire and JACK. It writes the `audio.load` counter, the `audio.over_budget` and `audio.xrun` instants, and the `CallbackLoad` that the UI meter reads. JACK now records `audio.render` too. Nova's own `cpu_` figure is kept because it measures one engine's share of a block, not the callback.
- **Report.** `trace_report` reads frame times from the `ui.frame_interval_ms` counter. The `ui.frame` span it used to read was never written.
- **Diagnostics.** Callbacks never write text. They publish fixed records to `foundation::realtime_diagnostics()`, a lock-free ring that a full queue drops from and counts. The UI tick drains it into the log (`foundation/log.hpp`, [environment](../environment.md)).
