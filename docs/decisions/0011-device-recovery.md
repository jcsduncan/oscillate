# ADR-0011 — Device loss and in-process reselection

**Status:** Accepted for C1 control contract; live reselection proof remains open
**Date:** 14 October 2026  
**Requirements:** PRD §§5, 7, 19, 21; FS-02 ER-05/ER-11; ADR-0006.

## Context

M2 can stop on loss of a selected PipeWire sink, but restarting the player is not the user-facing recovery path. Reopening an adapter must not restart from an earlier cursor, advance time while silent, reuse prefetched frames from an old device epoch or discard a saved project edit. JACK has a fixed server rate and different selection identity from PipeWire.

## Decision

The application owns the recovery sequence and project transport position. A common device status reports backend, requested project/client rate, observed graph/server rate, actual buffer quantum, lifecycle, available xrun evidence and an opaque selected-target identity. Backend-specific serials or names do not enter project state. PipeWire may report graph-rate conversion; JACK rejects a server/project rate mismatch before starting playback and reports both rates.

On a loss event, a callback-safe playback gate closes. The adapter is quiesced and joined before its resources are retired. The application captures the last **completed** project block position and transport generation, keeps the project model and active graph intact, and invalidates the old prefetch feed generation. A user or policy selection opens a new target, prepares any block-size-dependent render resources off the callback, primes the new feed, and only then reopens the gate. The first resumed block begins at the saved completed position. A failed or superseded selection remains stopped with a visible reason; a late event from an older selection epoch cannot reopen the gate. No adapter callback mutates project state or waits for recovery work.

The status distinguishes an unavailable backend xrun count from a measured zero. Device loss, source underrun and render failure remain separate diagnostics. A rate change uses the existing stopped-transport lease and invalidates rate-dependent prefetch/cache state before reopening.

## Alternatives considered

- Retrying solely inside the adapter could race with application transport and graph preparation.
- Saving only the most recently requested cursor position can skip or repeat a block when loss occurs during a callback.
- Reusing the old prefetch generation can play frames prepared for the wrong seek, loop or rate.

## Consequences and evidence

The transport needs a control-safe completed-block snapshot and bounded lifecycle epochs. Fake device/clock/worker tests cover loss during playback, failed and stale reselections, no cursor movement while stopped, ring invalidation and unchanged model/graph revisions. A disposable PipeWire target test removes and reselects within one process. JACK dummy-server tests cover matching rate, mismatch, 64/128-frame buffers, xrun and shutdown notifications. Physical-device and reference-workload evidence remain release qualification work.
