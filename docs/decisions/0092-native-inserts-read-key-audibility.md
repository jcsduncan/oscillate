# ADR-0092 — Native inserts read which keys sound from routing

**Status:** Superseded by [ADR-0094](0094-mute-lanes-gate-keys.md) where it says `key_note_onsets` filters notes by `keys_now` alone and that `native_inserts` uses nothing else from `routing`: a key source's mute lane also gates its keys. The rest is accepted and implemented. **Date:** 7 November 2026.
**Supersedes in part:** [ADR-0043](0043-one-native-insert-path.md), where it lists what `native_inserts` depends on.

## Context

A built-in effect that reads its keying tracks' notes (Pump's MIDI trigger, Dust's duck, Flyby; ADR-0058) gets them from `NativeEffectStage::refresh_key_notes` in `native_inserts`. A key must sound only from a track you can hear ([#245](https://github.com/jcsduncan/oscillate/issues/245)), with one exception: soloing the keyed track keeps its keys, so a soloed bass still ducks to its kick, as it did before #245. Whether a track can be heard is the mixer's mute and solo rule, which `routing` owns. The audio key's edge gain follows the same rule there. A second copy of the rule in `native_inserts` could disagree with the mixer, and then the MIDI and audio keys would disagree too.

## Decision

`native_inserts` links `routing` privately and asks it which keys sound:
- `routing::silent_key_sources(snapshot)` names the tracks whose keys are silent: muted tracks, and tracks soloed away unless the solo keeps a track they key.
- `routing::keys_now(snapshot, silent, source, send)` says whether one sidechain send keys its effect now.

`key_note_onsets` takes notes only from sends that `keys_now` accepts. `native_inserts` uses nothing else from `routing`. `routing` does not depend on `native_inserts`, so the link adds no cycle.

## Consequences

- One rule decides both the audio key and the key notes, live and in export.
- The HLD module table and `cmake/module_dependencies.json` list `routing` among `native_inserts`' dependencies.
