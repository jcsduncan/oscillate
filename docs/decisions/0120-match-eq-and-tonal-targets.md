# ADR-0120 — Match EQ and tonal balance targets in Lens

**Status:** Accepted and implemented. **Date:** 10 November 2026.
**Builds on:** ADR-0042 (Lens), ADR-0047 (Shape), ADR-0054 (live controls), ADR-0103 (shared device panel parts).
**Contract:** `include/oscillate/native_dsp/tonal_targets.hpp` (the grid, target curves and the factory targets, shared with the adaptive tonal balance device), `include/oscillate/native_dsp/tonal_analysis.hpp` (the band meter, building targets, the target file), `include/oscillate/native_dsp/match_eq.hpp` (the correction and the band fit), Lens's telemetry and match parameters (`effects/lens.hpp`), `shape::band_values`, `NativeSynthPanelAdapter::write_next_effect`. No schema change.

## Context

Lens compares a mix with a reference in its sidechain, but only shows the difference. Matching a mix to a reference, or checking it against what finished records in a genre look like, then means reading a curve and setting EQ bands by hand.

## Decision

1. **The grid.** Tonal curves use third-octave bands on base-2 centres, 1 kHz · 2^(n/3) for n = −16…12: 29 bands from 24.8 Hz to 16 kHz, tiling 22.1 Hz to 18.0 kHz. Each band splits into four twelfth-octave fine bands, so the fine bands sum to a band exactly.
2. **Levels are loudness-normalised.** A curve's level is the long-term average power in each band, in dB, shifted so its spectral loudness is 0 dB. The spectral loudness is 10 log10 Σ 10^((L + K)/10), with K the BS.1770 K-weighting at each band's centre, so gain never moves a curve. A correction between two normalised curves keeps the mix's loudness.
3. **One band meter, in Lens and offline.** `tonal::BandMeter` measures two signals' fine-band powers with Welch's method: a periodic Hann window of 16384 frames at 48 kHz (the nearest power of two at other rates, so about 0.34 s), half overlapped, the two signals in one complex transform. Lens runs it on the mid of its input and of its sidechain, whatever the view, frozen or not. Its telemetry gains each fine band's mean power for both signals and the window count (`telemetry::tonal_*`). The target tool runs the same class on decoded files, so a target and a mix in Lens are measured alike.
4. **Match EQ.** `LensMatch` (`apps/daw_quick/src/lens_match.*`) keeps its own long-term average of the meter from each telemetry frame. The correction is the reference's normalised fine curve minus the mix's. The reference is either the sidechain or a target's median, interpolated linearly in log frequency between band centres. Lens's match parameters shape it:
   - **Smoothing:** a Hann window in octaves whose half-power width is 1/6, 1/3, 1/2 or 1 octave.
   - **Amount:** 0–100%.
   - **Max Boost and Max Cut:** 0–12 dB.
   - **Low and High Limit:** the correction fades to nothing over half an octave beyond them.

   These parameters are saved with Lens and not automated.
5. **Shape bands, not a match filter.** `match_eq::fit` draws the correction with the fewest Shape bands. It adds one band at a time where the error is largest, trying a peak there and a shelf at either end (at most one of each). After each addition it refines every band together by Levenberg–Marquardt over log frequency, log Q (a shelf's slope) and gain, against Shape's own response at 48 kHz (`prepare_eq` and `eq_magnitude_db`). It stops once the RMS error over the limits is 0.25 dB or less, or at Shape's eight bands; bands left under 0.1 dB are dropped. Peaks keep Q 0.3–8 and shelves a slope of 0.3–1.

   A linear- or minimum-phase FIR match filter was not built. It would add latency (linear phase) or a new engine, and it would leave nothing to edit. Writing bands keeps the result in an EQ the user can adjust, it is free of latency, and it glides on live edits like any Shape edit. The fit error is shown, so a curve the bands cannot follow is visible.
6. **Apply** writes the bands into the Shape right after Lens (every band past the fit off), or into a new Shape inserted there when the next device is not one, as one undo step through `EffectService::apply_effect_edits`. A Shape after Lens measures nothing Lens sees, so applying again after more listening replaces the bands rather than stacking corrections. Values reach a running Shape as a live edit, and its band transitions glide.
7. **Tonal balance targets.** A target has, per band, the median of its references' normalised curves and the range of their middle half (the 25th and 75th percentiles, linear between order statistics). Lens shows the mix's normalised curve against the chosen target, drawn flat about the median, and marks each band inside or outside the range.
8. **Factory targets** were built by `tools/tonal_targets` from 26 commercial club tracks the maintainer bought, grouped by their genre tags:
   - All References (26 tracks);
   - Electronica (10);
   - Melodic House & Techno (9).

   Genres with fewer than eight tracks were left out. Only these statistics ship (29 numbers per curve, rounded to 0.01 dB), in `src/native_dsp/tonal_targets_factory.cpp`, generated by the tool. They are averages over whole tracks at a third of an octave: no audio, and nothing from which audio could be recovered. The tracks are not distributed or named.
9. **User targets.** `tools/tonal_targets/build_targets.py` builds a target from any folder of references. It decodes them with ffmpeg and measures them with `oscillate_tonal_targets`, writing the target to `$XDG_DATA_HOME/oscillate/tonal-targets/`, or one target per genre tag. In Lens, Save as target stores what the sidechain played (the input when the sidechain was silent) as a target. Its range is the middle half of that reference's own curve over 3 s chunks; it needs four chunks. Lens lists every valid file in that folder after the factory targets.
10. **Target files** are JSON: `format` `"oscillate-tonal-target"`, `version` 1, `name`, `references`, the grid's `centres_hz`, and `median_db`, `low_db` and `high_db` (29 each, to 0.01 dB). A file is refused when:
    - it is over 64 KiB;
    - its format or version is another;
    - its name is empty or over 64 bytes;
    - it has no references;
    - its centres are off the grid;
    - any level is not finite or outside −120 to 40 dB;
    - any range misses its median.

    The same references in any order build the same file.
11. **Threads.** The band meter allocates only when built. On the audio callback it buffers each frame, and every half window it runs one 16384-point transform and the band sums. The correction and the fit run on the UI thread. They run when a match setting or the target changes, and at most once a second while the average grows. A fit takes about 30 ms for five bands.

## Consequences

- **Measured recovery** (`native_match_eq_contract`): pink noise against its own copy through four known Shape curves, measured by the band meter, corrected at 1/6-octave smoothing and fitted. Each recovered response is compared with Shape's own over 40 Hz–12 kHz, less the loudness offset:

  | Curve | Bands | Recovered: RMS, peak | Fit error: RMS, peak |
  |---|---|---|---|
  | Shelves | 2 | 0.05, 0.13 dB | 0.07, 0.37 dB |
  | Two peaks | 2 | 0.19, 0.35 dB | 0.24, 0.42 dB |
  | Tilt and a bump | 3 | 0.23, 0.75 dB | 0.21, 0.70 dB |
  | A narrow cut and air | 2 | 0.11, 0.48 dB | 0.11, 0.16 dB |

  The stated tolerance is 1 dB peak and 0.35 dB RMS.
- **Tests:**
  - `native_match_eq_contract`: recovery; the fewest bands; the reported response and error are what Shape plays; the correction follows its settings; target interpolation.
  - `native_tonal_targets_contract`: the grid; K-weighting against the loudness analyser; gain-free curves; the meter on sines and noise, allocating nothing; deterministic rebuilds and the file round trip; malformed files; the factory targets; placement.
  - `native_lens_contract` (`tonal_meter_reads_input_and_sidechain`): Lens's telemetry is the band meter's.
  - `quick_lens_panel_contract`: the panel proposes bands and Apply writes them; in and out of range on a user target; saving a target; broken files ignored.
  - `quick_native_synth_session_adapter_contract`: a new Shape after Lens, then the same one, each one undo step.
  - `application_routed_effect_live`: a six-band write goes live with no allocation and no gap or jump.
- **CPU:** the tonal meter adds about 0.25% of a core to Lens, as one 0.34 ms transform every 0.17 s (i7-10700K, GCC 15.2).
- **Panel:** a Match EQ card (the correction dashed, the fitted Shape response with numbered bands, the match settings, Restart and Apply to Shape) and a Tonal Balance card (target picker, range corridor, in-range count, Save as target). The panel is 1058 px tall.
