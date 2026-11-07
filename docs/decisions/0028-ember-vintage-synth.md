# ADR-0028 — Ember: vintage analog synth (second native engine)

**Status:** Accepted; implemented; circuit model in ADR-0066. **Date:** 18 October 2026.
**Builds on:** ADR-0027 (Prism and the native-synth platform). **Requirements:** PRD §13; FS-04 PI-06–PI-08.

Ember is a warm poly synth in the spirit of classic 1980s Juno/Prophet-style instruments. It is the first sibling engine on the ADR-0027 platform, and it must ship with **no project-format, session, live-playback or export changes**. Only its parameter table, kernel, presets, registry entry and device skin are engine-specific.

**Goal:** a lush, hands-on vintage poly. It plays instantly, sounds "alive", and is cheap enough to stack.

**Non-goals for v1:** a modulation matrix, wavetables, MPE, user preset files, and exact circuit emulation of any trademarked product. Character is "inspired by"; there are no brand names in the UI.

### Voice
| Block | v1 specification |
|---|---|
| DCO A (Juno-style) | Mixable saw, pulse and sub-square levels; pulse width with LFO or envelope PWM; noise. Octave range 16'/8'/4'. |
| VCO B (Prophet-style) | Saw, pulse and triangle; coarse and fine tune; hard sync to A; low-frequency mode, where it becomes a second LFO. |
| Poly-mod | Filter envelope and VCO B act as sources for A's frequency, A's pulse width and the cutoff. |
| HPF | Four-position non-resonant high-pass, like the Juno (0 / 1 / 2 / 3). |
| Filter | New OTA 4-pole low-pass (IR3109-style per-stage saturation, resonance bass loss, self-oscillation). Also a new 12 dB SEM-style state-variable filter with LP→notch→HP morph. Controls: cutoff, resonance, envelope ±, LFO, keytrack, velocity. |
| Envelopes | One ADSR for the filter and modulation. The amp is either an ADSR or Gate (a Juno-style switch). Analog-style RC curves. |
| LFO | Triangle, square, sample-and-hold or saw. Rate, delay/fade, free or retriggered, tempo sync. Hard-wired to pitch, PWM and cutoff. |
| Age macro | One knob, 0 to 100 %. It scales per-voice pitch drift (slow, random), oscillator phase jitter, cutoff and envelope-time tolerance per voice, a little voice-to-voice detune, and the DCO noise floor. Values come from deterministic seeds per voice slot, so export equals playback. |

### Voicing
- **Polyphony:** 8 voices with vintage round-robin allocation. Voices differ audibly through Age.
- **Unison:** stacks all 8 voices, detuned by Age plus a unison detune control.
- **Chord memory:** the held chord is stored, then transposed by single notes.
- **Glide:** portamento, with mono and legato behaviour in Unison mode.
- **Bend and wheel:** bend range and mod-wheel depth for the LFO. Wheel, bend and pressure are automatable performance parameters, as in Prism.

### Built-in effects (series)
1. **Ensemble:** a three-phase Solina-style string ensemble with amount.
2. **Chorus:** reuses Prism's Juno chorus with modes I, II and I+II. Adds an optional BBD hiss amount.
3. **Delay:** a stereo BBD-flavoured delay. Tempo-synced or free time, feedback with a darkening filter, mix, and ping-pong.
4. **Tape/console:** gentle saturation with drive, plus a wow/flutter amount.
5. **Output:** output level and the safety knee.

### Quality bar (tested)
- **Oscillators:** the existing BLEP oscillators; aliasing targets as for Prism.
- **OTA filter:** 2× oversampled. Small-signal response matches four cascaded one-pole stages. Self-oscillation is bounded and near the cutoff.
- **SEM filter:** matches its bilinear analog prototype.
- **Age:** zero Age is fully deterministic and stable. Any Age value renders identically across block splits and between export and playback.
- **CPU:** at most 8 % of one core at 48 kHz with 64-frame blocks, with all 8 voices, Unison, every effect on, and Age at 100 %.
- **Inherited contracts:** no allocation in the callback; a 32-frame control grid; next-note deferral; declicked steals and seeks.

### Presets
16 presets with golden fingerprints:
- **Keys and leads:** Ember Default (warm poly), Juno Strings, Solina Ensemble, Brass Section, Poly Keys, Sync Lead, Unison Lead.
- **Bass:** Bass Pluck, Fat Unison Bass.
- **Pads:** Sweep Pad, Dark Pad, Chord Stab (chord memory), Dream Delay.
- **Character:** Hoover-ish Age Max, Tape Keys, Init.

### Platform proof
- **Ember's own code:** a parameter table (`ember.*` ids), a kernel, presets and a registry entry. Nothing else is engine-specific.
- **Generic panel:** the Prism panel's controller and session adapter become engine-generic, taking an engine descriptor. Each engine supplies its own QML skin and small curve helpers.

## Delivery order

1. This PRD.
2. New DSP primitives with independent oracles: OTA 4-pole, SEM state-variable filter, four-position HPF, ensemble, BBD delay, tape saturator and drift source.
3. The Ember engine, its tests and a CPU benchmark (`docs/performance/ember-synth.md`).
4. A platform proof: the session, routed-playback and export-parity tests run against every registered engine, with no changes to the shared layers.
5. The Prism panel controller and session adapter become engine-generic.
6. The Ember Quick panel, in a warm vintage skin.
7. The sixteen presets with golden fingerprints.

## Decisions taken during implementation

- **Chord shapes, not chord capture.** `ember.voice.chord` selects a shape: major, minor, major 7, minor 7, sus 4, fifths or octaves. Capturing a held chord is transient performance state. It could not be saved or exported deterministically, and clips carry no controller data to trigger it.
- **Mono voices.** Each voice card is mono, with one oversampled filter, as in the hardware. Stereo comes from the unison pan spread, the ensemble, the chorus and the ping-pong delay. This is also why Ember's CPU budget is lower than Prism's.
- **Amp mode.** The options are Env (shared with the filter envelope, Juno-style), Gate, and Own (a separate ADSR, Prophet-style).
- **OTA saturation.** Only the input and the first two stages saturate, using a division-free cubic. Saturating every stage doubled the serial cost per sample, with little audible gain once the early stages have shaped the level.
- **Hiss follows the signal.** BBD hiss follows a 150 ms signal envelope, as the hardware compander does. Silence is silent, so idle tracks and export tails end cleanly.
- **Fixed tape latency.** The tape stage always runs through its 3 ms delay. Wow and flutter can then rise from zero without combing against a dry path. The latency is the same in playback and export.
- **Delay ping-pong is switchable** (`ember.fx.delay_pingpong`, on by default). *Amended 19 October 2026:* v1 had it fixed on. A switch crossfades how the delay lines are fed over 20 ms, so it never writes a step that would come back as a click.

Evidence is in `docs/performance/ember-synth.md`, `native_vintage_dsp_contract`, `native_ember_contract` and the engine-parameterised `application_routed_synth` and `application_synth_export_parity` tests.
