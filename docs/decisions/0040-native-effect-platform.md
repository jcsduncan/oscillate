# ADR-0040 — A generic native effect platform

**Status:** Accepted; implemented. Model, schema, routing and export integration follow [ADR-0043](0043-one-native-insert-path.md). **Date:** 19 October 2026.
**Builds on:**
- ADR-0023, the unified insert chain;
- ADR-0027, the native synth platform;
- ADR-0034, stepped automation;
- ADR-0037, telemetry;
- ADR-0038, live switching.

## Context

Requirement: native effects built on a shared platform, each with its own designed panel like the instruments. The first set is:
- Filter;
- Saturation;
- Modulation;
- Delay;
- a much better reverb, in the class of Valhalla's reverbs.

Main has five native effects: EQ, compressor, reverb, limiter and convolution. Each is a bespoke settings struct in `EffectProcessor`, hand-switched in about 60 places across the model, codec, routed playback, export and Quick. None of their parameters can be automated, because no automation target addresses a native effect. Adding five more effects the same way would multiply that plumbing five times.

## Decision

**One generic insert type, driven by tables.** Effects use the same pattern as the native synths: each effect is a versioned parameter table plus a DSP kernel.

- **Model.**
  - `NativeEffectPatch{engine, engine_version, parameters, generation}` is appended to `EffectProcessor` and `NativeEffectProcessor`.
  - Its parameters are sparse `{id, value}`, like a native instrument's.
  - Validation resolves them against the engine's table.
- **Automation.** `NativeEffectParameterTarget{slot, parameter_id}` addresses one parameter of one effect slot (slot ids are project-unique). Its domain comes from the descriptor, so discrete parameters automate as stepped lanes (ADR-0034).
- **Persistence.** The next free schema minor adds:
  - the effect kind `native_effect` (`{generation, engine, engine_version, parameters}`);
  - the target kind `native_effect_parameter`.

  Older files load unchanged.
- **DSP** (`native_dsp/effects`):
  - `EffectKernel` mirrors `SynthKernel`: control ticks on the 32-frame grid, in-place stereo processing with an optional sidechain, reset, status and telemetry.
  - `EffectEngine` carries the descriptor, `create_kernel`, `tail_frames` and `latency_frames`.
  - `NativeEffectRuntime` is a `SidechainProcessorRuntime`. It reuses the synth runtime's patterns:
    - the values mailbox with glide;
    - the per-frame parameter lanes;
    - the telemetry triple buffer;
    - a seek reset on `state_generation`.
  - A registry lists the engines.
- **Routing, playback and export.** Every per-type switch gains one generic branch: build a `NativeEffectRuntime` from the registry. Latency and tail come from the engine.
  - **Live edits** are offered to the running runtime's mailbox; a running runtime is never rebuilt for a parameter edit.
  - **Edits that change latency** go through the existing rebuild path.
  - **Automation** of effect parameters is prepared per block, as the synth player prepares it.
- **Quick.**
  - The insert chain projects `nativeKind: "effect"` with the engine id.
  - The add-effect browser lists the registry.
  - Opening an effect shows its own designed panel, through a descriptor-driven device controller shared with the synths.
- **The existing five stay as they are** for now. They can move onto the platform later, one at a time.

## First engines

| Engine | Quality bar (tested) |
|---|---|
| **Filter** | Ladder, SVF, OTA and comb models from the synths. LFO, envelope follower and sidechain drive. Responses checked against analytic prototypes, and switching is click-free. |
| **Saturation** | Tape, tube, transistor, fuzz, wavefold and bitcrush, oversampled. Aliasing below −90 dBFS for test tones. DC-free, and output trim stays level-matched. |
| **Modulation** | Chorus, ensemble, flanger (including through-zero) and phaser. Modulation stays artefact-free, tempo sync and mono compatibility are checked, and delay timing is exact. |
| **Delay** | Digital, tape and BBD, with ping-pong and multi-tap. Tempo sync, feedback filtering, saturation and wow are in the loop, plus freeze and ducking. Times are exact, and time changes bend pitch smoothly instead of clicking. |
| **Reverb** | A modulated FDN design with diffusion and early reflections. Modes run from Room and Hall to Plate, Chamber, Cathedral, Shimmer and vast "cloud" spaces. Per-band RT60 matches the setting, echo density builds, the stereo field decorrelates and modulation stays artefact-free. |

Every engine inherits the platform contracts:
- block-split invariance;
- export equal to live playback;
- no allocation in the callback;
- a tail bound;
- click-free switching;
- goldens.

Each gets a designed panel, built by a fork, with screenshots.

## Consequences

- **A one-time generic pass** through the about 60 per-type sites. After it, adding an effect is only its table, kernel, presets and panel, with no plumbing.
- **Effect parameters become automatable.**
- **One schema bump.**
- **Shared files** (`routed_playback_control.cpp`, the export path, `project_bridge_inserts.cpp`) change in one focused pass to keep the integration small.

## As built: stage 1 (DSP)

The platform and five engines are in `oscillate::native_effects` (`src/native_dsp/effects`):
- the runtime (`NativeEffectRuntime`) and its registry;
- the engines: Contour, Forge, Orbit, Relay and Expanse.

The runtime shares `ParameterControl` and `TelemetryBuffer` with the synths, which were extracted from `NativeSynthRuntime` unchanged.

`tests/native_dsp/effect_platform_tests.cpp` runs the platform contract over every engine and preset:
- registry consistency;
- bit-identical block splits;
- silence in, silence out;
- the tail bound (−60 dB);
- seek reset;
- constant lanes equal to panel values;
- rejected blocks leave audio untouched;
- offers;
- no allocation.

| Engine | As built | Measured |
|---|---|---|
| **Contour** (filter) | Eleven models from the synth library: SVF LP/BP/HP/notch, ladder, OTA, SEM and ±comb. 2× oversampled, with a phase-aligned dry path. Synced stereo LFO; envelope follower on the input or sidechain. Live model crossfades. | SVF modes are within 0.25 dB of Butterworth and band-pass analytics. The follower's sweep is within 1 semitone of its 60 dB mapping. |
| **Forge** (saturation) | Tape, tube, transistor, diode, clip, triangle fold, sine fold and rectify. Each curve has a closed-form antiderivative, for first-order ADAA at 16×. Colour is an exact emphasis/de-emphasis pair; auto gain matches −12 dBFS. | Audio-band aliasing is below −90 dBFS at any drive for smooth curves, with fundamentals up to 5 kHz. Folds and rectify meet it up to 24 dB of drive; past that their spectrum exceeds any oversampling. Level match is within 0.25 dB. |
| **Orbit** (modulation) | Chorus (1–4 voices), ensemble, dimension, flanger, phaser (2–12 stages), vibrato and tremolo/auto-pan. One per-sample LFO with sync and stereo offset. Lagrange-interpolated delays from always-fed lines. | The vibrato's demodulated delay swing is within 2%. Flanger combs, with ±0.7 feedback, are within 0.05 dB of analytic. Phaser notches fall where the allpass law puts them, below −50 dB. |
| **Relay** (delay) | Digital, tape and analog. Stereo, ping-pong or mono, up to 4 s, synced, with a right offset. In the loop: low/high cuts, ADAA saturation, BBD clock darkening and length-compensated diffusion. Also wow/flutter, ducking and width. | Echoes land to the sample, and repeats fall by the feedback within 0.05 dB. Digital time changes hold pitch within 0.5%; tape ones bend it. Wow matches its formula within 5%. Self-oscillation is bounded. |
| **Expanse** (reverb) | Room, hall, plate, chamber, cathedral and cloud. A 16-line Hadamard FDN with two allpasses per line, Jot shelf absorption and allpass-interpolated modulated reads. Damped input diffusion, early reflections, shimmer in the loop, a lossless freeze, and spill-over on a change of space. | Octave-band RT60 is within 10% of design in every space and at any size. Echo density reaches 0.9 in 20–60 ms (cloud blooms in ~110 ms). Late channels are decorrelated (\|ρ\| < 0.05). Tail colouration is within 3 dB of ideal decaying noise. |

| **Vista** (stereo width, added after the first five) | Mid and side run through the same phase-compensated Linkwitz-Riley tree (mono bass split plus three crossovers). Up to four bands scale the side only. Stereoize (diffuse decorrelator or Haas) runs above 300 Hz, side only. Safe width is a correlation floor that never narrows the input. Rotation, balance, swap and polarity. Meters: exact per-band correlation and a goniometer feed. | The mono sum is identical (1e-6) under any width, band, stereoize, mono-bass or safe setting. At 100%, mid stays mid and side stays side, flat within 0.01 dB. Band widths are within 0.3 dB, and mono bass follows the LR4 slope within 0.3 dB. Stereoize brings white noise to \|ρ\| < 0.15; the floor holds within 0.03. |

**Departures from the plan:**
- **Forge has no fuzz or bitcrush.** A clean antialiased bitcrusher is a different effect, and fuzz is covered by diode and clip with bias.
- **Orbit has no through-zero flanger.** It needs a latency that changes with the mode, which the platform keeps fixed per instance.
- **Relay has no multi-tap or freeze.** They're left for a later version.

Every engine reaches its quality bar with tests. CPU per instance at 48 kHz is in `docs/performance/native-effects.md`.
