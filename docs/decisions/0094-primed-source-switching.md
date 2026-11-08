# ADR-0094 — Sources switch at a primed block; no held path

**Status:** Accepted and implemented; superseded in part by [ADR-0098](0098-whole-replacement-switches.md). **Date:** 8 November 2026.
**Supersedes in part:** [ADR-0024](0024-live-source-handoff-contract.md), its held source handoff; [ADR-0049](0049-live-graph-switching.md), where the held path remains for source edits, with its `set_live_switching` test knob.

## Context

The held path switched a graph by freezing the transport: the callback rendered silent blocks at a held boundary while the control side built the new graph, then playback resumed. Two kinds of edit still took it:
- **Source edits** outside ADR-0024's source-only overlay: an audio clip fade, a duplicate, an edit in a project with any instrument track, and a native synth engine switch. Each inserted at least one silent block.
- **Every edit the live paths did not take**, such as adding or removing a track or a tempo change. These held the transport and were then refused anyway.

## Decision

### A replacement source switches in at the block it was primed for
- A worker prepares the replacement source and primes its rings for the transport's next block, as before.
- The control side stages it for its revision and publishes the graph that reads it as a live switch (`publish_captured_source`, through `prepare_and_publish_live`).
- The callback fills the staged source *exactly* on the block it tries the switch. If every clip frame is there, the new graph and source take over in that block and the mixer hands its running state over (`adopt_live_state`). If not, the old graph and source render that block and the switch waits.
- A switch the callback has passed over for `live_candidate_passed_block_limit` blocks is withdrawn. The adapter then primes a fresh source at a later block (`restart_captured_source`), at most `max_source_primes` times, before it reports the edit failed.

### Instruments the edit leaves alone keep playing
- The capture lists the running player of every built-in instrument track the edit leaves unchanged (`KeptPlayer`). The replacement source shares that player instead of preparing a new one, so its voices sound on through the switch.
- An exact fill streams every slot's clips before any player renders. A block that fails for a missing clip frame has therefore advanced no shared player, and the old source renders it exactly once.
- Audio clip edits in projects with instrument tracks take this path.
- A native synth engine switch still waits for a note-free boundary. No note may start in the `synth_switch_blocks` after that boundary, the window in which the switch takes over.

### The held path is gone
- `RoutedMixerPublisher` loses `request_hold`, `held_boundary`, `cancel_hold` and `RoutedRenderStatus::holding`. `prepare_and_publish` publishes only the initial graph. A live switch also checks the caller's source layout before adopting a candidate.
- `RoutedPlaybackControl` loses `publish_held`, `publish_held_with_source`, `request_captured_source_hold`, `cancel_captured_source_hold` and `set_live_switching`.
- An edit that no live path takes is refused at once, with its reason (`live_refusal`). The old graph plays on with no silent block, and the edit is saved for the next Play. These edits are:
  - adding or removing tracks, or a reorder that changes the routed source order;
  - a tempo or meter change;
  - new or changed source media;
  - a change of instrument;
  - a MIDI edit on a VST3 instrument track.

## Consequences

- No edit made while playing inserts a silent block or holds the transport.
- `tests/ui/quick/routed_playback_tests.cpp` checks that a fade change switches at its primed block with no held block, and that a refused edit leaves the transport free to pause and seek.
- The refused edits listed above remain to become live transitions.
