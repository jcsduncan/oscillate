# ADR-0058 — Pump: a drawn volume shaper, and documents for effects

**Status:** Accepted; platform, engine, MIDI trigger and panel built. Superseded in part by [ADR-0094](0094-mute-lanes-gate-keys.md) where it makes `routing::keys_now` the whole test for a sounding key: a key source's mute lane also gates its keys. **Date:** 25 October 2026.
**Builds on:** ADR-0040 (native effect platform), ADR-0041 (Pocket), ADR-0043 (one native insert path), ADR-0057 (structured synth state).
**Panel design:** [ui/pump-panel-mockup.html](../ui/pump-panel-mockup.html).

## Context

Requirement: a Kickstart-class volume shaper whose duck is drawn as a curve against the kick's waveform, because shaping attack and release with knobs is slow and indirect. Pocket already ducks bass around a kick, and Press compresses from a key. Neither lets the user draw the duck as a shape against the kick.

A drawn shape is a curve, and an effect's state has been only its dense parameter table. Nova's document (ADR-0057) already holds curves for synths.

Sharp attacks and releases must never click, and that protection belongs inside Pump, not in a separate stage.

## Decision

1. **Effects carry a document.**
   - `NativeEffectPatch` gains `document`, the same `NativeSynthDocument` that synths use, with the same model checks. Effects take no media.
   - `EffectEngine` gains `validate_document`, `prepare_document` and `preset_document`. `EffectKernel` gains `adopt_document`.
   - `NativeEffectRuntime` compiles and offers documents through a lock-free mailbox, and adopts them at a block start before the values with the same revision, as synths do.
   - A document edit to a running stage goes live without a rebuild.
   - The session has `native_effect_document`, `set_native_effect_document` (undoable, gestures coalesce) and `set_native_effect_preset`. Value edits keep the document, and presets bring theirs.
   - Schema 1.41: the effect payload gains a `document` field. Older projects are not read (no legacy).
2. **Pump (id `pump`)**: one curve, `shape`, the volume over one cycle (y = 1 is untouched). An empty document plays Classic Pump.
   - **Triggers:**
     - *Tempo* restarts the cycle on the song grid: 1/16 to 2 bars, straight, dotted or triplet.
     - *Kick Audio* restarts it on each kick detected in the sidechain, or in the input when there is no key. A lookahead of 0 to 20 ms (a latency) puts the duck on the kick.
     - *MIDI* restarts it on each note-on of the tracks keying Pump (the same sidechain send Kick Audio listens to). The notes come from the project's compiled MIDI plan, so the shape is read ahead: the duck lands on the note with no lookahead and no latency.
     - Both keyed triggers hear only a key that sounds (`routing::keys_now`): its send is live with a gain above zero, and its track is not muted or silenced by another track's solo, by the mixer's own rule. A solo that keeps the keyed track keeps its key, so a soloed bass still ducks to its kick. A silent key carries no audio (its edge gain is zero) and no notes.
   - **Depth, Nudge (±20 ms), Mix and Output.**
   - **Band:** Full, or Lows Only below a Linkwitz–Riley crossover (40–400 Hz). The low and high bands sum to an allpass, so with no duck only phase changes.
   - **Anti-click, built in.** The gain runs through two cascaded one-poles: every drawn edge becomes an S-curve whose 10–90 % rise is the Anti-click time (0.5–10 ms, default 2 ms). The shape is read early by the smoother's own delay (2τ), so a drawn edge lands where it was drawn.
     - Measured: a 40 Hz sine through Hard Gate keeps its worst 5 ms of energy above 1 kHz at −67 dB with the default, against −57 dB at the minimum.
   - **Telemetry for the panel:**
     - the cycle position as heard;
     - the gain now;
     - the cycle length;
     - the key state;
     - the last cycle of the key (or input) as 256 peaks, aligned to the output;
     - the applied curve: the shape after depth, mix and anti-click, as it sounds.
3. **Key notes for effects.**
   - An `EffectEngine` with `key_notes` gets the note-on frames of the tracks keying its slot whose keys sound. They are sorted and unique, and notes a clip boundary restarts are left out.
   - `NativeEffectRuntime::offer_key_notes` hands them over through a lock-free mailbox. The kernel adopts them at a block start (`adopt_key_notes`) and is told each chunk's project frame (`locate`).
   - `NativeEffectStage` compiles the notes when it is prepared. Every graph publish refreshes them on stages it keeps, and so do a mute, solo or key gain edit, so editing the kick track's notes or muting it while playing moves the duck without rebuilding Pump.
4. **The panel's grid follows the meter.** Telemetry reports the cycle's length in beats of the song's meter.
5. **Triggers that work on a drum bus.**
   - *Kick Audio* listens only between Detect Low and Detect High (24 dB/octave each side, 30–160 Hz by default), so hats and snares in a keyed drum bus never restart the cycle. *Listen* plays what the detector hears.
   - The band filters delay what they pass. A full-band detector (15 ms hold) times each onset; the band-limited detector confirms it was a kick within 30 ms; the cycle starts at the onset. The kick test keeps its 1 ms accuracy with the filters in.
   - Key notes carry their key and velocity (`KeyNote`). *MIDI* listens only to notes between Note Low and Note High (the kick of a drum rack), and *Velocity* scales the depth by the note's velocity.
   - *Length: Kick Spacing* stretches each cycle to the gap until the next trigger. MIDI knows the next note; Kick Audio predicts from the last gap. Gaps under a quarter or over four times the rate's cycle fall back to the rate (a break is not a beat).
   - The MIDI trigger walks the notes with cursors that pass each note once; a seek, new notes or a new range rebuilds them, so live playback, a seek and export render alike.
6. **Shapes, bands and stereo.**
   - *Slots:* up to four curves, `shape` (A) and `shape.b` to `shape.d`. *Slot* plays one, steps through a *Pattern* of 1–8 cycles (each step names a slot), or *Morph*s A into B (automatable). A missing slot follows A.
   - *Three Band* splits at the Crossover and the High Crossover (Linkwitz–Riley; the lows pass the high crossover's allpass so the bands sum flat). The mids and highs take their own depth and, optionally, their own curves (`<slot>.mid`, `<slot>.high`), which otherwise follow the slot's.
   - *Stereo* pumps both channels, the mid only (the width stays) or the side only. Per band it is a 2×2 matrix; with both applied it is the plain gain, bit for bit.
   - *Swing* (Tempo, 50–75 %) starts the second cycle of each pair late, and the shape stretches to fit.
   - Telemetry adds the last cycle of the input and output as heard (the before and after), the last keying note (for Learn), the pattern step and slot playing, and the mid and high gains.
   - Cost: 0.7 % of a core on the effects bench, with every band and stereo path always running so a switch only fades.

## Workflow (schema 1.42)

1. **Linked instances.** `NativeEffectPatch` gains `link`: empty, or a native identifier that effects of one engine share. Linked effects share their document; values stay per instance.
   - The session edits the group together. A document edit, a factory preset's shape or a user preset's shape on one member reaches every member in the same command, one revision and one undo step. A preset's values stay with the instance it was loaded on.
   - `link_native_effects` founds a group (the first member's document copied to all). The link is the founding slot's id and the founding revision, so it stays unique after an undo. `unlink_native_effect` leaves a group; the last member left is unlinked with it. `native_effect_link_count` is 1 alone.
   - The model only checks that a link is a native identifier and counts it in `same_settings`. The session keeps the link on every value, document and preset edit.
   - Live: an edit that changes one slot still uses the parameter and document mailbox. A group edit changes several slots in one revision, so it takes the graph republish path, which reaches every member.
2. **Duck to a kick in one gesture.** `duck_tracks_to(source, targets)` is one command. On each target (never the key itself), it reuses the first Pump on the chain or appends a new one. It keys every member from the source alone, replacing other keys, and links them all to the first reused Pump's shape (else Classic Pump). New slots are named in advance from `command_insert_slot_id`, so the sends can target them in the same apply. In the Quick app, a track's context menu offers "Duck tracks to this…", a checklist of the other tracks (returns unticked) that calls `RouteCommands::duck_tracks_to`.
3. **User presets.** A built-in effect's state saves as a preset file, `$XDG_DATA_HOME/oscillate/presets/<engine>/<name>.json` (`~/.local/share` when unset).
   - The file holds the engine, its sparse values and the document, in the project codec's own encoding (`persistence::encode_native_effect_preset`). Generation and link belong to a slot, so presets carry neither.
   - Loading one is a single command, `set_native_effect_state`: values and document together, the document reaching the linked group. The engine resolves the file as it would a saved slot, so a preset for another effect, or a malformed one, is refused.
   - The panel can save, list, load, delete, export to any file and import (an import is loaded, then kept with the user's presets). A loaded user preset names the panel (category "User") until something else is loaded.
   - The device controller also carries the system clipboard (to copy shapes between panels), the link count and Unlink.

## Consequences

- Any effect can now hold drawn state: envelopes, LFO shapes, transfer curves.
- Schema 1.42 writes `link` on every built-in effect, empty when unlinked. Greenfield: only 1.42 loads.
- Pump's sound depends on tempo when its values do not. The platform's constant-lane test now renders both sides with the same transport.
