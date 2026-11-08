# ADR-0094 — A key source's mute lane gates its keys

**Status:** Accepted; implemented. **Date:** 8 November 2026.
**Supersedes in part:** [ADR-0092](0092-native-inserts-read-key-audibility.md), where it says `key_note_onsets` filters notes by `keys_now` alone and that `native_inserts` uses nothing else from `routing`; [ADR-0058](0058-pump-volume-shaper.md), where it makes `routing::keys_now` the whole test for a sounding key.

## Context

The mixer ignores a track's saved mute when a channel-mute lane drives it: the lane mutes the channel through its node gain, sample by sample. `routing::silent_key_sources` follows the saved mute and solo only, so a key source muted by its lane still keyed its sidechain effects ([#248](https://github.com/jcsduncan/oscillate/issues/248)). Two keys need the lane:

- The audio key is tapped before the source's fader, so the channel's mute gain never reached it.
- The key notes (Pump's MIDI trigger, Dust, Flyby) are compiled ahead of playback on the control side.

The key notes could follow the lane in two ways: evaluate the lane at each note-on when the notes are compiled, or gate each onset by the lane on the callback. The second puts automation evaluation into every keyed effect's callback, and the effect stage would have to hold the source track's prepared lanes. The first uses the same prepared lane the mixer evaluates, at the onset's project sample. Live playback and export both compile the notes through `NativeEffectStage::refresh_key_notes`, and every lane edit republishes and refreshes them, so the result is exact in both.

## Decision

- A channel-mute lane mutes its channel where its value is at least `routing::mute_lane_threshold`. The mixer's node gain, the audio key and the key notes use this one test, `routing::mute_lane_mutes`.
- **Audio key.** `PreparedRouteGainBlocks` binds each sidechain edge whose source has a mute lane. The edge's gain is its send-gain lane, or else its saved gain, multiplied by the source channel's mute gain, with the same short glide. The mute gain is taken as many frames back as the edge delays the key, so the key is silent on exactly the source audio the channel's mute silences, in live playback and export.
- **Key notes.** `routing::KeyMuteLanes` prepares the mute lanes of the tracks that key an effect. `key_note_onsets` drops each note-on where its track's lane mutes the channel. A revision with no mute lane on a key source prepares nothing.
- **Bounce.** Bounce into a new track mutes the source. Its saved mute does not silence a source that has a mute lane, so that source keeps its sends, and its keys now follow its lane. The bounced copy gets the same lane but no sidechain sends, so each hit keys once. Where the lane mutes the source, its key notes stop at the lane's step and its audio key fades out over the same glide as the channel. Flatten replaces the source, so its copy takes the sends and the lane.

## Consequences

- One lane decides a source's audio, its audio key and its key notes, and live playback and export evaluate it the same way. `keys_now` alone no longer says whether a key sounds: a key it accepts may still be muted by its source's lane.
- A sidechain edge bound to a mute lane takes the guarded graph path for a send-gain edit, like an edge with a send-gain lane. A live gain drag on such a key is heard once the edit is saved.
- `native_inserts` uses `routing::KeyMuteLanes` and `routing::keys_now`.
