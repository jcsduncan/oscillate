# ADR-0096 — MIDI clips keep notes trimmed off their left edge

**Status:** Accepted and implemented. **Date:** 8 November 2026.

## Context

A MIDI clip's notes and pedal points count from its start, and it plays the first `pattern_length` ticks of them. Its right edge already trims without loss: notes at or after the pattern's end are kept, silent, so lengthening the clip again restores them (PI-06c). Its left edge did nothing. Trimming a clip's start, as in Live, moves the start later or earlier while every note stays where it sounds on the timeline, so the notes before the new start are hidden, and dragging the edge back reveals them.

That needs the clip to hold content before its start. Two models do it: a content offset field (the tick of the content at the clip's start, as an audio clip's source range holds), or note positions allowed below zero. An offset field changes every reader of note positions, from playback to the piano roll, harmony and Consolidate, and adds a persisted field, which would need a schema version; the codec reads only the current schema, so every saved project would stop opening. Positions below zero mirror the existing rule for the right edge, need no new field, and leave readers that already skip silent notes correct once they skip these too.

## Decision

- A MIDI note may start before tick 0, and a pedal point may sit before tick 0. Validation accepts them; a note still ends after it starts, and a lane's points stay strictly ascending.
- Such a note is kept but silent, like a note at or after the pattern's end. `project::note_plays(clip, note)` is the one test: `0 <= start < pattern_length`. Playback, model validation, Consolidate, Quantise, Arrange from loop, Clay's MIDI walk and the native synth's clip window use it.
- Pedal points before tick 0 are silent too, but the last of them holds its value from each repeat's start until the first point inside the pattern, unless that point is at tick 0. A sustain held across the trimmed edge stays down.
- `MidiClipService::trim_midi_clip_start` moves the start, keeps the end, and shifts every note and pedal point and the pattern length by the same ticks in one undo step, so the pattern's end and every note keep their timeline places. A looping clip's start stays inside its first pattern. Clip automation stays with the clip's start, as it does when an audio clip is trimmed.
- The arrangement and the piano roll do not show notes before tick 0, so they cannot be selected or edited until the edge is dragged back over them.
- The saved format is unchanged: note starts and pedal positions were already signed. No schema version changes.

## Consequences

- The left edge of a MIDI clip trims on the grid (Alt for free) and plays live, through the MIDI clip path of ADR-0093.
- A reader that walks a clip's notes must skip notes before tick 0 as it skips notes past the pattern's end.
