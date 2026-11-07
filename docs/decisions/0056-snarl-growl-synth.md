# ADR-0056 — Snarl: a dubstep growl and wobble synth (ninth native engine)

**Status:** Superseded by [ADR-0057](0057-nova-flagship-synth.md): Snarl is not in the code; its growl tables, formant model and rack modules are part of Nova. **Date:** 21 October 2026.
**Builds on:** ADR-0027 (native-synth platform), ADR-0044 (Lumen wavetables), ADR-0031 (Halo FM), ADR-0038 (live switching).

## Context

> Historical record: Snarl was removed when Nova replaced it (ADR-0057). Nothing below is live.

Requirement: a dubstep growl and wobble instrument with a deep preset library: big growls and interesting detail, nothing thin.

Lumen can wobble, but a modern growl comes from a chain that Lumen does not have:
- oscillator-to-oscillator FM, run oversampled so it does not alias;
- heavy distortion;
- a vowel (formant) filter, for the talking "yoy";
- a comb filter for metallic detail;
- an OTT-style multiband squash that pulls every detail forward;
- one tempo-synced wobble LFO that moves all of these together, each by its own amount.

## Decision

**Snarl** is a native engine (id `snarl`), mono-first, built for bass. The first design (a single fixed effects pass with one lockstep wobble LFO over generic tables) was rejected in listening tests as too basic; this is the second.

### Voice (4× oversampled)
| Block | Specification |
|---|---|
| **Osc A, Osc B** | Two equal layers, each with 16 dedicated **growl tables** generated at first use: Growl Saw, Formant Yoi, Formant Square, FM Growl, FM Bark, Resample Grind (FM saw → fold → vowel → clip, a growl "bounced" into a table), CZ Resonance, Hollow Comb, Metal Fold, Bit Grit, Sync Growl, Roar, Talk Box, Diode Saw, Wub Sine, Basic Shapes. Each layer has position, warp, octave/semi/fine, level, and unison 1–7 with detune and spread. All seven units always run, so the unison count fades instead of switching. |
| **FM** | A sine at a ratio of the note (½ to 8, plus √2 and 2.76), with feedback, **or Osc B**, phase-modulates A and/or B by separate amounts. Band limits allow for the modulator's speed. |
| **Noise, voice filter** | Low-passed noise; Ladder 24 / LP / BP / HP / Notch with keytrack. |
| **Sub** | A sine, −1 or −2 octaves. It joins after the rack and the growl's low cut, so the low end stays clean and mono. |

### FX rack (bus, 4× oversampled)
Eight serial slots. Each slot holds any of 17 modules: Tube, Hard Clip, Fold, Rectify, Diode, Crush, Formant (five-formant A–E–I–O–U morph with shift and resonance), Low/Band/High Pass, Notch, Comb (tracks the note), Flanger, Phaser, OTT (three-band LR4, up and down), Dimension (side-only widener above 250 Hz), EQ. Every module has three knobs and a mix. Because the module is chosen per slot, the order is free and stages can repeat (distortion → formant → distortion → OTT …). A module change crossfades over 20 ms. The rack runs on the summed growl; then the growl's low cut, the sub, a clipper and the level.

### Modulation
- Four LFOs, tempo-synced (song-locked or retriggered) or free. Shapes: sine, triangle, saws, square, two **16-step patterns** with glide, random. Each has skew and curve, so one shape covers the wub, yoi and press gestures.
- A synced LFO's rate modulation moves in whole octaves, so it stays on the grid.
- Three envelopes (amp and two for the matrix).
- **Four macros**, and a **16-row matrix**: 17 sources × 58 targets. A parameter target moves by amount × source of its whole knob travel.
- Every preset keeps the macros' roles (Growl = FM, Grit = drive, Speed = LFO 1 in octaves, Metal = comb), so automating macros in a clip changes the sound per hit, as modern dubstep is written.

### Telemetry (for the panel)
LFO values and phases, macros, cutoff, the first OTT slot's band gains, the output peak, and every slot's knobs after modulation.

## Quality bar (tested)
- The platform contracts: switching, pedals, telemetry, export parity (largest difference 0), routed synth.
- Every rack module renders finite at every setting the module sweep covers.
- The formant module's vowels show their first two formants at the table's frequencies (checked by analysis while tuning).
- Panel: every parameter has a control, and no control names an unknown id.

## Presets
21 plus Init, each its own chain, all checked for level (within about 1 dB RMS) and movement by analysis:
- Snarl (yoi growl), Chaos Engine, Yoi Talker, Robot Mouth, Neuro Reese, Riddim Square, Tearout Screech;
- Laser Wob, Vowel Wub, Metal Mouth, Deep Sub Wob, Brostep Saw, Chopper, Grind FM;
- Hydraulic, Gurgle, Bit Muncher, Liquid Wob, Wonk, Dragon, Hollow Riddim.

## Panel
`qml/snarl/`: oscillators, FM, the four macro knobs, the sub, noise and filter, the envelopes, voice and output, and a paged deck (the rack as a left-to-right chain with modulation rings and OTT meters, the matrix, the LFOs with live shape previews, the step patterns).

## Not yet
- Golden fingerprints per preset.
- A table-waveform preview on the panel: it needs a visuals hook for Snarl's own tables.
