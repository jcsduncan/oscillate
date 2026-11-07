# ADR-0008 — Atomic project commands and revision publication

**Status:** Accepted; implemented. `ProjectModel` applies validated commands as atomic revisions with undo/redo (`src/project/model.cpp`).
**Date:** 14 October 2026
**Requirements:** PRD §§8–10, 18; FS-01 TA-01/03/04/06/07/09/10; FS-03 PM-03; HLD §4.

## Context

Arrangement clips can use musical or elapsed anchors. A tempo, meter, timebase or project-rate edit can change rendered sample intervals even when no clip is dragged. An accepted command must never publish a state containing same-track overlap, partial compound changes or stale asynchronous results. Saved project revision and audible graph revision remain distinct.

## Decision

`project` uses typed foundation IDs for projects, tracks, clips and sources. A monotone checked 64-bit `RevisionId` identifies each accepted model state; it is separate from persistent entity IDs. The project snapshot contains the nominal sample rate, immutable tempo/meter maps, tracks, clips, source references and revision. Commands produce a private candidate snapshot, validate the entire affected state with the shared `timeline` evaluator, and publish one immutable snapshot and change set only on success. Failure leaves revision, undo depth and published snapshot unchanged.

An unchanged routing destination, clip placement, clip source range or project
sample rate is rejected as a no-op; it creates no undo entry and does not clear
redo history.

A clip occupies a half-open interval `[start,end)` in project samples. Musical endpoints convert through the candidate timing map and project rate; elapsed endpoints are already project sample coordinates. Zero/negative intervals and same-track overlap reject atomically, while touching edges are valid. Map or rate changes revalidate all clips, including tracks mixing anchor modes, and report the conflicting stable IDs. Source-media ranges are independent of arrangement placement.

Typed commands cover track/clip edits, timing-map edits, timebase/rate changes and atomic compounds. Successful undo/redo also publish new monotone revisions and restore exact saved fields. A gesture ID coalesces successive edits into one undo step; a new ordinary command clears redo. Undo history remains session-scoped. A change set carries affected IDs and dirty categories as a hint; the immutable snapshot is authoritative. An asynchronous result carries origin revision, target ID, source identity and settings generation; stale completion is rejected without a revision.

The `application` control sequencer serializes transport transitions with project-rate edits. It supplies an authoritative stopped-transport gate through a project-owned interface so `project` does not depend on `engine`; a caller-provided boolean is insufficient. Playback `pending/active/failed` state is an application read model keyed by model and engine revisions, not persisted project truth.

## Alternatives considered

- Mutate the shared model in place and roll back on error: observers could see partial changes and rollback might miss associated state.
- Check overlaps only for directly edited clips: tempo/rate changes can make other clips overlap.
- Store active audio-graph status in the project snapshot: mixes durable intent with transient engine availability and would make save/reopen misleading.
- Reverse the rate-conversion formula during undo: rounding loses original elapsed-anchor coordinates.

## Consequences and evidence

Candidate validation costs grow with affected tracks; indexes and incremental checks may be added only after a correct full oracle exists. G2 fixtures cover adjacent/mixed-anchor clips, tempo/rate-induced overlap, atomic compound failure, exact rate-change undo, gesture coalescing, redo invalidation, stale async rejection and retained snapshot lifetime. The timing API from G1 must merge before consumers implement these conversions.
