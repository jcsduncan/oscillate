# ADR-0001 — Processor runtime survives topology rebuilds

**Status:** Accepted; implemented (reviewed 20 October 2026). `engine::Graph` reuses a processor runtime whose stable instance id and compatibility key match the prior plan (`TransitionKind::reused`) and replaces the rest off the callback (`src/engine/graph.cpp`).
**Date:** 14 October 2026
**Requirements:** PRD §§7, 15–16; FS-02 ER-08/ER-10; FS-04 PI-08.

## Context

An immutable graph plan is useful for callback safety, but plugin instances, oscillator phase, delay history and reverb tails are mutable and must not reset whenever routing changes. Serializing plugin state is not necessarily equivalent to transferring live DSP history. The graph builder cannot safely duplicate or inspect a running plugin from the audio callback.

## Decision

Immutable topology and buffer plans are separate from mutable `ProcessorRuntime` objects. Every live processor has a stable instance ID and exclusive ownership by the live-render domain. A new graph plan may reference the same runtime for a configuration-compatible topology edit. The old and new plans are never processed concurrently with that shared runtime. Parameters and automation update the existing runtime through bounded events.

An incompatible processor change creates a new runtime off the callback, restores the last committed project/plugin state and validates buses, buffers and latency before publication. The transition manifest names reused, replaced and retiring runtimes. If live DSP history cannot be transferred, the old runtime may drain a bounded tail through a preplanned transition path, or the affected output uses a short controlled fade/mute. The UI reports the selected fallback and any audible limitation. A replacement is not silently described as continuous.

Offline export creates separate runtime instances from the chosen project revision. It never shares a live plugin instance with playback. Persistence stores plugin state and project settings, not transient graph pointers or delay buffers.

## Alternatives considered

- Recreate every processor on each graph build: simple plan ownership, but resets phase and tails and can make saved plugin state sound different from continuous playback.
- Copy live runtime memory into the new graph: unsafe for opaque plugins and mutable state, and would race with rendering.
- Pause playback for every topology edit: avoids concurrent use but interrupts ordinary editing and does not preserve a replaced processor's history.

## Consequences and evidence

Topology-only changes preserve tails and phase without state serialization in the callback. Runtime reuse needs compatibility predicates and strict lifetime tests. Replacement can be audible, so its policy is product-visible. A stateful fixture processor and fixture plugin must prove continuity across graph switches; a replacement fixture proves bounded fallback, notification and no use-after-free under ASan/UBSan.
