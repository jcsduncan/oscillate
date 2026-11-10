# ADR-0119 — Keel: adaptive tonal balance toward a target curve

**Status:** Accepted and implemented. **Date:** 10 November 2026.
**Builds on:** ADR-0040 (native effect platform), ADR-0042 (Lens's band powers, where pink noise reads flat), ADR-0064 (Temper's dynamic EQ), ADR-0103 (shared device panel parts), ADR-0117 (the limiter audit).
**Contract:** `include/oscillate/native_dsp/effects/keel.hpp`, the table `src/native_dsp/effects/keel_table.hpp`. No schema change.

## Context

A master needs its long-term tonal balance (how much low, mid and top it has against a reference shape) held steady across a track and from track to track. Shape is a static EQ that someone has to set by ear. Temper reacts in milliseconds to resonances that stand out from their neighbours; it does not know what the whole spectrum should look like, and it only cuts. Nothing compared the programme's balance with a target and corrected it.

## Decision

1. **A new native effect, Keel (id `keel`),** rather than a Temper mode. Temper's controls are about time (attack, release, transients against sustain) and narrow peaks. Keel works on a scale of seconds, over the whole spectrum, toward a target, and it boosts as well as cuts. A mode would have put two control sets and two latencies (Temper's FIR paths have up to 27 ms; Keel has none) into one table and one panel. Keel has a table and a kernel, no document, and it reads a sidechain for its reference target. Parameters:
   - Amount (0–100%), Speed (0.5–30 s), Max Boost and Max Cut (0–18 dB each);
   - Target (Tilt, Curve or Reference) and Tilt (−6 to +6 dB/oct about 1 kHz);
   - Low, Mid and High weights (0–100%), with Low Split (60–600 Hz) and High Split (1.5–10 kHz);
   - Level Match, Output (±12 dB) and Delta;
   - the curve: 31 values, one per third-octave band (±24 dB).

   Target, Level Match, Delta and the curve are not automatable.
2. **The target grid** is the interface other target sources write to: 31 third-octave bands at 1 kHz × 2^((b − 17)/3), from 19.7 Hz to 20.2 kHz (`keel::band_hz`). A target is one dB value per band (`keel::Curve`), and only its shape counts. `keel::set_curve` writes a curve into the parameters; `keel::target_curve` reads the target back. Genre targets measured from references (#346) are curves on this grid.
3. **Tracking.** A Hann-windowed STFT of both channels: 16384 points every 2048 frames at 48 kHz, scaled with the rate. Each band's power is the sum of the bins inside its edges (the band centre × 2^(±1/6)), with partial bins weighted, so pink noise reads flat and the 20 Hz band holds one and a half bins.
   - **Gates.** Each hop's K-weighted band powers give it a loudness. A hop under −70 dB is ignored. A recent loudness follows every other hop over Speed. A hop more than 20 dB under that loudness is left out of the balance, so fades and gaps do not drag it. A hop more than 6 dB over it is scaled down to that level, so a burst counts as no more than a loud bar.
   - **Averaging.** Band powers average over Speed seconds: a running mean until a Speed has passed, then an exponential average. The balance is smoothed a third of an octave each way.
   - **The window waits until it has filled** after a start or a seek, so zeros never read as a fade-in.
   - **The analysis is spread across the hop.** Each window's transform is windowed, reordered and computed stage by stage, a share every 16 frames, and is ready by the next hop. `dsp::Fft` gained a staged forward transform for this, bit for bit the same as `forward()`. One instance's 99th-percentile 64-frame block fell from 26% to 2.6% of its period.
4. **Comparison.**
   - **Presence.** Bands more than 45 dB under the K-weighted power mean hold no content. They are left out of the means and never boosted. A low-passed programme is not given back a top it never had.
   - **Alignment.** The balance and the target are each centred on their K-weighted mean over the bands with content. The correction is −Amount × the region weight × (balance − target), within −Max Cut and +Max Boost.
   - **Regions.** The region weights cross over an octave centred on each split.
   - **Reference.** The target is the sidechain's balance (tracked the same way) plus the tilt. Until a reference has been heard the correction holds at 0 dB.
5. **Filtering: minimum phase, no latency.** The correction feeds a bank of eleven trapezoidal state-variable filters (A. Simper's form): a low shelf at 20 Hz, bells at Q 0.6 an octave apart from 31.25 Hz to 8 kHz, and a high shelf at 12 kHz.
   - **Fitting.** Each hop the gains are fitted to the correction by weighted least squares on the bands' exact digital responses. The outer bands (20 Hz, 25 Hz, 20 kHz) weigh a quarter, and each fit refines the last hop's twice.
   - **Ripple.** Fitted to a 3 dB/oct tilt held within 18 dB, the bank ripples 0.25 dB from 40 Hz to 16 kHz. Narrower bells, or an octave-spaced bank without the 31.25 Hz bell, ripple more (0.8–1.4 dB).
   - **Gliding.** The gains ramp across each hop and are smoothed over one more. The filters are designed every 16 frames and their coefficients glide frame by frame between designs. Designing every 16 frames without gliding left a zipper floor 88 dB under a multitone. The ramps alone left the hop rate (23 Hz) audible as sidebands 77 dB under it.
   - **At 0 dB a section's output is its input exactly,** so Amount 0 is bit for bit.
   - **Why not linear phase.** Converged on a 4 dB lift at 80 Hz, Keel's measured impulse response delays the 63 Hz band by 1.5 ms, no band from 31.5 Hz up by more than 2 ms, and none from 200 Hz up by more than 0.15 ms. A linear-phase filter that resolves the 20 Hz band would need about 170 ms of taps: about 85 ms of latency, and pre-echo before every kick. Minimum phase is kept.
6. **Level Match** estimates the loudness the correction adds from the tracked band powers, K-weighted, and takes it out. The gain ramps across the hop.
   - **Output** trims after everything, gliding over 10 ms.
   - **Delta** plays the output less the input.
7. **Learn** (`keel::learned_curve`; the controller's `keel_learn`) makes the balance in the latest telemetry the target. It writes the curve (the balance less the tilt) and sets Target to Curve, all as one edit.
8. **Telemetry:**
   - input and output peaks;
   - the latency (0) and Level Match's gain;
   - whether the input and a reference have been tracked, and the target source;
   - per band the target and the tracked balance, each centred, and the correction now;
   - 48 slices of the correction, one every half second.
9. **Presets,** each at most 4 dB either way, at equal loudness:
   - Default: Amount 50%, 4 dB either way, tilt −2.5 dB/oct.
   - Gentle Polish: 35%, 2 dB, 10 s.
   - Tame Harsh: cuts only, mostly above 3 kHz.
   - Warm It Up: tilt −3 dB/oct, 3 dB.
   - Streaming Balance: 3 dB, 8 s.
   - Low End Control: the lows alone, under 250 Hz.

   The default tilt is measured: on the eight-track corpus the masters fall 2.7 dB/oct against pink noise from 63 Hz to 12.5 kHz (−1.9 to −4.6).
10. **Panel** (`qml/keel`):
    - **The balance view.** Centred bands on a log axis: the tracked balance as a ghost, the target, the correction as amber and sky bars, and the balance Keel lets out. On Curve the same view edits the curve: drag a band, or choose one with the arrows and move it.
    - **The history.** The correction over the last 24 s as a band-by-time map.
    - **Meters.** The peaks and Level Match.
    - **Controls.** Steer (Amount, Speed, limits); Target with Learn and Flatten; Regions; Output.

## Consequences

- **Tests:** `native_keel_contract` (26 cases):
  - **Convergence.** Pink noise tilted ±3 dB/oct converges to a flat target within 1 dB from 40 Hz to 16 kHz in under 2 s at Speeds of 1, 4 and 15 s. It ends within 0.5 dB, and a Welch analysis of the output agrees within 1.25 dB.
  - **Following.** A reversal from −3 to +3 dB/oct is followed within 1 dB after 6.2–6.6 Speeds (the bands average power).
  - **Passthrough and stillness.** Amount 0 is bit for bit. On steady pink noise the correction wanders 0.25 dB RMS or less (less when slower), and what moves faster than a second stays under 0.05 dB RMS. A periodic multitone holds its correction within 0.05 dB.
  - **No zipper.** While a reference turns the correction over, a multitone gains no sidebands more than 40 Hz from its tones above −100 dB.
  - **Phase.** The measured group delay stays under 2 ms from 31.5 Hz up and under 0.25 ms from 200 Hz up.
  - **Targets.** The curve, reference and learned targets are reached.
  - **Controls.** Regions, limits, absent bands, the gates (silences move the correction under 0.25 dB; rare bursts move it less than half as far as they move a plain average), Level Match (within 0.5 LU), Delta and Output.
  - **Other rates.** 44.1 and 96 kHz converge alike.
  - **Platform.** Block splits, export against live, hostile input, a bounded tail, restrained presets, a seek, no allocation and the telemetry.

  `quick_keel_panel_contract` covers the panel, Learn and screenshots (`KEEL_PANEL_SCREENSHOT_DIR`). The platform contracts and `effect_export_parity` cover Keel with every engine.
- **Audit:** `oscillate_limiter_audit --balance` measures each preset's distance from its target before and after, the loudness change, the second-to-second wander of the applied correction and the onset contrast change. On the eight-track corpus the Default preset takes the mean RMS distance from its target from 4.20 to 3.05 dB (largest 9.46 to 8.12 dB), with the loudness within 0.1 LU; Gentle Polish to 3.49 dB, Streaming Balance to 3.19 dB (`docs/performance/native-effects.md`).
- **CPU:** one instance costs 1.2–1.3% of a core (pinned, whole process), with a 99th-percentile 64-frame block of 2.6% of its period; 1.8% with a reference. Sixteen in a chain cost about 20%, 99th percentile 42% at 64 frames; about 30% with a reference on each.
