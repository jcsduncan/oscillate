# ADR-0063 — Crate: a chop sampler (replaces the sampler)

**Status:** Accepted; implemented. **Date:** 26 October 2026.
**Builds on:** ADR-0027 (native-synth platform), ADR-0057 (structured synth documents and managed media), ADR-0060 (Anvil's media slot), ADR-0059 (note effects), ADR-0035 (pedals).
**Supersedes:** the sampler of ADR-0014, ADR-0050 §30 (sampler in the source bank), ADR-0059 §117 (note effects refused on the sampler). The sampler is removed.
**Functional spec:** [specs/09-crate-sampler.md](../specs/09-crate-sampler.md). **Panel design:** [ui/crate-panel-mockup.html](../ui/crate-panel-mockup.html).

## Context

Requirement: a modern sampler for chopping old samples. The design is an MPC/SP-style chopper with automatic slicing, pads, per-chop controls, Chop to MIDI and vintage "era" character.

The existing sampler cannot grow into that. `SamplerPatch` is one sample, one root key, a linear ADSR and a one-pole lowpass. Its voices interpolate linearly, so pitched-up notes alias. It has no waveform, no regions and no way to load a user's file, and it lives on a separate path through the model, codec, source bank, live control, automation and export, beside the native-synth platform that already has presets, automation, macros, note effects, pedals, freeze and managed media.

## Decision

1. **Crate is a native-synth engine (id `crate`).** It rides the ADR-0027 platform: a parameter table, presets, a kernel, the panel overlay, automation, macros, note effects, pedals, freeze and export come with it.
2. **The sampler is removed entirely**, with no migration (no-legacy rule): `SamplerPatch`, `SetSamplerPatch`, `sampler_level`, the sampler runtime, its feeds and live control, the export path and the panel. The starter sounds become Crate presets that load a bundled sample as one chop. Schema 1.44; projects with a sampler track stop opening.
3. **Regions join the structured document.** `NativeSynthDocument` gains `regions`: `NativeRegion{slot, media, start_frame, tag, name}`. A region runs to the next region's start in its media, or to the media's end. The model checks structure; the engine checks meaning. Regions are generic, so Nova's multisample oscillator can use them later.
4. **Per-chop controls are flat parameters keyed by the region's slot,** not its position: `crate.chop.NN.<control>`, NN being the slot (00–31). Automation and macros stay with the audio when chops are added, moved or reordered, as Nova's route slots do (ADR-0057 §4). A removed region resets its slot's parameters. Re-chopping a sample (document and parameters together) is one undo step through `ProjectSession::set_native_instrument_state`.
5. **Up to 32 chops**, two banks of 16 pads. A two-bar break cut in sixteenths is 32 chops; 32 × 17 parameters plus the globals stays well under the 1024 cap.
6. **The sample is managed media,** in the document's media slot `sample`, exactly as Anvil's click: a managed source referenced by sha256, decoded on a worker, reused across edits while the hash is unchanged. Import gains an asynchronous path that also takes encoded files (MP3, FLAC) where the codec build has them.
7. **Chopping is application code,** pure and deterministic over the decoded sample: transient, grid, equal and manual. Each chop is tagged kick, snare, hat or other from its band energies, high-frequency share and decay. Tempo, key and root come from the existing analyses.
8. **Playback is band-limited.** The warp engine's band-limited kernel becomes a public, allocation-free reader, and each voice reads the resident sample through it at any speed, forward or reversed. In Clean, pitching a chop up two octaves does not alias. Follow Tempo stretches each voice in the kernel with an allocation-free grain overlap-add that restarts at the chop's start, so attacks stay sharp; it follows tempo automation.
9. **Eras are measured, not guessed.** Clean, SP-12, MPC60, S950 and Tape set bit depth, sample rate, interpolation, the anti-alias filter and saturation. Their figures are fitted to measured references (`tools/crate_reference`), as Anvil's kicks were.
10. **Chop to MIDI** writes one MIDI clip, as one undo step, that plays the chops in their original order at the sample's detected tempo. Pads map from MIDI key 36 (C1) upward; bank B continues from 52.

## Consequences

- One sampling instrument, on the same platform as every other engine: everything the platform gains, Crate gains.
- A large removal across model, persistence, playback, export and UI, landed together with the schema bump, since removing the variant breaks every consumer.
- Old projects with a sampler track stop opening; examples and fixtures are regenerated.
- Follow Tempo is grain-based in the kernel. A Complex-quality pre-render can be added later on the worker that decodes media.
