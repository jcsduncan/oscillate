# ADR-0093 — Live MIDI edits and queued revisions

**Status:** Accepted and implemented. **Date:** 8 November 2026.
**Supersedes in part:** [ADR-0050](0050-one-chain-per-track.md), where it lists MIDI changes as not yet covered while playing; [ADR-0024](0024-live-source-handoff-contract.md), where it leaves MIDI changes without a preparation rule.

## Context

A MIDI note or clip edit made while playing changed the track's MIDI clips, which the routed control counted as source content. The edit took the held path: the callback froze the transport for a silent block, then graph publication refused it ("Routed live source content changed; prepare and restart playback"). The edit played only from the next Play. Every later edit compared against the old audible revision, so a mute after a refused MIDI edit was refused too.

An edit offered while an earlier one was still switching in was refused ("Previous routed graph update must activate first"), so rapid edits failed one after another.

## Decision

### A built-in instrument's MIDI is part of its player's plan
`NativeSynthPlayer` renders from one plan per saved revision: the track's compiled notes and pedals (`engine::InstrumentEvents`), its synth and note-effect lanes, and its note effects' values and switches. The plan travels through the player's `PlanMailbox`, so the control thread builds it and the callback adopts it whole at a block start, allocating nothing. `offer_plan` replaces `offer_automation`.

On the first block of a new plan:
- **Sounding notes hand over.** `HeldNotes::copy_block` takes the new plan's `engine::NoteSpans`, an index of each note's interval built with the plan. A held note the new plan still holds (same clip, note and key, on before the block and not yet off) keeps its voice and ends at its new note-off. Any other held note is released at offset 0.
- **Pedals follow.** `InstrumentEvents::copy_pedals` sends at offset 0 every pedal whose value at the block differs between the two plans, so a removed pedal comes up.
- **No chasing.** A note the edit adds or moves under the playhead starts at its next note-on, as in other DAWs.

### The control offers instrument edits to the running players
`RoutedPlaybackControl::try_queue_live_players` takes an edit whose changes, on any number of built-in instrument tracks, are confined to what a plan carries: MIDI clips (notes, pedals, placement, clip lanes), player lanes (synth parameters, macros, note effect parameters), note effect values and switches, and macro values. A clip lane must be a player lane. The note effect layout must match the running player. Each changed track's player gets the plan; the revision is audible once every player has rendered it. An edit to an instrument track with no instrument changes nothing audible and is active at once. An edit that leaves everything audible unchanged (an undone edit, a mute then an unmute) is active at once.

### Edits queue onto the newest revision
An edit offered while a graph switch, a player offer or a VST3 parameter is still settling is accepted and deferred, not refused. A full mailbox defers too. `observe()` applies the model's newest revision once the earlier switch has settled, so a burst of edits coalesces onto its last revision. A pending offer of the same kind is still superseded in place.

## Consequences

- Note add, move and delete, MIDI clip add, duplicate, move and delete, and mutes switch at one block boundary with no hold, no silent block and no click. `tests/application/routed_live_edit_tests.cpp` checks each against a fresh render of the edited song, sample for sample.
- A plan copies its track's events; an edit re-compiles the project's MIDI on the control thread.
- MIDI edits on a VST3 instrument track, and on a track whose MIDI clips carry a lane for anything other than its player, still take the graph path.
