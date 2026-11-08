# ADR-0101 — VST3 instruments and effects follow live edits

**Status:** Accepted and implemented. **Date:** 8 November 2026.
**Supersedes in part:** [ADR-0098](0098-whole-replacement-switches.md), where it refuses VST3 instrument changes, MIDI and tempo edits on VST3 instrument tracks, and tempo changes while VST3 effects play; [ADR-0005](0005-delay-compensation-transitions.md), its open gate for VST3 latency-changing transitions.

## Context

A VST3 instrument read its MIDI, tempo and parameter lanes once, when it was prepared. So did a VST3 effect, for its tempo. A MIDI edit on a VST3 instrument track, a tempo or meter change with VST3 plugins running, and a change to or from a VST3 instrument were refused while playing and applied at the next Play. A replaced VST3 instrument would also have kept playing its own MIDI while it rang out.

ADR-0005 left latency-changing VST3 replacements as an open gate. Since ADR-0049 and ADR-0050, a VST3 stage with changed state is prepared as a new instance and switches live. The mixer's latency handover moves delay compensation with it.

## Decision

### A VST3 instrument plays from a plan
The instrument runtime renders from one plan per saved revision: its compiled MIDI, the tempo with its block boundaries, and its parameter lanes. The plan comes through a `PlanMailbox`, as a built-in synth's does (ADR-0093).
- **Held notes hand over.** The runtime tracks held notes with `engine::HeldNotes`. On a new plan's first block, a held note the plan no longer holds is released at offset 0, and one it still holds plays on to its new end. Each note keeps one VST3 note number for the instrument's whole life, so its note-off always matches its note-on.
- **Pedals follow** as in ADR-0093.
- **Bounds.** A plan may not need more lanes than the instrument's event budget was sized for. It may not bring a track's first pedals either. Either edit fails visibly.

`PreparedTrackInsert` carries `offer_events` and `applied_events_revision`. A MIDI edit confined to a VST3 instrument track's notes, pedals and clip placement goes to the running instrument as a plan for its next block, in the same edit as any built-in players it changes. When a clip carries a lane, the edit is structural (ADR-0098): the plan is staged and adopted when the switching graph activates.

### Tempo and meter changes
A structural switch stages a new plan, with the new tempo, for every VST3 instrument that plays on. A VST3 effect is prepared afresh (`ChainContext::fresh_vst3_effects`), and its predecessor rings out on its drain beside it. Lookahead and delay compensation hand over as for any changed stage (ADR-0049).

### A replaced VST3 instrument releases its notes
A graph that no longer runs a VST3 instrument deactivates it and does not activate it again. On its drain, the instrument then plays an empty plan: it releases every held note on its first block and plays no more. A change to or from a VST3 instrument is a structural edit (ADR-0098).

### Latency changes
A VST3 effect whose new state changes its latency is a new instance. It switches live, and the parallel paths' delay compensation moves to the new latency. A running plugin that requests a latency restart from its editor applies the change when the editor commits its state.

### Still refused while playing
A change of VST3 parameter automation lanes, as before.

## Consequences

- `tests/application/vst3/instrument_provider_tests.cpp` moves a held note on a running VST3 instrument while playing. The note is released on the next block and starts again at its new place, with no allocation. A VST3 instrument removed while playing releases its note on the switching block and rings out only its declared tail.
- `tests/application/vst3/routed_track_insert_tests.cpp` changes a VST3 effect's latency from 7 to 96 frames while playing. No block holds, fails or allocates, and the wet and dry paths meet again at 96 frames.
- `tests/track_chain/track_chain_tests.cpp` checks that a retimed chain prepares its VST3 effects afresh and keeps its instrument.
- A VST3 effect replaced for a tempo change restarts its own state (filters, envelopes); its old instance's tail rings out beside it.
