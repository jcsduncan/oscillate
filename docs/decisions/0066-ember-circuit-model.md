# ADR-0066 — Ember's circuit model: a vintage poly that misbehaves like one

**Status:** Accepted; implemented. **Date:** 29 October 2026.
**Builds on:** ADR-0028 (Ember), ADR-0037 (synth telemetry).

## Context

Requirement: an analogue synth with circuit parasitics and slight tuning error. Ember is rebuilt in that spirit rather than adding an engine. Ember's Age knob only scaled seeded drift, a few per-voice tolerances and a noise floor, the same way for both oscillators. That is a virtual-analogue core with ageing laid on top, not the behaviour of the circuits it imitates.

## Decision

`ember_circuit.hpp` holds the model as pure functions of a voice card, so each law is tested on its own. Every value comes from seeds for the card and the unit, so export equals playback.

1. **DCO A is crystal-divided (Juno-style).**
   - It never drifts. It can only land on `8 MHz / N`, so high notes sit off by up to a cent or two.
   - With Age, its charge DAC leaves its level rippling a little from note to note.
2. **VCO B is a saw-core VCO (Prophet-style).**
   - **Tracking:** the exponential converter tracks up to ±2.5 cents per octave from middle C.
   - **Top of the keyboard:** bulk resistance takes 1–2 cents per kHz, and the core's 0.8–2.2 µs reset adds to every period, so the top runs flat.
   - **Wander:** slow drift plus fast jitter.
   - **Ramp:** the saw bows as its timing capacitor charges.
   - **Interaction:** the steady DCO and the wandering VCO beat against each other, as on the instruments.
3. **Each card has its own offsets:** cutoff, resonance, envelope speed, pulse width and VCA control offset. A **Unit** (1–16) picks the set of cards, so each unit is a different instrument and the same unit is always the same one.
4. **Leakage (Bleed):**
   - Every oscillator bleeds into the mixer about 60 dB down, whatever its level.
   - The closed filter lets input through about 54 dB down.
   - The VCA never quite shuts (about 66 dB down). The leak fades over 250 ms once a voice ends, and then the voice frees.
   - The VCA's control offset passes a trace of the envelope itself: the thump on a fast attack.
5. **The supply sags** with the summed level of the sounding cards. At full Age, eight cards at full level flatten the VCOs by 5 cents. The DCOs don't sag.
6. **Mains hum** at 50 or 60 Hz, with the rectifier's harmonics. It is gated by the instrument sounding, so silence stays silent and the tail stays bounded.
7. **Parameters:** `ember.circuit.tuning` (calibration), `bleed`, `hum`, `mains` and `unit` join `ember.age` in a **Circuit** section. Age now means card wear: tolerances, curvature, the drift's age share, DCO ripple and supply sag. Tuning means VCO calibration: tracking, droop, reset and drift.
8. **Telemetry:** for each card, whether it sounds, VCO B's and DCO A's cents from ideal, and its cutoff offset. Also the hum gate and the supply load. The panel draws the eight cards live.

## Consequences

- **Defaults:** Tuning 0.3 and Bleed 0.25, so a new Ember already sounds like a real instrument. "Age Max" turns everything up on unit 7.
- **Factory sound:** the factory fingerprints were re-baselined. Levels moved by at most 1.6 dB, and tone centroids by about 10%.
- **Testing:** a calibrated, leak-free patch (Age, Tuning, Bleed and Hum at 0) still plays within half a cent of ideal pitch.
- **Not yet done:** fitting the model to recordings of real vintage instruments remains open.
