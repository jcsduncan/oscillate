# ADR-0052 — Constant-tempo and key analysis

**Status:** Accepted; implemented. The downbeat rule arrived with the clip strip (ADR-0053). **Date:** 21 October 2026.

## Context

Mixing DJ sets depends on every imported track getting a right, straight beat grid. The previous tempo analysis:
- built a single loudness envelope;
- histogrammed the gaps between neighbouring onsets;
- anchored the grid to the first onset.

On a commercially released dance track with a four-on-the-floor kick from the first bar (125 BPM), it reported 139.85 BPM at confidence 0.18. Key analysis returned "unknown" on the same track.

Released dance music, and nearly every produced track, holds one tempo. Long reverb tails, fade-outs and beatless intros must not bend that tempo. That was the failure that forced manual warp-marker work in other DAWs.

## Decision

### Tempo: one straight grid, fitted only where the music pulses
1. **Onset curves.** Two curves of log spectral flux: the kick band (below 150 Hz) and the whole spectrum. A third curve measures linear power rise in the kick band. Each has its local mean removed. The source is preceded by a window of silence, so a transient at sample 0 is seen.
2. **Phase-free score.** For each candidate tempo, the score sums the magnitude of each curve's Fourier component at the beat rate and at its 2× and 4× harmonics. Bands are never summed before the magnitude: offbeat hats sit half a beat from the kick, and mixing them would cancel the beat. Scoring each band separately is what fixed the 125 BPM track.
3. **Most likely tempo.** Taken from a padded FFT, with a broad prior around dance tempi (centred on 122 BPM, σ = 0.6 octave) to settle octave choices.
4. **Pulse weighting.** Local clarity is measured in 8 s windows. Tails, fades, breakdowns and beatless intros get zero weight.
5. **Weighted fit.** Refined by golden-section search on the exact DFT, to 0.0005 BPM. The tempo snaps to a whole BPM when the straight grid fits as well there.
6. **Grid phase.**
   - The whole-spectrum component places the beat.
   - The kick's power decides which half of the beat is the kick.
   - The grid then moves by the median offset to the steepest level rise around each beat, measured on a ⅓ ms envelope. This places the grid on the attacks themselves, whatever their shape.
7. **Confidence.** Mean beat clarity over the pulsing stretches, times the share of beats where an onset curve actually peaks. A tempo needs at least four landed beats.

**Results:**
- 26 of 26 commercially released dance tracks with tagged tempos (115–125 BPM, many with long breakdowns and tails) come out exactly at their whole-number tempo.
- Grids on click fixtures are sample-exact.
- A synthetic groove with louder offbeat hats, a beatless intro and an 8 s tail returns the exact tempo, with the grid within 5 ms of every kick.

Analysis takes about 5 s per 7-minute track in an optimised build, on the analysis worker. The cache payload moves to tempo v3, so earlier estimates are rebuilt.

### Key: tuned chroma against KeyFinder profiles
1. **Chroma.**
   - Spectral peaks between 55 Hz and 2 kHz, in 16384-point windows.
   - The source's tuning is the energy-weighted circular mean of each bin's distance from A440.
   - Each peak is weighted by √magnitude and a cos² closeness to a tuned semitone.
   - Windows are normalised, so every moment counts once.
2. **Scoring.** Pearson correlation against Sha'ath's KeyFinder major and minor profiles, which outperformed Krumhansl, Temperley and the EDMA profiles on the test set.
3. **Unknown.**
   - Fewer than five pitch classes in play: a lone chord is not a key.
   - Best correlation below 0.5.
   - A margin over the runner-up below 0.01.
4. **Confidence.** The margin divided by 0.25.

On the 26 tracks, 19 match Mixed In Key's tags. Five of the seven disagreements are near neighbours: a fifth away, or the relative or parallel key. Those tags are another detector's opinion, not ground truth, so the analysis is not tuned to them. Disputed keys are settled by ear.

## Consequences
- Tempo is a straight grid by design. Variable-tempo material (live recordings) will get a "Free time" material hint, alongside "Stem / loop" (whole bars from sample 0, BPM = 240·bars/seconds), on the clip's BPM editor.
- Downbeat detection, and the stem rule that prefers the project tempo, arrive with the clip strip and the downbeat flag. Manual ÷2, ×2, tap, typed BPM and warp markers remain as overrides.
