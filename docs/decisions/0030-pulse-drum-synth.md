# ADR-0030 — Pulse: analog-modelled drum synth (fourth native engine)

**Status:** Accepted; implemented. **Date:** 18 October 2026.
**Builds on:** ADR-0027 (the native-synth platform), ADR-0028 (Ember) and ADR-0044 (Lumen).

Pulse is a sample-free drum machine on MIDI keys C1 to B1. Like every native engine, it adds no project-format, session, playback or export changes.

**Goal:** a punchy, instantly usable analog-style drum machine on MIDI keys. It plays great with defaults and is shaped quickly per pad.

**Non-goals for v1:** sample playback, per-pad multi-output (parameter ids are designed so it can be added later), a built-in step sequencer (MIDI clips do sequencing), MPE.

### Pads (12, fixed keys from C1 = MIDI 36, GM-like)
| Key | Pad | Voice model |
|---|---|---|
| C1 | Kick | Bridged-T sine with pitch envelope; tone, decay, sweep, click, drive |
| C#1 | Kick 2 (909) | Sine plus noise-burst transient, faster sweep |
| D1 | Snare | Two tuned resonant bodies plus filtered noise; snappy and tone balance |
| D#1 | Rim | Short resonant ping with high-pass |
| E1 | Clap | Three or four fast noise bursts, a reverb-like tail, band-pass |
| F#1 | Closed hat | Six inharmonic square oscillators (808 ratios) into band-pass and high-pass; chokes the open hat |
| A#1 | Open hat | Same metal source, longer decay |
| F1, G1, A1 | Low / Mid / High tom | Pitched sine body with pitch drop, plus noise |
| G#1 | Cowbell | Two squares (about 540 / 800 Hz) into band-pass |
| B1 | Cymbal | Metal source, longer decay, brighter filter |

Every pad shares a core set of parameters: level, pan, tune (±24 st), decay, tone, drive, velocity amount and a per-pad character amount (snap, click and the like).
- **Velocity:** sets level and brightness.
- **Retriggers:** each pad is mono; a retrigger declicks over 64 frames, like hardware.
- **Choke:** the closed hat chokes the open hat.

### Kit bus
- A per-kit **glue compressor** (amount and attack-release character) and **saturation**.
- No reverb of its own (removed 21 October 2026). The built-in room rang metallically on every transient; send the kit to a return with a reverb such as Expanse instead.
- Master level and the safety knee.
- Output is stereo; pad pans place each hit in the image.

### Quality bar (tested)
- **Analytic checks:**
  - Kick pitch-sweep start and end frequencies within 1 %.
  - Decay time to −60 dB within 5 %.
  - Hats show the six 808 metal frequencies and have no energy below their band.
- **Determinism:** noise is seeded per pad and hit index, so renders are deterministic. Any block split renders identically, and export equals live playback.
- **Steals and chokes:** retrigger and choke are click-free (bounded step).
- **CPU:** at most 5 % of one core at 48 kHz with 64-frame blocks, with all twelve pads ringing at once and the compressor on.
- **Inherited contracts:** no allocation in the callback; the 32-frame grid; declicked seeks.

### Kits (12 golden-fingerprinted presets)
808 Classic (default), 808 Boom, 909 House, 909 Techno, Electro, Lo-Fi, Trap, Industrial, Minimal, Dub, Breaks, Init.

### UI
- A drum-machine hardware faceplate: warm grey and beige, with a coloured pad strip.
- One column of per-pad knobs for the selected pad.
- Big pads that flash on click.
- Kit bus controls and a kit browser.

## Design notes

- **Pad voices.** Each pad is one mono voice. A retrigger hands the old voice's last output to a 64-frame declick and restarts the voice. A choke (the closed hat stopping the open hat) applies a fast 5 ms release, not a cut. Unmapped keys are ignored.
- **Determinism.** Noise generators are seeded by pad and hit count from the last reset, so playback after a seek and an export from the same point are identical.
- **Bus order.** Pads sum into glue compression, then saturation, the DC blocker, the output level and the safety knee.
- **Band-limited noise.** No pad adds full-band white noise. The kicks' click (their character) is noise in a knock band, 1.5–6 kHz for the 808 and 2–9 kHz for the 909, over a 3 ms decay. A tom's skin noise sits in 300 Hz–4 kHz and fades three times faster than its tone. The snare's noise falls at 24 dB/oct past its tone, and the clap's band-pass is closed above it by a low-pass. Each clap burst rises over 0.2 ms rather than in one sample. Full-band noise and instant bursts read as hiss and clicks on every hit, up to 24 kHz.
