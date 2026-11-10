# ADR-0118 — Strike: a single-band and multiband transient shaper

**Status:** Accepted and implemented. **Date:** 10 November 2026.
**Builds on:** ADR-0040 (native effect platform), ADR-0082 (Strata's crossovers and linear-phase bank), ADR-0103 (shared device panel parts), ADR-0108 (Edge's curve and oversampler), ADR-0117 (the limiter audit).
**Contract:** `include/oscillate/native_dsp/effects/strike.hpp`, the table `src/native_dsp/effects/strike_table.hpp`; the shared bank `src/native_dsp/effects/linear_bank.hpp`. No schema change.

## Context

Drums need their attack brought forward or rounded off and their ring shortened or lengthened, without a threshold that has to follow the level: a kick mic, a room mic and a mastered mix sit 30 dB apart. On a bus or the master the same shaping must stay within a band, so the snare's crack can rise without the cymbals or the sub rising with it. Press and Strata change level against a threshold; Edge and Ceiling only take peaks down.

## Decision

1. **A native effect, Strike (id `strike`),** on the effect platform: a parameter table and a kernel, no document, no sidechain. Parameters: Bands (1 to 4), Crossover 1–3 (30 Hz to 16 kHz, a third of an octave apart at least), Phase (Minimum or Linear), Attack Time (1–100 ms), Sustain Time (20 ms to 2 s), Lookahead (off, 0.5, 1, 2 or 5 ms), Stereo Link, Mix, Output (−24 to +12 dB), Clip and Ceiling (−12 to 0 dBFS); per band Attack and Sustain (±18 dB) and Solo. Bands, Phase, Lookahead and Clip are not automatable: the last three set the latency.
2. **The detector reads ratios, not levels.** Per band and channel:
   - **Level:** the band's analytic power, from a pair of allpass chains 90° apart within 0.7° from 20 Hz to 23.5 kHz at 48 kHz (O. Niemitalo's Hilbert pair), smoothed over 0.5 ms. A steady sine has a steady analytic power, so a held note reads neither attack nor sustain at 40 Hz or 10 kHz. At higher rates the pair's lowest octave rises in proportion.
   - **Attack weight:** a follower rises towards the level with Attack Time and falls back with 20 ms. The level's rise over it, in dB, is the attack.
   - **Sustain weight:** a follower takes the level at once and falls with Sustain Time. The level's fall under it, in dB, is the sustain. Decays faster than Sustain Time count; slower ones are left alone.
   - Each weight is 0 up to 1 dB and 1 from 7 dB, linear in dB between, so noise's flicker does not count and a hit from silence counts fully. The band's gain is Attack × attack weight + Sustain × sustain weight, in dB, smoothed over 0.5 ms; at rest it lands on exactly 0 dB.
   - Both weights are power ratios, so scaling the input scales nothing else. This differential envelope (a fast level against a slow follower) was chosen over a fixed-threshold onset detector because it needs no threshold at all.
   - Stereo link moves each channel's level towards the louder channel's before the followers; fully linked, both channels share one gain.
3. **Bands.** One band is no split. Two to four bands split by Phase:
   - **Minimum:** a phase-compensated Linkwitz-Riley tree (fourth-order, ADR-0082), zero latency. The bands sum to one allpass.
   - **Linear:** Strata's linear-phase FIR bank, now shared (`LinearBank`): differences of 2047-tap Blackman-windowed sinc low-passes, by FFT in 512-frame hops. The bands sum to the input delayed by 1535 frames (32 ms at 48 kHz).

   The allpass turns low frequencies later. On the limiter audit's drum loop, three minimum-phase bands at zero settings lowered the low band's onset contrast by 1.1 dB and the middle's by 0.4 dB. Linear phase changes neither, so the bus and master presets use it, and the minimum phase stays for tracks where latency matters.

   Each band's gain applies to that band; Mix blends each band with itself unshaped, so a partial mix does not comb. Solo plays only the soloed bands, fading over 20 ms.
4. **Lookahead** delays the bands against their detectors, so the gain is in place when the hit arrives. Two milliseconds lift the first cycle of a 1 kHz hit by the whole setting.
5. **Clip** passes the sum, after Output, through Edge's curve at half its widest knee, under the ceiling, four times oversampled through Edge's light filters (0.77 ms). Added punch then cannot overshoot: a hit lifted 12 dB into a −6 dBFS ceiling peaks 0.15 dB over it, where unclipped it would pass 6 dB over.
6. **Telemetry:** input and output peaks per channel (held 1 s), how far Clip took the loudest peak down, the latency, the band count and the crossovers, each band's gain now, and a scrolling view of the last 2 s in 256 slices: the input's and the output's peak and each band's gain furthest from 0 dB.
7. **Presets,** each measured on the hits it is for (the onset contrast: the first 15 ms against 30–150 ms on):
   - Kick Snap: attack +6 dB, sustain −2 dB, 6 ms, 1 ms lookahead.
   - Snare Crack: attack +7 dB, 8 ms, 1 ms lookahead.
   - Tighten Room: sustain −8 dB over 500 ms.
   - Drum Bus Punch: three linear-phase bands at 150 Hz and 2.5 kHz, +3/+4/+2 dB attack, a little sustain cut, clipped under −0.3 dBFS.
   - Master Impact: four linear-phase bands, 1–2 dB of attack, clipped under −1 dBFS.
   - Soften Attack: attack −6 dB, sustain +2 dB.

   Each moves the contrast its way by 1 to 10 dB (Master Impact by 0.3 or more), and the clipped ones stay under their ceiling.
8. **Panel** (`qml/strike`): the scrolling view with the input as a ghost under what Strike let out, each band's gain over them in its colour (lifts rise, cuts fall) and the ceiling when clipping; the input, output and clip meters; the band count, phase and crossovers (unused ones idle); a column per band with Attack, Sustain, solo and the gain now; the timing with the lookahead and the latency (any effect's latency through `effect_latency_ms`); and the output with mix, level, link, clip and ceiling.

## Consequences

- **Tests:** `native_strike_contract`:
  - at zero settings: one band is bit-exact; the minimum-phase bands sum flat within 0.01 dB from 20 Hz to 20 kHz; the linear-phase bands sum to the input delayed;
  - the measured gains: attack within 0.04 dB and sustain exact at −40, −20 and 0 dBFS, the same at every level; a steady tone left alone;
  - the timing: a 6 dB lift stays over half for 2, 6 and 26 ms at 2, 10 and 50 ms of Attack Time; Sustain Time counts a 50 ms decay partly at 30 ms and fully from 300 ms;
  - the bands: each band's lift reaches another band's hit only through the crossovers' skirts; solo; stereo link; mix; clip; latency exact for every phase, lookahead and clip; lookahead lifts the first cycle;
  - block splits, export against live, hostile input, a bounded tail, restrained presets, a seek, no allocation and the telemetry.

  `quick_strike_panel_contract` checks every parameter's control, the text, the controls, the telemetry, the real Strike, the presets and status, and takes screenshots with `STRIKE_PANEL_SCREENSHOT_DIR`. `effect_export_parity` and the platform contracts cover Strike with every engine.
- **Audit:** `oscillate_limiter_audit --shaper` measures a transient shaper's presets at the programme's own level: the loudness, crest factor, onset contrast per band and true peak each preset changes, with latency and CPU (`docs/performance/native-effects.md`).
- **CPU:** one band costs about 0.35% of a core. The linear-phase bank runs its FFTs once per 512-frame hop, so its blocks are uneven: one Master Impact instance averages 5.3% with a 99th-percentile block of 13–21% of its period. It is for a bus or the master, not every track.
- Strata's linear-phase bank moved, unchanged, into `linear_bank.cpp`, shared by both effects.
