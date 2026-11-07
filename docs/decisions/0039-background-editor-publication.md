# ADR-0039 — Background publication of routine editor changes

**Status:** Accepted. **Date:** 19 October 2026. **Amends:** ADR-0004 (writer capability and publication scheduling).

## Context

Routine native synth and VST3 parameter commits called `ProjectSession::save()` on the UI thread. A durable save writes and flushes a full project generation. Storage stalls therefore froze a panel after a gesture, even though its model and audio edit had already been accepted. The same issue applies to native effect controls. An autosave alone is insufficient because it advances `AUTOSAVE`, not the project's explicit `CURRENT` state.

## Decision

- A routine editor commit accepts one undoable model revision and requests publication of `CURRENT` after a short coalescing delay. The delay never holds an audio or UI lock.
- The UI thread captures an immutable snapshot and a narrowly scoped, duplicated writer capability. The worker performs serialization, validation, writes, flushes and pointer publication. It cannot publish `AUTOSAVE` or stage media.
- The worker shares the store's nonblocking save gate with explicit saves, autosave and media publication. A competing save reports a conflict; the editor coordinator retries only transient conflicts. The store rejects any explicit revision older than the current pointer, so an older worker cannot regress a newer save.
- A worker completion marks only its captured revision durable. An edit accepted later remains dirty until a later publication. The UI names accepted model intent and disk durability separately; failure retains the edit and displays an error.
- Routine worker saves admit only projects whose referenced managed media and external links are already in a saved generation. Import, relink, repair, collect, named revisions and migrations keep their verified publication paths. Project switching and normal close wait for any active editor worker before changing ownership.
- Live DSP adoption may use the accepted immutable model revision before its disk publication finishes. This allows a control to respond immediately. The UI reports the pending save separately from whether the audio callback applied the value.

## Rejected alternatives

- Moving routine edits to the existing autosave worker would leave `CURRENT` unchanged and force recovery selection on ordinary reopen.
- Posting a delayed synchronous save to the Qt event loop would merely move the freeze to a later frame.
- Running `ProjectSession::save()` itself on a worker would race mutable receipts, external proof, the model and project ownership. The worker receives only an immutable snapshot and a restricted store capability.

## Verification

Focused tests cover native panel edits during an older captured save, latest revision reopen, refusal to regress a newer `CURRENT`, and VST3 manual state publication. The Quick VST3 editor contract covers accepted, pending, live and reopen status. Storage fault and interrupted-write tests for ADR-0004 remain applicable to the same `ProjectStore::save()` protocol.
