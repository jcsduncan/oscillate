# ADR-0031 — Halo: six-operator FM synth (fifth native engine)

**Status:** Accepted; implemented. **Date:** 18 October 2026.
**Builds on:** ADR-0027 (the native-synth platform), and ADR-0028 to ADR-0030.

## Goal

Halo is a classic-inspired six-operator FM synth for e-pianos, bells, mallets, FM basses, brass and glassy pads. Like every native engine, it adds no project-format, session, playback or export changes.

**Non-goals for v1:** DX7 SysEx import, per-operator LFO amplitude sensitivity, MPE, and any ROM patch data. All 16 presets are original.

**Amended 19 October 2026:** the DX 4-rate/4-level envelope model and true cross-operator feedback loops, first non-goals, are now in. See *Envelopes* and *Algorithm topologies* below.

## Voice

| Block | v1 specification |
|---|---|
| Operators ×6 | Ratio (0.5, 1–31), fine ratio, detune (±10 c), or a fixed frequency (1 Hz–10 kHz). Output level, velocity sensitivity and key-level scaling. Waveform: sine, half-sine, abs-sine or quarter-sine. Each operator has its own envelope: four rates and four levels on the chip's 0–99 scales, with keyboard rate scaling (0–7). |
| Algorithms | 32 classic routings, with feedback (0–1) on the algorithm's marked operator. |
| Filter (optional) | Off, LP 12, LP 24, BP 12, HP 12 or ladder, reusing the Prism filters. Cutoff, resonance, key tracking and velocity. |
| LFO | Sine, triangle, saw, square or S&H. Rate, delay, and depths to pitch and to carrier level. |
| Voicing | 16 notes, each with 1–4 unison voices (detune and spread). Poly, mono or legato, with glide and bend range. Wheel, bend and pressure are automatable performance parameters. |
| Effects | Chorus (I / II / I+II), ensemble, and a BBD delay (synced or free), with output level and the safety knee. |

## Quality bar (tested)

- **Sidebands.** A sine carrier modulated by a sine at index β has sideband n at J_n(β), within 0.2 dB, checked against `std::cyl_bessel_j`.
- **Pitch.** Within 0.5 cent at every rate.
- **Feedback.** Output stays bounded at every feedback amount.
- **Aliasing.** The FM core runs 2× oversampled. The E-Piano at C6 must keep its inharmonic floor below −70 dBFS at 44.1 kHz.
- **CPU.** At most 10 % of one core at 48 kHz with 64-frame blocks, for 16 notes × 2 unison with all six operators.
- **Inherited contracts:** no allocation in the callback, the 32-frame grid, block-split invariance, declicked steals and seeks, and export equal to live playback.

## Presets

16 original presets with golden fingerprints: E-Piano, Bright EP, Bells, Tubular, Marimba, FM Bass, Slap Bass, Brass, Strings, Organ, Clav, Harp, Glass Pad, Metallic Lead, Sitar-ish and Init.

## UI

A retro-digital 80s faceplate: a dark brown/black body, a membrane-button algorithm selector with a drawn routing diagram, an operator card per operator with an envelope sketch, an LCD-style readout, and teal and amber accents.

## Design notes

- **Algorithm topologies.** The 32 topologies follow the widely published DX-style algorithm charts. In every algorithm a modulator has a higher operator number than its target, so operators evaluate from 6 down to 1. Algorithms 4 and 6 close true loops, as the charts draw them: op 4 (algorithm 4) and op 5 (algorithm 6) feed back into op 6, one frame late, through the same two-sample average as self-feedback. Every other algorithm feeds back on itself. Halo is not a bit-exact emulation.
- **Operator tables.** Operators read 4,097-point tables (sine and its three variants) with linear interpolation, about −130 dB of error. A polynomial sine would cost too much at 16 × 2 × 6 operators, 2× oversampled.
- **Feedback.** Feedback averages the operator's last two outputs, as the DX does, which keeps high feedback stable.
- **Modulation depth.** An operator's full output shifts its target's phase by 2 cycles (index ≈ 4π). Operator levels use a skewed control, so low levels are fine-grained.

## Implementation decisions (bea0e43)

- **Next-note parameters.** Algorithm, filter type, unison count and voice mode apply from the next note. A sounding note keeps its routing, so changing the algorithm never clicks mid-note. The status chip reports the deferral.
- **Fixed-point phase.** Operators use a 32-bit wrapping phase, where 2^32 is one cycle. Its top 12 bits index the table, so reads need no floor and no wrap branch. Modulator levels and feedback depth are scaled into these fixed units at control rate, keeping the scaling off the serial operator chain.
- **Kernel per routing.** The 32 routings are compile-time data (`fm_algorithm_data`). The operator section is a template on algorithm and unison count, chosen at note start: only real connections are summed, and accumulators stay in registers.
  - Unison lanes and both oversampled frames are evaluated interleaved, because only the feedback operator links one frame to the next.
  - This took the budget case from 29.9 % to 8.4 % of a core (`docs/performance/halo-synth.md`).
- **One decimator.** All voices sum at the oversampled rate into one shared stereo half-band decimator. Stolen voices are declicked before it, so the fade is band-limited too.
- **Envelopes.** Each envelope (`DxEnvelope`) follows the documented DX7 behaviour:
  - **Run order.** Note on runs L4 → L1 at R1, then L1 → L2 at R2, then L2 → L3 at R3, and holds L3 while the key is down. Note off runs to L4 at R4 from wherever the envelope is.
  - **Speed.** Rates are speeds, not times. Falling segments are linear in decibels. Rising segments jump to about −50 dB and then speed up the quieter they are.
  - **Units.** Levels move in 0.75 dB steps. Timing is against the chip's 44.1 kHz reference, so a segment takes the same time at any sample rate.
  - **End level.** A sounding L4 holds after release, as on the hardware.
  - **Fractions.** Whole-number rates and levels are the chip's exact values. Fractions interpolate between them, so the knobs and automation glide.
  - **Rendering.** Envelopes run at the base rate and are interpolated linearly to the oversampled rate.
  - **Feedback depth** follows the envelope of the operator whose output is fed back, as on the classic chips.
- **Preset conversion.** The 16 presets were converted from their original ADSR settings:
  - **attacks** are matched by mean amplitude over the old attack time, because a DX attack is a convex swell and the old curve was concave;
  - **sustains** take the nearest DX level;
  - **decays and releases** are matched at the old curve's −30 dB point.

  Their goldens were rebaselined. Hold levels are within about 2 dB of the originals.
- **Voice retirement.** A voice retires only on the 32-frame control grid, once its carrier envelopes are idle and, with the filter on, its output is below −120 dBFS. Retiring on the grid keeps resonant filter tails block-split invariant.
- **Additions beyond the PRD.**
  - A global **Brightness** control (±1) scales every modulator by up to 2^±1.5. Pressure adds to it.
  - The LFO can sync to tempo, like Ember's.
- **Levels.** Carriers are normalised by 1/√(carrier count). A modulator at full level shifts its target by 2 cycles.

## Measured

- **Aliasing.** Bright EP at C6, 44.1 kHz, has an inharmonic floor of −96 dBFS (target below −70).
- **Sidebands.** Through the whole voice they match J_n(β) within 0.2 dB.
- **CPU.** 8.4 % of a core for 16 notes × 2 unison (budget 10 %). The ladder filter adds about 4 %.
