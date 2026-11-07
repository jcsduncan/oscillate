# ADR-0057 — Nova: the flagship wavetable synth (replaces Lumen and Snarl)

**Status:** Accepted; platform, engine core, panel, FX rack, arpeggiator and clip sequencer are built. The Sample, Multisample, Granular and Spectral oscillators render silence, and the wavetable editor cannot save drawn frames; see spec 08 §13. **Date:** 23 October 2026.
**Builds on:** ADR-0027 (native-synth platform), ADR-0038 (live switching), ADR-0054 (live controls).
**Supersedes:** ADR-0044 (Lumen wavetables) and ADR-0056 (Snarl growl synth). Both engines are removed.
**Functional spec:** [specs/08-nova-synth.md](../specs/08-nova-synth.md). **Panel design:** [ui/nova-panel-mockup.html](../ui/nova-panel-mockup.html).

## Context

Requirement: one flagship synth with the scope of a modern commercial wavetable synth (the Serum 2 class), with dense, wide unison and the classic subtractive path (sine, saw and so on into a filter, then LFOs), replacing Lumen and Snarl rather than extending them.

Lumen and Snarl each cover part of that ground. Neither is a flagship, and two half-instruments are worse than one complete one. The project owns no samples or wavetables, so the content must be generated or sourced under licences that allow bundling.

The current platform cannot hold a patch of that class:
- **Dense parameters only.** Each engine exposes one dense `std::vector<double>`, capped at 256 entries. The cap appears in `max_engine_parameters` (`parameters.hpp:52`), the control mailbox (`parameter_control.hpp`), `max_native_instrument_parameters` (`model.hpp:476`) and the codec.
- **No structured state.** Drawn LFO curves, modulation routes, custom wavetables, samples, clip-sequencer notes and FX order have no home.
- **No media path into synths.** `SynthEngine::create_kernel(sample_rate)` receives no media store.
- **Undo copies whole snapshots.** Large blobs inside the patch would make every edit expensive.

## Decision

1. **One flagship engine, Nova (id `nova`),** with the scope in the functional spec:
   - 3 main oscillators. Types: Classic, Wavetable, Sample, Multisample, Granular, Spectral.
   - Unison of up to 16 voices per oscillator.
   - Sub and noise.
   - 2 filters with routing.
   - Drag-to-modulate, with 8 drawable LFOs, 4 envelopes, 8 macros, chaos, MPE and an unlimited matrix.
   - A reorderable FX rack with buses.
   - An arpeggiator and a clip sequencer.
   - A wavetable editor.
   - A large factory library.
2. **Lumen and Snarl are removed entirely,** with no migration, per the no-legacy rule. Snarl's reusable DSP moves into shared modules and Nova uses it: the rack modules, the formant/vowel filter, the OTT and the growl table generators. The removal inventory is in the spec (§11).
3. **Platform: structured synth state.**
   - **Patch structure.** `NativeInstrumentPatch` gains a typed, versioned `NativeSynthDocument` alongside the flat parameters. It holds mod routes, curves (LFO shapes, envelope curves, matrix curves), FX order, arp and clip-sequencer data, and **references** to tables and samples.
   - **Media.** Tables and samples are managed media addressed by sha256, like `ConvolutionIrReference`.
   - **Parameter cap.** The flat limit rises to 1024 everywhere it is enforced. The mailbox becomes a heap-allocated, prepare-time array rather than a `std::array` on the stack.
   - **Schema.** Schema 1.40, with no migration. A project with a Lumen or Snarl track will not open; this is accepted.
4. **Modulation amounts are first-class parameters,** so they are automatable. Every route has a stable parameter id `nova.mod.<route-uid>.amount` in a reserved block of 256 route slots. A route that is deleted frees its slot; ids are never reused within a document.
5. **Kernels get a media path.** `SynthEngine::create_kernel` receives a `SynthResources` view: decoded tables and samples resolved off the audio thread, swapped in through the existing live-edit revision and acknowledgement protocol.
6. **Content strategy:**
   - procedurally generated tables in-house (the primary source);
   - the **AKWF** single-cycle waveforms, which the author dedicates to the public domain under CC0;
   - user import (WAV/FLAC/AIFF, Serum-style `clm` markers, single-cycle folders).
   - Nothing from any commercial synth or pack is bundled.
7. **The panel follows the house synth-panel style** and the panel design: navy plate, cyan values, violet modulation, amber time.
8. **Never copy a commercial synth's assets or identity:** no tables, presets, names, branding or exact layout. Feature parity with the class is the goal; identity stays our own.

## Consequences

- One instrument to make excellent, not three to maintain. The engine count drops from nine to eight: Prism, Ember, Pulse, Halo, Torque, Drift, Concert and Nova.
- The structured-state platform also benefits the existing engines later, for example drawable LFOs in Drift. Nova is its first consumer.
- Old projects that use Lumen or Snarl stop opening. Demo projects and fixtures are regenerated.
- This is the largest DSP surface in the app. It is delivered in phases (spec §12), each releasable and tested.

## Quality review against the class (26 October 2026)

Nova was measured for cleanliness and ease of use against the leading commercial wavetable synths, then fixed:

- **Classic oscillators are band-limited to the output's Nyquist at every oversampling factor.** The shared 24-tap BLEP left the top harmonics folding back just below Nyquist (−31 dB at 23.9 kHz), and at 2× and 4× the Classic shapes carried harmonics up to the internal Nyquist into the non-linear filters, which folded them into the audible band (−55 to −73 dBc through the Ladder). Nova now uses `clean_blep`: 64 taps with the stopband below Nyquist, and at 2× and 4× the same span in time with the cutoff scaled down (`clean_blep_2x`, `clean_blep_4x`), chosen per note. A Classic saw through the Ladder now measures −94 to −101 dBc at High and Ultra, level with the wavetables. The other engines keep `standard_blep`, bit for bit.
- **A DC blocker after the filters,** before the amp. A saturating filter turns an asymmetric wave into DC (−41 dB on a filtered saw), which the amp envelope shaped into a thump at each note's ends.
- **Init is an open saw with the filter off,** and a new instrument starts from its engine's Init preset rather than the bare defaults (which stay as they were, so saved projects sound the same).
- **The panel:** a preset browser (search, categories, tags, favourites, Save/Save As/Rename/Delete; user presets now save for synths), knobs that type values, reset with Ctrl+click and show their full name; drag the wavetable display, the filter curve and envelope nodes; every FX parameter on its card; all 36 factory tables in the picker with step arrows; no overlapping or abbreviated captions; the amp envelope shown first.
- **The library** grows from 10 to 46 presets, tagged, each played at one level (a chord peaks near −6 dBFS).
