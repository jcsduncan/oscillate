# ADR-0055 — Deck: a DJ mixer's three-band isolator

**Status:** Accepted and implemented. **Date:** 21 October 2026.

## Context

Mixing a DJ set means automating a channel EQ: the bass swap, a filtered intro, highs pulled for a breakdown. Shape (the eight-band parametric) can do that, but not the way a DJ reaches for it. A club mixer's EQ is three bands with kills, and its isolator removes a band entirely rather than shelving it.

## Decision

Deck is a native effect (ADR-0040) with a designed panel.

**DSP**
- **Two crossovers** split the signal into low, mid and high. Each is a fourth-order Linkwitz-Riley split (24 dB/oct, from the zero-delay-feedback state-variable sections the synths use):
  - **low crossover:** 60–600 Hz, default 250 Hz;
  - **high crossover:** 1–8 kHz, default 2.5 kHz, held at least an octave above the low one.
- **Flat at 0 dB.** The low band runs behind the high crossover's allpass, so every band carries both allpasses and the three sum flat in magnitude at 0 dB.
- **Band gain** runs from a full kill to +6 dB. The bottom stop (−40 dB) is silence, as on a mixer.
- **Kills.** Each band has a kill switch that fades over 10 ms.
- **Smoothing.** Gains glide on the control grid, so automation lanes never zipper or click.
- **No latency.** The tail bound is 50 ms.
- **Telemetry:** each band's level after its gain, and the output peak.

**Parameters**
- `deck.low`, `deck.mid`, `deck.high`: dB, automatable.
- `deck.low_kill`, `deck.mid_kill`, `deck.high_kill`: on/off, not automatable. Automating a band to its bottom stop is the same full kill.
- `deck.low_x`, `deck.high_x`: Hz, automatable.
- `deck.gain`: output gain, automatable.

**Presets:** Flat, Bass Swap (Low Killed), Mids Out, Highs Out, Bass Only, Warm, Crisp.

**Panel**
- A club mixer's channel: HIGH, MID and LOW as big knobs, top to bottom, each with a lit kill button and a live level bar in the band's colour.
- The response the isolator runs: each band filled in its colour, with the sum on top, from the same complex Linkwitz-Riley responses.
- The crossovers, the output gain and the output peak meters.
- The device strip shows Low, Mid, High and Gain.

## Consequences

The oracle tests pin the behaviour:
- **Flat:** within 0.05 dB from 40 Hz to 12 kHz, and for noise.
- **Kill depth:**
  - low kill: below −50 dB at 40 Hz;
  - high kill: below −45 dB at 12 kHz;
  - mid kill: below −30 dB at 790 Hz.
- **Bottom stop:** bit-identical to the kill switch.
- **Crossovers:** −6 dB at each corner.
- **Gain accuracy:** boosts and cuts exact in band.
- **Kills:** no click.
- **Crossover automation:** finite and bounded while swept.

Deck also passes the platform contract with every other effect: block splits, presets, tail bound and no allocation on the callback.
