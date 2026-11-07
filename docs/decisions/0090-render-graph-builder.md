# ADR-0090 — Export renders a graph the render_graph builder prepares

**Status:** Accepted and implemented, VST3 instruments and effects included ([#100](https://github.com/jcsduncan/oscillate/issues/100)). **Date:** 7 November 2026.
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
- VST3 stages come from a `Vst3StagePreparer` in the request, which the plugin host supplies (`src/application/vst3/offline_export.cpp`), as the live control's `Vst3Preparer` comes from its provider. A VST3 effect on any track, bus or the master is then a stage like a built-in one, a VST3 instrument is its track chain's first stage, and each failure probe joins the fault check. Without a preparer, a saved VST3 stage fails preflight. Neither `render_graph` nor `export` depends on VST3 code.
- `OfflineInstrument` holds a built-in instrument's `NativeSynthPlayer`, the player live playback runs.
- `held_non_finite` is the guard for a stage that held a non-finite frame.

Its routed mixer's nodes come from `routing::graph_nodes`, which live playback's `RoutedPlaybackControl` builds its nodes from too: each track's chain, mute and channel gain. The two differ only by its options:
- Live playback runs the master chain and master level on the master node, and adds its chain switches' drains and handoffs.
- An offline render runs the master chain and level after the mix, and mutes tracks nobody can hear so it skips their work.

`export` asks `render_graph` for the graph. It reads clips, runs blocks and writes. It links `render_graph`, and `native_dsp` only for the loudness and true-peak meters. Its stopped master, stem and encoded renders take the preparer and pass it on.

## Consequences

- `export` links only what the HLD table allows; the module table gains `render_graph`.
- The code moved as it was, so offline renders are bit-identical.
- A project with VST3 instruments or effects exports through the same render as any other (`render_vst3_master`, `render_vst3_stems`, `render_vst3_stem`). Export runs 1,024-frame blocks and each VST3 stage takes them in 128-frame chunks at their project positions, as live playback at 128 frames feeds it, so `application_vst3_export_parity` nulls exactly against live playback. The former VST3 effect and instrument exporters, with their own render loops and shape limits, are gone.
- A VST3 render's length follows the native rule: the latest note or clip end plus the longest path tail. It can end a few frames of silence later than the former exporters' per-source sums.
