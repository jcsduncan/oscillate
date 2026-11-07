# ADR-0036 — Concert: physically modelled grand piano (eighth native engine)

**Status:** Superseded by [ADR-0071](0071-concert-sampled.md) on 29 October 2026: Concert is now a sampled grand, and the model below (`piano_dsp`) has been removed. Kept as the record of what was tried.
**Date:** 19 October 2026.
**Builds on:** ADR-0027 (the native-synth platform), ADR-0034 (stepped automation) and ADR-0035 (pedals in MIDI clips).

## Goal

A concert grand that behaves like the instrument. Velocity changes the tone through the hammer's physics, the pedals work the dampers, and undamped strings ring in sympathy. Like the other engines it needs no samples.

**Sound comes first.** The CPU cost is measured and reported, never traded for sound.

**Non-goals for v1:**
- matching a measured Steinway (the model uses concert-grand-like curves, not a scanned instrument);
- tension-modulation "phantom" partials;
- MPE;
- re-pedalling articulations beyond the three pedals.

## Model

### Strings

Every key has strings that obey the physics:

| Register | Strings per key |
|---|---|
| Lowest octave and a half | 1 |
| Tenor | 2 |
| Upper | 3 |

**Each string is modal** (amended during implementation; see *Why modal* below). Each partial is its own resonator, held in rotation form. That form keeps frequencies exact even in single precision.

- **Frequencies and stretch.** Partial n sounds at exactly n·f0·√((1 + B·n²)/(1 + B)), with B following a concert-grand curve across the keyboard, scaled by the Inharmonicity control. Stretch tuning follows a Railsback-like curve, scaled by Stretch; at 0, tuning is equal temperament.
- **Strike position.** The hammer drives mode n through the string's mode shape at the strike point, sin(nπβ), with β from 1/7 to 1/12 by register. That puts the strike-position comb exactly where the physics does.
- **Decay.** Each partial decays at σ(f) = b₁ + b₃·f², a register-dependent law, scaled by Decay and Brightness.
- **How many partials.** Each string carries the partials up to the lower of 0.45·fs or a register cap, at most 128.
- **The bridge.** Every string meets one resistive bridge node, through per-partial coupling weights. The bridge takes energy from in-phase motion and passes it to the soundboard, so:
  - unison strings (detuned by a few cents) beat;
  - they show Weinreich's two-stage decay: the in-phase mode dies promptly and the anti-phase modes ring on;
  - the bridge's motion excites every undamped string, which gives sympathetic resonance.

  The coupling is passive (a dashpot shared by all strings), so it can only take energy out, never add it.

**Why modal.** The first design was a waveguide with a dispersion allpass cascade. It was prototyped against this ADR's own 1-cent oracle and failed it:
- **A cascade of identical first-order allpasses** (16 stages in the bass) is already 79 cents off at A0 within 2 kHz, and holds 1 cent only in the middle register.
- **A high-order allpass design** (Abel–Välimäki–Smith style) needs dozens of biquads per bass string to approach the stiff-string law.
- **The modal string** meets it exactly at every key and sample rate. Its cost is per partial, and it vectorises.

### Hammer

A felt hammer is a mass on a nonlinear spring, F = K·x^p (p ≈ 2.2 in the bass to 3 in the treble), with hysteresis. It collides with the string at the strike point. During contact it is integrated with sub-steps.

- **Velocity** sets the hammer's speed, so tone and loudness follow from the physics.
- **Hardness** scales K.
- **Una corda** (the soft pedal, which can be half-down) shifts the hammer: it strikes fewer strings, on softer felt.

### Soundboard and radiation

- **The soundboard** is a bank of tuned low modes plus a coloured 8-line FDN for the dense upper modes. The bridge force drives it.
- **The bridge** is the shared node that couples the strings (see *Strings*), so it is where sympathetic resonance comes from.
- **Lid, mic distance and perspective** shape colour and stereo image. Perspective places the bass on the left from the player's seat, or mirrored from the audience.

### Dampers and sympathetic resonance

- **Every string runs** while its damper is up: when its key is held, under the sustain pedal (including half-pedal, which interpolates damper contact), under sostenuto, or because it sits in the undamped treble above F6.
- **Undamped strings** take the bridge signal, so held keys and the pedal make the instrument bloom. Duplex aliquot resonators add their shimmer in the treble.
- **Damped strings** lose energy fast. A damped string whose energy falls below −120 dB is skipped until it is struck or its damper lifts, which has no audible effect.

### Mechanical noises

Synthesised and level-controlled:
- key-bottom thump;
- hammer knock;
- key-release and damper-fall noise;
- pedal noise.

### Instrument types

The types set model constants, and the character controls fine-tune them:
- **Concert Grand**
- **Bright Modern**
- **Warm Vintage**
- **Upright:** shorter strings, higher B, a boxier soundboard.
- **Felt:** a felt strip, a softer hammer, damped highs and more mechanism.

## Quality bar (tested)

- **Partials:**
  - partials follow n·f0·√(1 + B·n²) within 1 cent, for the designed partial range;
  - each fundamental is within 0.5 cent of its target, at 44.1, 48 and 96 kHz;
  - partial decay times match the loss design within 10 %.
- **Hammer:**
  - in isolation, the force follows K·x^p;
  - contact time falls in published ranges (about 1–4 ms), shortening with velocity and register;
  - the spectral centroid rises monotonically with velocity.
- **Strings and pedals:**
  - the unison beating rate equals the detune difference;
  - unison decay has two slopes;
  - sympathetic resonance: a silently held C3 rings at its harmonics after C4 is struck, and stays silent when damped;
  - half-pedal damping is monotonic between pedal up and down;
  - the undamped treble rings on after release;
  - sostenuto holds only the notes held when it went down.
- **Inherited contracts.** Block-split invariance, export equal to live playback (including the pedals), no allocation in the callback, a tail bound, the click-free switch contract, and goldens.
- **CPU** is measured and reported, with no gate.

## Presets

16, with golden fingerprints:
- Concert Grand (default), Bright Modern, Warm Vintage, Felt Piano;
- Upright, Close Mic, Hall, Soft Pedal;
- Ballad, Rock Piano, Honky-Tonk, Prepared-ish;
- Small Grand, Dark Felt, Stretch Tuned, Init.

## UI: Concert black

- **Look:** gloss-black lacquer with brass and gold accents.
- **String and soundboard view:**
  - shows the dampers, the undamped treble, and the pedal states;
  - shows each key's strings ringing, live from telemetry (ADR-0037), including sympathetic ringing;
  - previews how the current voicing excites partials and how long they decay.
- **Voicing controls,** grouped like a piano technician's: Hammer, Strings, Dampers & Pedals, Soundboard, Mic & Lid, Mechanics.
- **Selectors:** an instrument-type selector and a preset browser.

## As built (19 October 2026)

- **Primitives** live in `piano_dsp.hpp/.cpp`:
  - per-key physics (`key_physics`), on concert-grand register curves for length, tension, B, strike point, hammer mass, K and p (Chaigne–Askenfelt anchors), and the string, bridge and damper T60s;
  - `Strings`: every mode as a single-precision rotation with decay, the shared dashpot bridge, sub-stepped hammer contact, damper contact, and retirement of silent damped strings on the control grid;
  - `Soundboard`.
- **Kernel** (`concert.cpp`):
  - It models its pedals (`SynthEngine::models_pedals`). The sustain pedal lifts the dampers along a half-pedal curve; sostenuto captures the keys down when it goes down; soft shifts the hammer off the last unison string and softens the felt.
  - Dampers lift in 6 ms and fall in 40 ms.
  - Live voicing edits re-voice eight keys per control tick. Instrument type and perspective switch at the bottom of an 8 ms dip each side, so the switch never clicks.
  - Out-of-range MIDI keys are ignored, as on the instrument.
- **Telemetry** carries the three pedals, then each key's string energy and damper contact.
- **Tests:**
  - `native_piano_dsp_contract` covers the physics oracles listed above;
  - `native_concert_contract` covers the engine;
  - Concert also joins the cross-engine switch contract, and the export-parity contract, pedals included. The generic pedal contract skips it, because Concert models its own pedals.
- **CPU** is in `docs/performance/concert-synth.md`.
