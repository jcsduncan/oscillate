# ADR-0038 — Live structural switching in native synths

**Status:** Accepted; implemented. **Date:** 19 October 2026.
**Amends:** ADR-0034, which defined next-note semantics for the parameters this ADR makes live.

## Context

Structural parameters used to apply only to the next note: filter types, unison counts, sub octaves, Halo's algorithm, Torque's model and Reese voice count. A sounding note kept its old structure, and the status bar reported the change as deferred.

That was a limitation of the engines, not a choice made for musical reasons:
- Turning the filter type knob on a held pad did nothing until the next note.
- Stepped automation of these parameters (ADR-0034) only took effect on later notes.
- Hardware and the reference instruments switch these live.

Requirement: fix this in the engines rather than work around it. CPU budgets are soft; sound comes first.

## Decision

**Every structural parameter switches live, without a click.** On a change, the voice crossfades from its old structure to its new one over 20 ms.

- **`SwitchFade`** (`crossfade.hpp`) is the shared fade: a linear crossfade of 20 ms. Linear is right because the two signals start from the same input and are strongly correlated.
- **Warm copies.** While the fade runs, the old structure keeps running on a copy of its state, so neither side starts from silence:
  - **A filter type within one family** (two SVF modes, or Lumen's two combs) copies the filter, and the new type continues from the same state.
  - **A filter type across families** (Ember OTA ↔ SEM, Lumen SVF/OTA/SEM/comb) runs the old type on its own state, which the new type never touches. The new type's state starts clean and fades in from zero.
  - **Unison counts and sub octaves** (Prism, Lumen) copy the oscillator section. Lanes that both structures share keep their phase; added lanes start with the note-start phase policy.
  - **Halo's algorithm and unison** copy the routing (carriers, feedback operator, role ramps, lanes); the operators themselves are shared.
  - **Torque's model and Reese count** copy the model state (oscillators, levels, comb lines, whose storage is reused).
- **Where they mix.** Two oversampled filters mix at the oversampled rate before one downsampler. A base-rate type (Lumen's comb) mixes with an oversampled one at the base rate.
- **Fresh ramps.** The new structure's ramps jump to their targets at the switch instead of gliding from the old structure's values. It is silent at that moment, so the jump cannot click.
- **One switch at a time.** A change that arrives during a fade waits until it ends, then switches from the structure that is sounding. Fast automation therefore steps at most every 20 ms.
- **No allocation.** Every copy goes into storage prepared with the voice.

**Note-start settings stay next-note.** Some settings only mean something when a note begins, and stay deferred and reported as before:
- oscillator start phase (Prism, Lumen);
- LFO poly/mono mode (Prism, Lumen);
- voice mode (poly, mono, legato) in every engine, and Ember's chord shape.

## Alternatives considered

- **Keep next-note semantics.** This was rejected: it is the limitation being fixed.
- **Switch instantly at a zero crossing.** Rejected because a stereo, multi-voice signal has no shared zero crossing, and filter state would still jump.
- **Reset the new structure and fade it in from silence, with no warm copy.** Rejected because it dips in level for every switch, and resonant filters audibly re-excite.
- **An equal-power crossfade.** Rejected: the two signals are correlated, so equal power would bulge by up to 3 dB mid-fade.

## Consequences

- **The engines and their tests:**
  - Drift, Halo, Torque, Ember, Prism and Lumen switch structure live. Their tests show the change is never deferred and that the sounding note takes the new structure.
  - Lumen's test walks every filter family pair.
  - Torque's test checks there is no allocation.
- **Click contract.** The engine-generic switch contract switches every live discrete parameter mid-chord through a stepped lane. Each step's transient stays within twice the steady signal.
- **Goldens are unchanged**, because nothing differs while no switch is running.
- **CPU.** A voice renders the switched stage twice during a fade.
- **Panels** show "applies to new notes" only for the remaining note-start settings, because it is driven by the table.
