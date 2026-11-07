# ADR-0078 — Nova arp patterns: lanes of their own length, sixteen modes, a library and MUTATE

**Status:** Accepted; implemented. **Date:** 31 October 2026.
**Builds on:** ADR-0057 (Nova), ADR-0059 (the groove pool), ADR-0073 (Nova Analogue), ADR-0076 (Nova panel), ADR-0077 (Constellation's mutate).
**Panel design:** the `#arp` tab of [ui/creative-tools-mockup.html](../ui/creative-tools-mockup.html).
**Contract:** `include/oscillate/native_dsp/synth/nova/arp.hpp` (modes, lanes, scales, the settings and their constants), `include/oscillate/application/nova_arp_patterns.hpp` (the library, apply, mutate, groove), spec 08a §2.11, §3.5, §4 and §6.

## Context

Nova's arpeggiator had six modes and one sixteen-step pattern of on, velocity, gate and tie. It needed many more options: grooves and patterns. In minimal techno and house a part stays interesting by displacement and drift, not by on-grid eight-bar repeats. The design shows sixteen modes in groups, a library of about sixty patterns, ten lanes plus a Chord lane, each with its own length, a groove from the pool, passing notes, MUTATE with locks and a seed, and a variation every N bars. The open questions were answered on 31 October 2026:

- **Chord per step:** yes. A Chord lane (off, +oct, 5th, triad, 7th, sus) stacks notes over the step's note, diatonic to the arp scale when one is set, otherwise from the held chord.
- **Per-step pitch:** no pitch-bend lane. A tied step slides with Nova's portamento, as before.

## Decision

1. **Lanes, each its own length.** Eleven lanes of up to 32 steps: Note, Octave, Velocity, Gate, Tie, Ratchet, Chance, Condition, Accent, Mod and Chord. Lane `L` reads step `k mod length(L)`, where `k` counts steps from the pattern start (from the chord's first step with Retrigger, else from the song's beat 0). Lanes of 16, 12 and 7 therefore drift and repeat only after their least common multiple (`arp_period`, the panel's REPEATS EVERY readout). The 16 step parameters per lane and the old `repeats` are gone: the lanes replace them.
   - **Rests.** Gate 0, a Euclidean miss, a failed Chance, or a failed Condition.
   - **Conditions.** X:Y plays on the X-th of every Y passes of the condition lane. Fill and Not fill follow `nova.arp.fill` or a variation bar. First and Not first are the first pass.
   - **Ratchets.** 1–4 hits share a step, each with the gate's share. Hits after the first play at 0.8 of the velocity. A tie holds the last hit into the next step.
   - **Accent and Mod.** An accent adds 0.2 to the velocity. Each note carries its step's Mod value and accent to its voice, which publishes them as the new per-voice sources `arp.mod` and `arp.accent` (§4; telemetry 2035–2036). A route from Arp Mod to a filter is a 303 accent lane, drawn.
2. **Sixteen modes,** in the panel's groups:
   - **Order:** Up, Down, Up/Down, Down/Up, Converge (outside in), Diverge (inside out) and Played.
   - **Pedal:** Thumb and Pinky (the lowest or highest note between the others).
   - **Chance:** Random, Shuffle (every note once per cycle, in a seeded affine permutation) and Random walk.
   - **Harmony:** Chord and Strum (spread over `strum` of a hit, alternately up and down).
   - **Drawn:** Shape (the Note lane is the place).
   - **Rhythm:** Euclidean (hits, steps, rotate; Up on the hits only).

   Indices 0–2 keep their old meaning, so the factory presets that use the arp sound as before.
3. **Notes.**
   - **The Note lane** moves the step's note by places in the held notes (Held), or by degrees of the arp scale (Scale; the held chord's tones when the scale is Chord), crossing octaves.
   - **Passing notes** add the scale's tones between the lowest and highest held note.
   - **Scale and root** are the arp's own (`nova.arp.scale`, `nova.arp.root`). They are not taken from the project's key, since a synth patch has no project.
4. **Groove.** The arp's groove is a copy of a groove-pool template in parameters: `nova.arp.groove.steps` (0 is none), `.grid`, and 32 timing and velocity values, plus `nova.arp.groove_amount`.
   - **Why a copy, not a reference by name:** a copy saves with the patch and presets, renders without the pool, and needs no schema. The document label `arp.groove` names it.
   - **Effect on a step:** step n moves by timing × amount of a groove step, and its velocity scales by 1 + (v − 1) × amount. Swing and groove together are held to at most 0.24 of a step early and 0.75 late, so steps keep their order.
5. **Variation every N bars** (Off, 2, 4, 8, 16). The last bar of every N bars of the song plays as a fill, and reads the Note and Octave lanes 1–3 steps on, seeded by the bar. FILL also plays the fill and is an automatable parameter (`nova.arp.fill`, on at ≥ 0.5), so the arrangement can draw fills.
6. **Determinism.** Every random choice is a stateless hash of `nova.arp.seed`, a salt and the step: Random, Shuffle, the walk, Chance and the variation offset.
   - **Random walk** restarts every 64 steps, so any step's place is found from the phrase start. A cache makes the usual next step O(1).
   - **Step plans.** A step's notes (a ratchet's hits, a strum's notes) are planned when the step starts and played as events, which may fall in later blocks; all of them land before the next step.
   - **Result:** any block split, a seek or an export gives the same notes. This is tested over every mode with every lane at work.
7. **The library (application `nova_arp`).**
   - **The patterns.** Sixty factory patterns are data, written compactly per lane (a character a step for levels and toggles, words for numbers and choices), in eight categories: Minimal, Deep House, Techno, Acid, Dub Chords, Trance Gate, Classic and Polymetric. They are written to be displaced, polymetric and with gaps.
   - **Applying one.** `apply` sets the arp's mode, rate, gate, swing, octaves, note mode, passing notes, strum, Euclidean rhythm, variation and every unlocked lane, turns the arp on, and clears Fill and the seed. The scale and root, latch, retrigger, velocity mode and groove stay. Nothing outside `nova.arp.*` changes.
   - **MUTATE.** `mutate` varies a base by a seed and an amount: levels nudged, toggles flipped, notes and octaves stepped, now and then a ratchet, a condition or a chord, and lane lengths moved by one or two so the lanes drift. Locked lanes are left alone. Seed 0 is the base.
8. **Qt.** `NovaArp` is a child of the device controller, exposed as `controller.arp` (Nova only).
   - **What it offers:** the patterns, categories, modes (with their panel groups) and lane specs, the locks, and the mutation seed and amount.
   - **Calls:** `apply_pattern`, `mutate`, `set_groove` (a `NoteToolsController.grooves` entry), `clear_groove`, `period`, `period_beats` and `euclid_steps`.
   - **Edits.** Each change is one `apply_state` edit (one undo step) of values and the `arp.pattern` and `arp.groove` labels.
   - **What MUTATE starts from:** the sound before the first mutation, or, after the user has edited since, the sound now.
9. **Panel (ARP · CLIP page).**
   - **ARPEGGIATOR:** the grouped modes, knobs, velocity mode, RETRIG, LATCH, VARY and FILL.
   - **HARMONY · RHYTHM:** note mode, scale, root, PASSING, Euclidean knobs with a ring, and the GROOVE list from the host's groove pool (`NovaPanel.grooves`).
   - **PATTERNS:** the library. As in the preset browser, the list keeps the keys so the typing keyboard plays the held chord: ↑/↓ load, "/" searches, Escape returns, and the keys pill shows which.
   - **STEP LANES:** paint across steps (one undo step a drag), click toggles and choices, double-click resets. Each lane has a lock and a length stepper, with the playhead at k mod length. Below them: the period readout and MUTATE with ‹ seed › and amount.
   - **The clip sequencer** sits under them.

## Consequences

- **Table size.** Nova's table grows from 1139 to 1515 parameters, within the limit of 2048: 363 lane parameters and 66 for the groove replace the 65 step and repeat parameters. Telemetry grows to 2038 floats. Offset 79 now counts pattern steps instead of 0..15.
- **Saved arp patterns change.** Saved projects that used the old steps (`nova.arp.step.*`, `nova.arp.repeats`) lose them (no legacy, per the project's rule). Saved modes 3–5 (Played, Random, Chord) now name Down/Up, Converge and Diverge.
- **Cost.** The arp's settings are about 4 KB, copied on each control tick. The engine reads lane values directly; nothing allocates on the callback.
- **What other tools can drive.** `nova.arp.fill` is automatable, so an arrangement (or a later Arrange from a loop pass, ADR-0080) can draw fills. Patterns can be found by name through the library.
