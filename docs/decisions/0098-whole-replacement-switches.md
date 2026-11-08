# ADR-0098 — Structural edits switch with a whole replacement source

**Status:** Accepted and implemented. **Date:** 8 November 2026.
**Supersedes in part:** [ADR-0094](0094-primed-source-switching.md), where it refuses track, timing, media and instrument changes while playing and holds an engine switch for a note-free boundary; [ADR-0093](0093-live-instrument-edits-and-queued-revisions.md), where it refuses MIDI on a clip with a lane for something other than its instrument.

## Context

After ADR-0093 and ADR-0094, these edits were refused while playing and applied at the next Play: adding or removing tracks, a tempo or meter change, new source media, a change of instrument, and MIDI on a clip with a lane for something other than its instrument. A native engine switch waited up to ten seconds for a note-free boundary, then failed.

Each of them changes what the source bank holds or its layout, and changes several running processors at once. None fits a plan offered to one running player or a graph that keeps its sources.

## Decision

### One switch carries the edit
An edit that changes the track set, the tempo or meter, the source records, an instrument, or a built-in instrument's MIDI beyond its player's lanes is **structural**. `capture_source_edit` captures it with the edited graph's source layout and topological order, and `publish_captured_source` publishes the graph that reads them. The replacement source is primed as in ADR-0094, and the callback adopts graph and source together at the first block the source fills completely. Faster paths (mixer overlays, player plans, effect values, graph-only switches) still take every edit they can.

- `RoutedMixerPublisher::prepare_and_publish_live` takes `new_sources`: a switch that brings its own source bank may add or drop nodes.
- `RoutedPlaybackControl` takes the new layout when the switch is adopted and restores the old one when it is withdrawn.

### Nothing that plays on resets
- **Players** (`KeptPlayer`). A built-in instrument that keeps its patch and note effect layout plays on in the replacement source. When the edit changes what it plays (its notes, its lanes or the tempo), the control prepares its plan and stages it (`NativeSynthPlayer::stage_plan`). The replacement source releases the plan on its first block (`release_staged_plan`), so the player's sounding notes hand over exactly as in ADR-0093.
- **Tempo and meter** are part of each plan: `NativeSynthPlayer` and `NativeEffectStage` read them from the plan, not from a copy taken when they were prepared.
- **Effects.** Every built-in effect stage that runs on across the switch gets its lanes, tempo and meter for the new revision staged (`NativeEffectStage::stage_automation`). It adopts them when the graph activates (`graph_activated`), on the switching block.
- **A replaced instrument rings out.** The old player stays in its track's slot with a staged plan that holds no notes. From the switching block it releases every sounding note and renders for its declared tail, then stops. The new instrument starts with its next note. An engine switch takes this path, so it no longer waits for a note-free boundary, and it works inside a loop.

### Tracks fade in and out
- A node the switch adds fades in over the mixer's gain ramp (`adopt_live_state`).
- A node the switch drops must be silent before the switch can take over. While it is not, the callback mutes it in the active graph with the usual fade (`RoutedMixer::fade_out_dropped`) and renders the old graph. The mute runs on top of any level lane. When the node is silent, the switch takes over. A withdrawn switch unmutes the nodes it faded (`restore_dropped`).

### Still refused while playing
- A change to or from a VST3 instrument, and a MIDI or tempo edit on a VST3 instrument track: the plugin reads its MIDI and tempo once, when it is prepared.
- A tempo or meter change while VST3 effects play, for the same reason.
- A change of VST3 parameter automation, as before.

## Consequences

- `tests/application/routed_live_edit_tests.cpp` plays each structural edit mid-note through a replacement source, as the app does. Each test checks that no block holds, fails or allocates. A track added, a tempo or meter change, MIDI with a level lane, and new media each match a fresh render of the edited song sample for sample. A track added matches it until its first note. An instrument removed matches the song with its sounding note ending at the switch. A removed track fades out, with or without a level lane. A replaced instrument releases its note and the new one plays.
- `tests/ui/quick/routed_playback_tests.cpp` checks that a track removed while playing switches live with the transport free to pause and seek, and that an engine switch, also inside a loop, goes live.
- A structural edit re-streams every audio clip, because the replacement source holds them all. It takes effect a few blocks after the edit, once the source is primed and any dropped track has faded.
