# ADR-0044 — Lumen: wavetable synth (third native engine)

**Status:** Superseded by [ADR-0057](0057-nova-flagship-synth.md); Lumen is not in the code. **Date:** 18 October 2026.
**Builds on:** ADR-0027 (the native-synth platform) and ADR-0028 (Ember).

Lumen is a morphing wavetable synth. Every table is generated in code, deterministically, so Lumen ships no assets and needs no licence review. Like Ember, it must add no project-format, session, playback or export changes.

**Goal:** a modern wavetable synth for evolving pads, growling basses and glassy leads. Its headline is smooth table-position morphing under heavy modulation.

**Non-goals for v1:** user table import (later, as managed media), spectral resynthesis, MPE, drag-to-modulate, user preset files.

### Wavetables (generated, deterministic)
- **Format:** 32 frames per table, each a single cycle, stored as **mip-mapped band-limited levels**: one level per octave, built by FFT and truncation of the harmonics, with table length halving at higher levels. All tables are built on the control thread at prepare time and shared immutably across voices and engines through a cache.
- **Sixteen generated tables:**
  - Basic Shapes (sine → tri → saw → square)
  - PWM Sweep
  - Harmonic Series (additive build-up)
  - Odd Harmonics
  - Formant / Vowel (A-E-I-O-U)
  - Sync Sweep
  - FM Ratio Sweep
  - Phase Distortion
  - Bit Crush
  - Noise Spectra (seeded)
  - Bell Partials
  - Organ Drawbars
  - Digital Saw Stack
  - Hollow
  - Pluck Decay
  - Supersaw Fold
- **Playback:** linear interpolation within a frame; crossfade between adjacent frames (position) and between mip levels by pitch, so sweeps never step.

### Voice
| Block | v1 specification |
|---|---|
| Osc A and Osc B | Table select, position (0–1), octave / semi / fine, level, pan, phase (free / reset / random). Unison 1–8 with detune, blend and stereo spread. |
| Warp per osc | Off, Bend+ and Bend− (phase distortion), Sync (windowed band-limited sync), Mirror, Asym, FM from the other osc. Each has an amount. |
| Sub / Noise | Sub: sine or square at −1 or −2 octaves. Noise: white or pink. |
| Filter (shared types) | Prism SVF LP/BP/HP/Notch 12 and 24 and the ladder, Ember OTA 24 and SEM morph, plus a new **comb** (+/−). Cutoff, resonance, drive, keytrack, routing A / B / both. |
| Envelopes | Three ADSRs with curves: amp, env 2 and env 3. |
| LFOs | Three, with shapes sine / tri / saw / square / S&H / **step sequencer (16 steps)**. Tempo sync, retrigger, fade, poly or mono. |
| Matrix | 8 slots: source → destination × amount. Sources: envelopes, LFOs, velocity, key, wheel, bend, pressure, random-per-note, and osc B output. The prime destination is **wavetable position**; the others are warp amount, pitch, cutoff, resonance, drive, levels, pan, unison detune, LFO rates and effect mixes. |
| FX | Reuses Ember's primitives and Prism's chorus: drive, chorus, BBD delay (sync), ensemble, and a simple Schroeder-style plate reverb if cheap enough (otherwise omitted and noted). |
| Voicing | 16 notes, each owning its unison voices; poly / mono / legato with glide. |

### Quality bar (tested)
- **Aliasing, no warp:** the inharmonic floor is below −90 dBFS from 20 Hz to 18 kHz for the saw table at C8, 44.1 kHz.
- **Aliasing, with warp:** warps run with 2× oscillator oversampling and a mip level chosen for the warp's peak instantaneous frequency. Measured and reported per warp, with a target below −60 dBFS at C6.
- **Smoothness:** a position sweep from 0 to 1 over 100 ms is click-free, with no step larger than a band-limited edge.
- **Determinism:** tables are bit-identical across runs and builds of the same compiler; their generation is deterministic.
- **CPU:** at most 15 % of one core at 48 kHz with 64-frame blocks, with 16 notes × 4 unison on both oscillators, warps on, and an 8-slot matrix active. The 8× unison worst case is measured and reported.
- **Inherited contracts:** no allocation in the callback; the 32-frame grid; next-note deferral; declicked steals and seeks; block-split invariance; export equals playback.

### Presets
16 presets with golden fingerprints:
- **Default and pads:** Lumen Default (morphing pad), Glass Keys, Vowel Pad, Evolving Drone.
- **Bass:** Growl Bass, Reese Wavetable, FM Bass.
- **Leads and plucks:** Pluck Seq (step LFO), Sync Lead, Bell Shimmer, Supersaw Fold, Wobble WT.
- **Texture and character:** Organ, Digital Arp, Noise Sweep, Init.

## Design notes

- **Table layout.** Tables have 32 frames. Each frame is generated as a 16,384-point time-domain cycle, or analytically where a closed form exists, and transformed with an FFT; harmonics 1 to 512 are kept. Mip level L keeps harmonics up to 512 >> L, plus a final sine level. Each level is stored at length clamp(4·H, 256, 2048), with three guard samples so cubic reads never wrap.
- **Interpolation.** A 4-point cubic within a frame keeps interpolation images of the top harmonic near −100 dB. Linear interpolation would leave them about 78 dB down at best, and far worse on short levels.
- **Memory and build time.** The full set of sixteen tables, about 11 MB, is built once per process on the control thread and shared immutably. A live table change from the panel therefore never generates anything in the callback.
- **Mip selection.** The level is chosen at each control tick from the highest instantaneous frequency, including each warp's worst-case speed-up. A level change crossfades over one tick.
- **Warps.** Warps are phase transforms with bounded slope, so the mip compensation is exact. Bend uses a sinusoidal phase bend, Asym puts a knee at 0.1 or later, and Sync has a ratio up to 8× with a smooth window. Mirror crossfades with a mirrored read. FM is phase modulation by the other oscillator's output, so it is safe through zero.
- **No plate reverb in v1.** The DAW already has a native reverb insert.

## Decisions taken during implementation

- **Smooth warps.**
  - *Mirror* reads forward and back with a raised-cosine phase, so the turnaround has no corner.
  - *Asym* shifts the midpoint with the smooth periodic remap p + k·sin²(πp).
  - The first versions, a triangle-phase mirror and a piecewise-linear knee, aliased at −35 and −50 dBFS.
  - Measured at C6 with amount 0.8: Bend −100/−104, Sync −73, Mirror −104, Asym −102 dBFS.
- **Shared steps.** All three LFOs share one 16-step sequence, and any LFO uses it when its shape is *Steps*.
- **Per-voice matrix targets.** Matrix targets act on each voice: positions, warps, pitch, filter, levels, pan, detune and LFO rates. Global effect mixes are not per-voice targets.
- **Noise Sweep revoiced.** The narrow comb passes little broadband energy, so the preset needed more source level.
- **CPU budget.** The 15 % budget is met without warps (12.3 %). With Sync and Bend on every 4+4 unison voice it is 22.3 %; see `docs/performance/lumen-synth.md`. Sharing a SIMD warp path across unison voices is the planned follow-up.
- **No plate reverb in v1.** The DAW's native reverb insert covers it.
