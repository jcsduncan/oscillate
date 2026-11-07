# ADR-0090 — Export renders a graph the render_graph builder prepares

**Status:** Accepted and implemented for built-in chains and instruments; the VST3 offline renderers still prepare their own ([#100](https://github.com/jcsduncan/oscillate/issues/100)). **Date:** 7 November 2026.
**Supersedes in part:** [ADR-0043](0043-one-native-insert-path.md), where it has `export` link `native_inserts`.

## Context

Offline export prepared its whole graph in `offline_master.cpp`:
- which saved inserts it admits;
- the project automation;
- each track chain, through `track_chain` (ADR-0050) and `native_inserts` (ADR-0043);
- the routed mixer, its gains, solo isolation and tails;
- the master chain and saved convolution impulse responses;
- the built-in synth players.

So `export` linked `track_chain`, `native_inserts`, the synth engines, `automation_project` and `convolution_ir`, past its place in the HLD's module table ([#134](https://github.com/jcsduncan/oscillate/issues/134)).

## Decision

A module of its own, `oscillate::render_graph` (`src/render_graph`), prepares the graph an offline render runs:
- `preflight_inserts` admits the saved inserts.
- `prepare_offline_automation` prepares the project's automation once per snapshot, for every stem.
- `prepare_offline_graph` prepares the track chains through `track_chain`, the routed mixer, the master chain and convolution impulse responses. It bounds every tail.
- `OfflineInstrument` holds a built-in instrument's `NativeSynthPlayer`, the player live playback runs.
- `held_non_finite` is the guard for a stage that held a non-finite frame.

`export` asks `render_graph` for the graph. It reads clips, runs blocks and writes. It links `render_graph`, and `native_dsp` only for the loudness and true-peak meters.

## Consequences

- `export` links only what the HLD table allows; the module table gains `render_graph`.
- The code moved as it was, so offline renders are bit-identical.
- The VST3 offline renderers in `src/application/vst3` include `render_graph`'s `preflight_inserts`; they move onto the builder under #100.
