# ADR-0027 — Prism: default native synth and native-synth platform

**Status:** Accepted; implemented. Prism is the default native synth, and the native-synth platform it introduced (`oscillate_native_synth`, `oscillate_synth_engines`) serves every native engine. **Date:** 18 October 2026.
**Requirements:** PRD §13; FS-04 PI-06–PI-08. The synth is a native instrument adapter on the existing source → chain → output path, with its own CMake target, no JUCE or other new dependency, no callback allocation, locking or file I/O, and an independent oracle for pitch, envelope, aliasing, voice stealing and 44.1–96 kHz / 64–128-frame behaviour.

## Goal

Every MIDI track in a new project should sound great immediately. Prism is a clean, punchy and versatile modern virtual-analog synth. It is also the first engine on a reusable native-synth platform. Later siblings, such as a bass synth or an analog-warmth synth, are added as new engines without a schema change.

**Non-goals for v1:**
- wavetables;
- MPE;
- user preset files or a preset browser;
- in-synth delay or reverb, because the track FX chain provides them;
- a VST3 build;
- JUCE or any other new dependency.

## Sound engine

| Block | v1 specification |
|---|---|
| Osc 1 & 2 | Saw, pulse (PWM 5–95 %), triangle or sine. Octave ±3, semitone ±12, fine ±100 c, level and pan. Hard sync (2→1), linear FM (1→2) and ring-mod mix. |
| Unison | 1–8 voices per oscillator. Detune 0–100 c on a supersaw-curve spread, stereo width 0–100 %, random or fixed start phase. |
| Sub / noise | Sub: sine or square at −1 or −2 octaves, with level. Noise: white or pink, with level. |
| Filter | ZDF multimode: SVF LP, BP, HP and notch at 12 or 24 dB, plus a resonant ladder LP24. Cutoff 20 Hz–20 kHz on a log scale. Resonance 0–1, self-oscillating near 1. Pre-filter drive 0–24 dB, keytrack 0–100 %, envelope amount ±100 %, velocity→cutoff. |
| Envelopes | Amp ADSR and filter/mod ADSR. A, D and R each run 0.5 ms–20 s. Curve shape from linear to exponential, and velocity sensitivity. |
| LFO ×2 | Sine, triangle, saw, square or S&H. Free rate of 0.01–40 Hz, or tempo-synced from 1/32 to 8 bars with dotted and triplet options. Retrigger or free-running, fade-in, mono or per-voice. |
| Mod matrix | 4 slots with a bipolar amount. Sources: env 2, LFO 1/2, velocity, mod wheel, aftertouch, keytrack, pitch bend. Destinations: pitch (all, osc 1, osc 2), PW, oscillator mix, FM depth, cutoff, resonance, drive, level, pan, LFO rate. |
| Hard-wired | Env 2→cutoff; LFO 1→pitch (vibrato, scaled by the mod wheel); LFO 2→cutoff/PW knobs. |
| In-synth FX | Post drive (soft clip, 2× oversampled). Stereo Juno-style chorus with modes I, II and I+II, and a mix control. |
| Output | Level −inf to +6 dB, DC blocker, safety soft limiter. |

## Performance

- **Polyphony:** 16 notes. Each note owns up to 8 unison voices per oscillator, so there is no shared sub-voice pool.
- **Voice stealing:** deterministic. Released voices are stolen first, then the oldest. A repeated note retriggers its own voice. A stolen voice is faded over 64 frames to avoid a click.
- **Modes and glide:** poly, mono and legato. Glide runs 0–5 s, as constant-time or constant-rate, and applies either always or on legato notes only.
- **Controllers:** pitch bend with a range of 0–24 semitones, set separately for up and down. Mod wheel and pressure (aftertouch) are routable modulation sources, as is velocity. MIDI clips currently carry notes only, so wheel, bend and pressure are automatable performance parameters for now; clip controller data can drive them later. The sustain pedal waits for that controller data.

## Quality bar (tested)

- **Anti-aliasing:** oscillators correct every step and corner with table BLEP/BLAMP residuals (24-tap Kaiser-windowed sinc), including hard-sync resets and pulse-width sweeps. For the reference saw, aliasing stays below −80 dBFS between 20 Hz and 18 kHz up to C8 at 44.1 kHz. The kernel adds a fixed, linear-phase 11-frame latency. Two-point PolyBLEP was rejected because it cannot meet this bar at high notes.
- **Oversampling:** 2× oversampling runs only around the nonlinear stages: the filter drive, the ladder and the post drive.
- **Rates and blocks:** output is finite and DC-free. It matches within tolerance at 44.1, 48, 88.2 and 96 kHz, and across 64-frame, 128-frame and odd-length block splits.
- **Accuracy:** pitch is within ±0.5 cent and envelope times within ±1 %, both checked against an independent oracle.
- **No clicks:** at note boundaries, voice steals, seeks, transport-generation resets and live edits. Every continuous parameter is smoothed.
- **CPU budget:** at most 15 % of one core on average at 48 kHz with 64-frame blocks. The test load is 16 notes with 8-voice unison on both oscillators through the ladder filter. Evidence is in `docs/performance/prism-synth.md`.
- **Real-time safety:** the callback performs no allocation, locking or I/O, and uses no Qt and no mutable project access.

## Presets

Prism ships 16 curated factory presets as versioned data, each with a golden-render test:

- Init
- Prism Default (warm poly keys)
- Supersaw
- Saw Lead
- Square Lead
- Reese Bass
- Sub Bass
- Pluck
- Soft Pad
- Brass
- Strings
- Bell (FM)
- Sync Lead
- Acid (mono with glide and ladder)
- Wobble (LFO synced)
- Juno Chorus Pad

A new MIDI track loads Prism Default.

## Workflow

- **Default instrument:** a new MIDI track uses Prism. The sampler and VST3 instruments stay selectable, and their behaviour and identities are unchanged.
- **Live edits:** continuous parameters edit live without dropping voices, through a bounded mailbox that acknowledges each revision. The UI keeps saved and audible state honest.
- **Topology edits:** a change such as unison count or filter type takes effect at the next note-on, or is shown as deferred. It is never applied through a silent reset.
- **Save and undo:** save, reopen and undo restore the exact patch intent.
- **Offline render:** uses the same MIDI timing and synth semantics as playback, including the release tail, chorus tail and track routing and effects.
- **Automation:** every continuous parameter can be automated through a native parameter-ID target.

## Platform (for future synths)

- **`native_synth` primitives.** These live in their own CMake target with a public interface:
  - sound: `BlepOscillator`, `ZdfSvf`, `ZdfLadder`, `Adsr`, `Lfo`, `Oversampler2x`, `Chorus`;
  - control: `Smoother`, `VoiceAllocator` (poly, mono, legato and sub-voices), `MidiVoiceState` (bend, CCs, aftertouch and sustain).
- **Engine contract.** Each engine provides a descriptor. Preparation runs on the control side and takes the parameters and sample rate to produce an immutable prepared engine. The runtime's `process` takes the output, block MIDI, transport generation and per-sample parameter lanes. Prism is engine `prism`, version 1.
- **Parameter descriptor table.** Each engine publishes a versioned table. Each entry gives:
  - a stable string ID, such as `prism.filter.cutoff`;
  - unit, range and taper;
  - default value and smoothing time;
  - its class (live-safe or topology);
  - discrete steps, where they apply.

  The same table drives validation, persistence, automation domains, the UI and preset migration.
- **Project model.** A single new `InstrumentSource` alternative: `NativeInstrumentPatch { engine_id, engine_version, parameters (id → value), mod_slots }`.
  - It is added with a schema minor bump and a migration test.
  - A missing parameter takes its default.
  - Each engine supplies an upgrader for its version bumps.
- **Automation.** A new `NativeInstrumentParameterTarget { track, parameter_id }` alternative of `AutomationTarget`. The sampler can move onto it later.

## Delivery order

1. This PRD, followed by a design note covering voice lifecycle, the anti-alias strategy and the live handoff.
2. DSP primitives with independent oracles.
3. Voice allocator, MIDI state and the Prism engine, plus a CPU and allocation benchmark (`docs/performance/prism-synth.md`).
4. The live parameter mailbox and topology deferral.
5. Model and persistence, as a coordinated commit: schema 1.29→1.30, a migration test, the set-patch operation and undo.
6. Routed live source, route plan and offline render integration.
7. The native parameter automation target.
8. Factory presets with golden renders, and Prism as the new-track default.
9. A standalone Quick `PrismPanel` with a narrow adapter hook into the project bridge.
