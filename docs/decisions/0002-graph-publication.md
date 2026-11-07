# ADR-0002 — Prepared graph publication and reclamation

**Status:** Accepted; implemented. `GraphPublisher` publishes prepared plans at a block boundary and reclaims retired plans off the callback (`src/engine/graph.cpp`); the routed mixer follows [ADR-0018](0018-routed-mixer-publication.md).
**Date:** 14 October 2026
**Requirements:** PRD §§7, 18, 21; FS-02 ER-01/ER-06/ER-09.

## Context

Model edits can complete faster than a safe render graph can be built. The callback must not wait for construction or destruction. A failed build must not make the model or playback appear inconsistent without explanation.

## Decision

The control side validates and accepts a typed project command as model revision `M`. The graph builder prepares topology, buffers, runtime reuse/replacement manifest and PDC plan for `M` without mutating the active graph. A bounded publication slot presents only a complete ready plan. The callback acquires it at a block boundary, publishes active graph revision `G=M`, and releases the prior plan after its final block. Retirement and destruction run on a non-RT thread after an epoch/hazard-style reader proof; exact mechanism is chosen in the first engine contract PR and must not block or allocate in the callback.

The application tracks each engine-affecting revision as `pending`, `active` or `failed`. If preparation fails, the intended model edit remains saved and undoable while playback stays on the prior valid graph. UI status shows both revision IDs, the failing command, reason and retry/undo. Export of model revision `M` must build `M` independently or fail; it cannot render `G` while labelling output as `M`. A pending build superseded by a newer revision may be cancelled, but no stale plan may become active.

Control-side metadata derived from a prepared graph, including sampler diagnostics, processor slot identity history and the exact project snapshot used to build it, is staged with that publication. The control side installs it only after observing callback activation of the matching revision. A superseded or rejected plan cannot change the metadata of the audible graph.

## Alternatives considered

- Lock a mutable graph during edits: callback lock acquisition can block and violates the real-time contract.
- Unconditionally swap a lock-free pointer and immediately delete the old graph: reclamation can race with a callback already using the old plan.
- Commit to general RCU now: possible, but concrete reader, grace-period and memory bounds remain to be proven; the chosen publication contract permits a measured epoch or hazard implementation without promising an unverified mechanism.

## Consequences and evidence

Model and playback can intentionally diverge, so the status is part of the user experience and diagnostics. Tests force graph-build failure and rapid successive edits, verify no stale activation, show truthful UI state, verify that superseded processor slots and pending sampler diagnostics do not claim active status, and exercise repeated publication/reclamation under sanitizers and callback allocation/blocking probes.
