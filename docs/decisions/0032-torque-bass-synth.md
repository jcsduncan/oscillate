# ADR-0032 — Torque: multi-model mono bass synth (sixth native engine)

**Status:** Accepted; implemented.
**Date:** 19 October 2026.
**Builds on:** ADR-0027 (the native-synth platform), and ADR-0028 to ADR-0031.

## Goal

Torque is the instant, heavy bass instrument. One click switches between four classic bass characters. It sounds great by default, and its accent and slide behave honestly.

Like every native engine, Torque adds no project-format, session, playback or export changes. Its sequencer pattern is stored as ordinary parameters.

**Non-goals for v1:**
- polyphony or paraphony;
- MPE;
- sample playback;
- pattern import (such as x0xb0x);
- song mode (chaining patterns).

## Voice

The voice is mono with last-note priority.

- **Slide.** A note that overlaps the previous one glides to its pitch with an exponential slide: a constant time, as on a 303, adjustable. A note that does not overlap retriggers.
- **Accent.** A note is accented when its velocity is at least the threshold (default 100). Accent raises:
  - the note's level;
  - the filter envelope's depth;
  - the **accent sweep**, a 303-style capacitor charged by the filter envelope on accented notes. A run of accents opens the filter progressively wider.
- **Model.** A next-note choice that selects the oscillator, filter topology and drive character.

| Model | Oscillator | Filter | Character |
|---|---|---|---|
| **Acid** | Saw, or a 303-style square with tilted tops | New four-stage **diode ladder**, whose resonance thins the bass | Accent sweep; short decay by default |
| **Moog** | Three oscillators (saw, triangle, square) with octave and detune, plus noise | `ZdfLadder` with drive | Separate amp envelope; keytracking |
| **Reese** | 2–7 detuned saws with spread, plus a sub | `MultimodeFilter` (LP 24 or BP 12), then a `CombFilter` notch | LFO on detune and the comb; heavy drive |
| **Sub/808** | Sine sub with a pitch drop (`ResonantBody`), plus a fifth or octave layer | LP 12 | Saturation adds harmonics that small speakers can play; long decay; glide |

**Shared controls.** Cutoff, resonance, env mod, decay, accent, drive, tune, slide time and level keep stable ids across every model. The panel relabels a control where its meaning differs by model. Each model also has its own extra controls.

## Sequencer

The sequencer is built in and optional.

**Pattern.** Up to 16 steps, each with:
- a note (±12 semitones);
- an octave (−1, 0 or +1);
- a gate (off, on or tie);
- accent;
- slide.

**Settings:** length 1–16; rate 1/8, 1/16, 1/16T or 1/32; swing 0–75 %; gate length.

**Storage.** Every step setting is an ordinary discrete parameter, so a pattern is saved with the patch.

**Running it.**
- **Held note.** Holding a MIDI note runs the pattern, rooted on the held note. A later held note transposes it without restarting. Releasing every note stops it.
- **Timing.** Steps are timed from the note-on, counting integer frames and using the tempo. So:
  - a clip note on the grid puts every step on the grid;
  - any block split renders the same audio;
  - export equals live playback;
  - export length stays bounded by the MIDI notes.

**Sequencer off** (most presets): MIDI notes play directly. Velocity sets accent, and overlapping notes slide.

## Effects

In this order:
1. **Distortion:** Tube, Fuzz or Crush, with tone and mix.
2. **Chorus:** Off by default; suits Reese.
3. **Delay:** BBD, synced or free.

Then the output level and the safety knee.

## Quality bar (tested)

- **Diode ladder.**
  - **Small-signal response.** It equals the bilinear-mapped response of the linear state-space model below, within 0.5 dB up to 0.4 fs. The oracle is evaluated analytically at jΩ.
  - **Self-oscillation.** It starts at the analytic feedback threshold, at the analytic frequency, within 25 cents.
- **Slide.** The pitch trajectory follows the exponential slide law within 5 cents.
- **Accent sweep.** Over consecutive accented notes, the peak sweep rises monotonically, and it relaxes once the accents stop.
- **Sequencer.**
  - Step onsets land exactly on the frames computed from tempo, rate and swing.
  - Transpose follows the held note.
  - A tie holds the gate, and a slide glides.
  - Any block split renders the same audio, and export equals live playback.
- **Pitch.** Within 0.5 cent in every model.
- **Sub/808.** The pitch drop follows the `ResonantBody` analytic sweep.
- **Inherited contracts.** No allocation in the callback, declicked retriggers and seeks, and the tail bound.
- **CPU.** At most 4 % of one core at 48 kHz with 64-frame blocks, for the worst model (Reese with 7 saws, 2× oversampled).

## Presets

16, with golden fingerprints:
- **Acid:** Acid Classic, Acid Squelch, Rubber Acid, Seq Acid Line (sequencer on).
- **Moog:** Moog Bass, Funk Pluck, Taurus Pedal, Synthwave Bass.
- **Reese:** Reese, Neuro Growl, Wobble Reese.
- **Sub/808:** Deep Sub, 808 Glide, Distorted 808, Dub Sub.
- **Init.**

## UI

A dark modern panel:
- **Look:** a matte black body. The accent colour follows the model: acid green, Moog amber, Reese violet, Sub red.
- **Header:** model tabs, a preset browser and a status chip.
- **Display:** a large view of the filter response and an oscilloscope sketch of the oscillator.
- **Main knob row:** cutoff, resonance, env mod, decay, accent, drive and slide, then the current model's own controls.
- **Sequencer strip:** 16 step cells, each with its note, octave, gate state, and accent and slide toggles.

## Design notes

### Diode ladder

The linear model has four capacitor states. Unlike the transistor ladder, each stage is loaded by its neighbours, so the states form a tridiagonal system. With u = x − k·y4 and output y4, normalised to ωc = 1:

    ẏ1 = u + y2 − y1
    ẏ2 = (y1 + y3 − 2·y2) / 2
    ẏ3 = (y2 + y4 − 2·y3) / 2
    ẏ4 = (y3 − 2·y4) / 2

- **DC gain.** It is 1 without feedback, and 1/(1 + k) with it.
- **Analytic constants.** The open loop's phase reaches −180° at Ω = 1/√2, where its gain is 1/17. So the loop self-oscillates at k_max = 17, the published diode-ladder figure.
- **Cutoff knob.** The knob sets that resonant frequency, so ωc = √2 · cutoff. Without resonance, the open-loop −3 dB point sits near 0.11 × cutoff: the dark, round 303 low end.
- **Bass loss.** Resonance thins the bass by 1/(1 + k), and a partial makeup of 1 + 0.3k keeps part of that loss, as the hardware does.
- **Discretisation.** The implementation uses trapezoidal integrators, prewarped at the cutoff (g = tan(π·fc/fs)). Each sample it solves the tridiagonal system, using Sherman–Morrison for the y4 feedback term. Its small-signal response is therefore exactly the analog response at Ω = tan(ω/2)/g.
- **Nonlinearity.** It is a saturating input stage, applied after the feedback sum. It is solved by a second linear pass with the saturated input. For small signals it vanishes, which is what lets the oracle be exact.
- **Resonance.** Resonance 0–1 maps to feedback k from 0 to 1.02 × k_max. So full resonance self-oscillates, as on the hardware.

### Accent sweep

The sweep is a one-pole smoother of the filter envelope, fed only while the note is accented. It charges with a 0.1 s time constant and discharges with a 0.3 s one. It is added to the cutoff on top of the normal envelope modulation. Because it does not discharge fully between closely spaced notes, a run of accents climbs: this is the 303's "wow".

### Slide

The pitch approaches its target exponentially with time constant τ = slide time / 3, so it is within 5 % of the interval by the slide time. The tests check this law.

### Sequencer timing

The step clock is an integer frame count since the sequencer started, at frames_per_step = 1 / (beats_per_frame × steps_per_beat).
- **Step onset.** Step n starts at frame ⌈(n + swing_offset(n)) × frames_per_step⌉, where swing_offset is swing / 2 on odd steps and 0 otherwise.
- **Tempo changes.** A tempo change, seen at a control tick, rebases the clock at that tick.
- **Gate.** The gate closes at gate length × step. It stays open into the next step when this step slides into a gated step, or when the next step is a tie.

## Implementation decisions (f382ea2)

- **One voice, stereo path.** The signal path is stereo, but only Reese uses the second channel, to spread its saws. The other models process one channel and copy it to the other. Each channel has its own oversampler, filters, comb and distortion.
- **Per-frame filter coefficients.** The filter envelope and accent sweep move the cutoff every base-rate frame. That includes the diode ladder's factorisation and a `tan`; mono polyphony makes it affordable (0.6 % of a core for Acid).
- **Signal order, 2× oversampled:**
  1. drive into the filter's saturating input;
  2. the filter;
  3. the amp envelope, interpolated to the oversampled rate, with accent gain;
  4. distortion;
  5. decimation.

  Then chorus, delay and the output coupling.
- **Output coupling and the tail bound.** A 5 Hz output DC blocker plays the part of the hardware's coupling capacitor. Short enveloped bass notes and asymmetric drive leave sub-audio offsets, which it removes. `tail_frames` includes its settling (0.3 s), so exports end after it.
- **Voice retirement.** The voice retires on the control grid once the amp envelope is idle. The next note then restarts the oscillators at fixed phases and clears the filters, so every attack from silence is identical. Slides and legato notes never restart anything.
- **Sequencer root.** The pattern is rooted on the newest held note. A new note changes the root from the next step, without restarting the pattern. When the last note is released, the pattern stops and the gate closes. Switching the sequencer off while it runs stops it too.
- **No playhead channel.** The kernel status has no channel for the playing step, so the panel shows the pattern without a live playhead. A status field could add one later.
- **Export parity.** It runs Torque's Seq Acid Line through the same shared export and live layers as every engine: largest difference 0.

## Measured

- **Diode ladder.** Matches the analog model within 0.5 dB, at three cutoffs, three resonances and 40 Hz to 19 kHz. It self-oscillates within 25 cents of the analytic pitch and rings down below k = 17.
- **Slide and 808 drop.** The slide follows its law within 5 cents. Each 808-drop cycle lands within 0.01 cycle of the analytic phase.
- **CPU.** 0.5–1.7 % of one core (budget 4 %); see `docs/performance/torque-synth.md`.
