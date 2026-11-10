# ADR-0122 — Gild: a multiband exciter on Forge's curves

**Status:** Accepted and implemented. **Date:** 10 November 2026.
**Builds on:** ADR-0040 (native effect platform, Forge's curves), ADR-0082 (crossovers and the linear-phase bank), ADR-0103 (shared device panel parts), ADR-0108 (Edge's oversampler), ADR-0117 (the limiter audit), ADR-0118 (Strike's band split).
**Contract:** `include/oscillate/native_dsp/effects/gild.hpp`, the table `src/native_dsp/effects/gild_table.hpp`; Forge's curves `src/native_dsp/effects/forge_curves.hpp`. No schema change.

## Context

Mastering and mix buses need harmonics added to one region (air over 8 kHz, body in the mids, the second and third harmonics of a bass for small speakers) without saturating the rest and without the level rising with the drive. Forge saturates the whole signal at zero latency and returns it at the curve's level; it has no bands, and its drive always bends the waveform.

## Decision

1. **A native effect, Gild (id `gild`), not a Forge mode.** It needs a band split, per-band controls, a latency set by its oversampling and phase, a per-band level match and its own telemetry; as a Forge mode these would put two control sets and two latency rules into one table and one panel. Gild shares Forge's curves, moved unchanged into `forge_curves.hpp`.
2. **Parameters:** Bands (1 to 4), Crossover 1–3 (30 Hz to 16 kHz, a third of an octave apart at least), Phase (Minimum or Linear), Oversampling (Edge's 1×–16×, default 4×), Dynamic, Level Match, Delta and Output (−24 to +12 dB); per band Drive, Curve (Tape, Tube, Transistor, Diode), Even, Mix and Solo. Bands, Phase and Oversampling are not automatable: the last two set the latency.
3. **Bands** split as Strike's: a phase-compensated Linkwitz-Riley tree (no latency of its own; the bands sum to one allpass) or the shared linear-phase bank (the bands sum to the input delayed by 1535 frames).
4. **Only the curve's bend is added.** Each band goes up through Edge's linear-phase half-bands (120 dB stopband). There it is driven by gain *g* = 10^(36/20) × Drive³, that is 36 + 60·log₁₀(Drive) dB (25 % is about 0 dB, 50 % +18 dB), biased by *b* = 0.5 × Even, and normalised to unit slope at the bias: (f(b + g·u) − f(b)) / (g·f′(b)). Small signals pass at their own level. The residue, that less the input, goes back down and is added to the band delayed by the round trip. A band at zero drive adds exactly nothing, and one band at zero drive is the input bit for bit, delayed.
   - **Antialiasing:** from 8× the curve is replaced by its mean over each step (first-order antiderivative antialiasing) and compared with the mean of the inputs, which Edge's top decimator brings back into line.
   - **Even:** a bias makes the symmetric curves asymmetric. The residue splits into its odd and even parts in the input (the curve at b + g·u and at b − g·u). Only the even part, which carries the DC, passes a 5 Hz blocker. A blocker on the whole residue turned the curve's share of a 60 Hz fundamental by several degrees, which moved the hard-driven fundamental by 1.7 dB.
   - **Curve changes** fade over 20 ms between the two residues.
5. **Level Match** follows the dry band's and the excited band's powers over 0.4 s and scales the excited band by the root of their ratio, within ±36 dB, so drive changes the colour, not the level. **Mix** blends each band with its dry signal. **Dynamic** (0–100 %) drives a band up to 12 dB harder as its power over 80 ms falls under −18 dBFS RMS, so quiet passages keep their harmonics. **Delta** plays only what is added. Solo plays only the soloed bands, fading over 20 ms.
6. **Telemetry:** input and output peaks; the latency, band count and crossovers; per band its level and what it added (RMS over 0.4 s) and the drive reaching its curve. Also the dry input's and the added signal's spectra: a 4096-point Hann transform every 2048 frames, 96 log-spaced bins from 20 Hz to 20 kHz, smoothed over 0.3 s.
7. **Presets,** each exciting one region and leaving the rest dry:
   - Air Lift: over 8 kHz, tape, Drive 65 %, Even 30 %, Mix 60 %, Dynamic 50 %, 8×.
   - Warm Mids: 250 Hz to 2.5 kHz, tube, Drive 40 %, Even 60 %, Mix 50 %, Dynamic 50 %.
   - Bass Weight: under 120 Hz, tube, Drive 40 %, Even 70 %, Mix 50 %, Dynamic 50 %.
   - Tape Glue: four linear-phase bands through tape, Drive 20–25 %, Even 30 %.
   - Vocal Presence: 1.5 to 6 kHz, transistor, Drive 55 %, Even 20 %, Mix 50 %, 8×.
8. **Panel** (`qml/gild`): the spectrum, with the programme as a ghost and what Gild adds in gold over the bands' shading; the input, output and added meters; the band count, phase and crossovers; a column per band with its curve, Drive, Even, Mix, Solo, what it adds against its own level and its drive; the oversampling with the latency; and the output.

## Consequences

- **Tests:** `native_gild_contract`:
  - zero drive: one band is bit-exact at every factor; the minimum-phase bands sum flat within 0.01 dB; the linear-phase bands sum to the input delayed;
  - harmonics: what each curve adds (H1 to H5 under 20 kHz) at three drives and two Evens, with a sine in each of four bands, matches the static curve written out independently in the test, within 0.1 dB; Even raises H2 against H3, and with none the symmetric curves add no H2 (under −100 dBFS);
  - aliasing: a 10.5 kHz sine at −6 dBFS through every curve at 16×: the worst line under 20 kHz is −118 dBFS at full drive and −138 dBFS at half; at 1× the measurement sees the folding;
  - Level Match holds the level within 0.1 dB; Dynamic narrows the third harmonic's spread over 20 dB of level from 39.5 to 15.5 dB;
  - each band's drive stays in its band; solo, mix, delta; latency exact, and the excited fundamental's phase within 0.3° of the dry one's at 5 kHz;
  - block splits, export against live, hostile input, a bounded tail, restrained presets, a seek, a band that rejoins starts clean, no allocation and the telemetry.

  `quick_gild_panel_contract` checks every parameter's control, the text, the controls, the telemetry, the real Gild, the presets and status, and takes screenshots with `GILD_PANEL_SCREENSHOT_DIR`.
- **Audit:** `oscillate_limiter_audit --exciter` measures an exciter's presets at the programme's own level: the loudness change, what is added against the programme overall and in three bands, and the true peak change, with latency and CPU (`docs/performance/native-effects.md`).
- **Latency:** the oversampler's round trip is 1.65, 1.85, 1.92 and 1.94 ms at 2×, 4×, 8× and 16× (48 kHz); 1× has none. Linear phase adds the bank's 32 ms.
- **CPU:** the oversampling dominates. A band costs about 4 % of a core at 4× and 7 % at 8×. A band at zero drive sleeps once its resampler and blocker have drained.
- **Allpass:** at minimum phase the split changes the waveform even at zero drive; on the mastered tracks it raised the true peak by about 2.5 dB. The master preset (Tape Glue) uses linear phase.
