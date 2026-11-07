# ADR-0014 — M3 MIDI clips, event timing and sampler boundary

**Status:** Accepted; implemented. The bundled sampler is superseded by Crate ([ADR-0063](0063-crate-chop-sampler.md)) and removed. As built, the event plan fed the bundled sampler, the native synths and VST3 instruments, and since schema 1.32 also carries pedal lanes ([ADR-0035](0035-pedals-in-midi-clips.md)).
**Date:** 14 October 2026
**Requirements:** PRD §§9–10, 13, 17–18; FS-01 TA-03/TA-08/TA-09; FS-04 PI-06; FS-06 AM-07.

## Context

The project currently persists audio clips and renders one WAV feed. The first MIDI/instrument path must let a user draw notes, hear a bundled instrument, save/reopen, and export with the same timing. Project, engine, persistence, sampler and UI work can proceed in parallel only after the note/time/event contract is fixed.

## Decision

Schema 1.2 adds an instrument track kind and MIDI clips without changing the existing audio clip fields. Existing schema 1.0/1.1 tracks load as audio tracks with no MIDI. An instrument track persists a bounded native sampler patch with patch version, managed `SourceId`, root key and level; a missing source keeps the patch and notes as unresolved placeholders. `ClipId` identifies either clip kind and `NoteId` identifies each persistent note. A MIDI clip has an existing musical or elapsed placement, a positive pattern length in timeline ticks, a loop flag and a bounded note list. A note has a local half-open tick interval, key 0–127 and velocity 1–127; v1 uses one instrument channel. Drawn empty clips are valid. Notes have positive length and a nonnegative start. Only the first pattern-length ticks play: a note starting at or after the pattern's end is kept but silent, and one crossing it is cut there (amended with PI-06c, so a shortened clip keeps notes for when it is lengthened again). Duplication creates new persistent clip/note IDs; loop iterations refer to the same notes. Atomic project commands and undo apply to MIDI exactly as to audio. Project model and schema 1.2 writer must integrate as one save-safe change; an older writer explicitly rejects an instrument/MIDI snapshot rather than silently omitting it.

For a musical clip, each note's global tick is clip-start tick plus loop iteration times pattern length plus local note tick. For an elapsed-pinned clip, the clip's exact start sample remains fixed: convert it to an anchor tick through the authoritative tempo map. An event sample is `start_sample + tick_to_sample(anchor_tick + iteration * pattern_ticks + local_boundary) - tick_to_sample(anchor_tick)`, with checked arithmetic. Clip ends crop notes at their half-open boundary and require a synthetic note-off there if a voice is still active. The timeline module alone performs tick/sample conversion; meter changes affect display and snapping but do not silently resize an existing pattern. Revalidate timing after tempo or project-rate changes. Invalid edits, tempo changes or loop counts that yield unrepresentable or zero-sample notes reject before publication.

One scheduler compiles note boundaries into immutable project-sample events for both live playback and offline export. Each block receives events with offsets in `[0, block_frames)`, ordered by offset, note-off before note-on, then stable note ID. An event exactly at a block end belongs to the next block. The first prepared capacity is 1024 events per instrument per block. Plan construction checks the maximum event density in any supported 8192-sample window after each affected tempo/loop revision; this bounds smaller callbacks too. It also checks that at most 1024 notes sound at once on each instrument, so the callback's fixed held-note table can release every note at a loop wrap. Overflow rejects before publication or transport start with a visible error, never in the callback and never by silently dropping notes. Audio-only processors receive an empty MIDI span. A note edit changes the event plan while a compatible sampler runtime retains its live state across graph publication. Offline render creates its own runtime from saved patch state and uses the same event compiler.

The bundled sampler predecodes/pins a managed source and prepares any pitch tables off the callback. Voices, event storage and stereo buffers are fixed-capacity in the callback. The first sampler uses 64 voices per instance with deterministic free/released/oldest-active stealing and reports steals. Transport-generation changes clear sounding voices and invalidate old-generation note-offs. The first M3 slice may omit note chase when seeking into a sustained note and must report that limitation; v1 supplies a control-side note-chase plan for active notes at a seek position. No VST3 or Qt type enters the project, event or sampler interfaces.

[ADR-0025](0025-planned-loop-processor-history.md) refines the runtime reset rule: an ordinary seek changes the processing generation and clears voices; a planned, primed loop wrap changes only source transport generation and retains sounding voices.

Starter sounds live in a separately auditable bundle with creator, source/permission evidence, licence, SHA-256 and transformation record per file. A selected sound is staged into project-managed assets through a source receipt so reopening and export do not depend on the installed application bundle. No sample is labelled royalty-clear solely because it is generated or downloaded; rights evidence is required.

## Alternatives considered

- Encode MIDI notes as audio source ranges: this loses editable note identity and shared timing semantics.
- Let the UI or export calculate event samples independently: tempo ramps and rounding would diverge from playback.
- Allocate event vectors or load samples in the callback: violates the real-time contract.
- Treat a missing starter sound as a reason to delete the clip: violates project integrity and relinking rules.

## Consequences and evidence

The project model and persistence schema must merge together before a MIDI edit is exposed to users. The engine event API must freeze before sampler and export consumers branch. Independent fixtures cover 120-BPM and ramp offsets at supported rates, same-sample ordering, half-open blocks, looping and meter changes, elapsed anchoring, duplicate/undo/reopen, graph reuse through a note edit, deterministic voice stealing, callback allocation probes, and live/offline event-trace equality. Bundle hashes and licence evidence are checked before assets are packaged.
