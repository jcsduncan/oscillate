# ADR-0009 — Durable source identity and stereo render context for M2

**Status:** Accepted; implemented: durable source records (`project::SourceRecord`) and a stereo render context whose `engine::BlockContext` carries the project sample position and transport generation.
**Date:** 14 October 2026
**Requirements:** PRD §§7–9, 18, 21; FS-02 ER-05/ER-11; FS-03 PM-01–PM-05; ADR-0004/0006/0008.

## Context

The first engine, media and storage modules are individually testable, but a saved clip currently contains only a source ID. The project cannot locate its audio after reopening. The render graph accepts mono samples without a project position or transport generation, so it cannot place a clip or distinguish a seek from continuous playback. M2 must prove those links before adding wider production features.

## Decision

`project::Snapshot` gains bounded source records keyed by stable `SourceId`. A record describes the encoded original by an encoding discriminator, lowercase SHA-256 digest, byte length, sample rate, channel count and frame count. It contains no user path. An atomic project command registers a record; clip commands may share that transaction. Clips whose source ID has no record remain intact as unresolved media placeholders. Removing a record does not silently remove clips.

`persistence` stages a managed WAV off the audio thread through its held project-root descriptor. It copies an opened input into a temporary file under `assets/`, computes the digest while copying, validates WAV metadata from the pinned staged file, flushes the file, publishes to a digest-derived asset name without replacing an existing entry, then flushes `assets/`. A store-issued, move-only receipt is available only after this durable publication and exposes the verified source record. Existing digest names are deduplicated only after a no-follow regular-file and content check. An interrupted stage may leave an orphan, never a project reference. The store keeps the receipt identity private; a client-constructed source record is not proof of staging.

The next manifest writer uses schema 1.1 and serializes bounded source records canonically. The reader explicitly accepts existing strict schema 1.0 as an empty source registry, preserving its clips as unresolved; it never rewrites an old generation during load. `save` requires a matching same-store receipt for a newly referenced managed source, or an identical record already validated in a prior committed generation. Existing assets receive bounded no-follow type and byte-length checks on save, without hashing all media on every edit. Full hash validation occurs during staging/deduplication and through an explicit integrity audit. Loading a valid manifest with missing or damaged media returns the project and source-specific availability diagnostics rather than dropping edits.

The engine render contract uses stereo frames and passes a block context containing the first project sample and a nonzero transport generation to each processor. The application owns play/seek/loop position and advances the generation at discontinuities; the engine does not infer transport state. Frame counts and half-open sample intervals are checked before a graph switch. Invalid or overflowing context produces bounded silence and leaves a pending graph unpublished. The audio adapter copies stereo frames to its device buffer without aliasing unrelated storage. Its observed block size informs control-side graph preparation; an undersized pending graph remains pending while the previous graph continues.

[ADR-0025](0025-planned-loop-processor-history.md) later separates source transport generation from processor history generation for a primed, explicitly marked loop wrap. Ordinary seeks retain this decision's reset behaviour.

## Alternatives considered

- Keeping only paths in clips would make source identity unstable and embed machine-specific locations in project intent.
- Reopening a staged file by pathname would permit a path swap between copy and validation.
- Rehashing every managed asset on every save would make routine edits scale with media size.
- Continuing a mono graph through M2 would require replacing the processor contract immediately afterward and could conceal channel-loss bugs.

## Consequences and evidence

This extends the project schema and the engine processor interface before the first audible application. Each module implements and tests its contract independently; consumers and top-level CMake are updated once the contracts are in place. M2 must import a small same-rate WAV, remove the external original, render distinct left/right samples at the correct clip placement and source offset, survive a live graph edit and save/reopen, and preserve the prior complete generation after an interrupted save. A failed graph build must leave the saved/model revision visibly different from the active render revision. Cold-start and seek priming are distinct from counted steady-state underruns. Source-rate conversion and additional codecs remain later work.
