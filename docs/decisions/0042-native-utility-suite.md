# ADR-0042: A native utility suite

- **Status:** Accepted; all six built. Superseded in part by [ADR-0125](0125-master-chain-cost.md): Lens analyses on the thread that reads its telemetry, not in the callback.
- **Date:** 2026-10-19.
- **Builds on:** ADR-0040 (native effect platform) and ADR-0041 (Pocket).

## Context

Requirement: a suite of analysis and utility devices (spectrum analysis and similar), each best-in-class rather than minimal.

The project already has worker-side analysis:
- `StereoSpectrumAnalyzer`: a single 2048-point Hann window;
- offline BS.1770 loudness;
- a real-time-capable `StereoTruePeakMeter`;
- `StereoMeter`.

None of them is something a user can put on a track, look at, and act on.

## Decision

The utilities are native effects on the ADR-0040 platform. Each one:
- passes audio through bit-exact (unless it's a utility that changes audio, like Trim);
- analyses in the callback without allocating;
- publishes everything its panel needs through telemetry;
- reuses existing meters wherever they're real-time safe, or is tested against them where it needs more than they give (Loudness's windowed true peak is checked against the project's `StereoTruePeakMeter`);
- gets a designed panel, as the effects and synths do.

| Utility | What it is | Quality bar (tested) |
|---|---|---|
| **Lens** (spectrum analyser) | Multi-resolution analysis: 16384, 4096 and 1024-point windows blended on a log grid, so lows resolve to a few hertz while highs follow transients. Fractional-octave band power (so a sine reads its level and pink noise is flat untilted), power averaging (fast, medium, slow, infinite), peak hold, tilt (0, 1.5, 3 dB/octave), L/R/M/S views, a sidechain overlay with a difference curve for reference matching, the strongest resonances with note names, and spectrogram history in the panel. | A sine reads its true level (±0.1 dB) at its frequency across 20 Hz–20 kHz. Pink noise is flat untilted. 30 and 35 Hz are resolved apart. Highs respond within one short hop. Peak hold holds, then decays at its rate. The audio is untouched. |
| **Loudness** | EBU R128 / ITU-R BS.1770-4 in real time: momentary, short-term and gated integrated LUFS, LRA, true peak (4×), PSR and PLR. A history graph and target presets (streaming, broadcast, club). | The EBU Tech 3341/3342 test signals read within their tolerances. True peak matches the existing meter. |
| **Scope** | A tempo-synced oscilloscope (beats across the screen) with a sidechain overlay. Estimates the cross-correlation delay and polarity between the track and the sidechain: the kick and bass alignment helper. | Delay estimates to the sample on synthetic pairs; polarity detection; the display is locked to the beat. |
| **Tune** (tuner) | YIN/MPM pitch detection: note, cents and a strobe. A kick mode tracks the fundamental's glide and decay pitch, for tuning kicks to the key (Pocket's phase 3). | ±0.5 cent on steady tones from 30 Hz to 4 kHz; a gliding 808 tracked. |
| **Trim** | Gain, pan with selectable law, width, polarity, mono, swap, L/R/M/S solo, DC block, and phase rotation for headroom. | Bit-exact at unity; pan laws exact; phase rotation flat in magnitude. |
| **Signal** | A test generator: sine, sweep, pink and white noise, impulse; added to or replacing the input. | Levels exact; pink noise flat in octave bands (−3 dB/octave in density) within ±0.5 dB. |

Order: Lens, Loudness, Scope, Tune, Trim, Signal.

## As built: Lens

**Analysis.**
- Three 4-term Blackman–Harris windows per resolution:
  - Fast: 4096, 1024 and 256;
  - Balanced: 16384, 4096 and 1024;
  - Precise: 32768, 8192 and 2048.
- The hop is 10.7 ms, on the absolute frame grid. Windows longer than eight hops update at 87.5% overlap.
- Each of the 400 display points (20 Hz–20 kHz) uses the shortest window no more than twice as coarse as its band, crossfaded with the next.
- Where the band covers the window's main lobe (five bins), the point reads band power (the summed bins over the noise bandwidth). Narrower bands read the window's own spectrum at the point, through a log-power parabola. The two readings crossfade.
- Resonances are picked from the averaged curve, then refined on the spectrum itself.

**Measured.**
- A sine reads within 0.1 dB from 25 Hz to 18 kHz, at −6 and −40 dBFS and at 44.1, 48 and 96 kHz.
- Pink noise is flat within 0.36 dB from 80 Hz to 19 kHz (Balanced) and within 1.16 dB from 40 Hz (Precise), where an estimate from about a hundred measurements is itself that noisy.
- Below where a window resolves the band, the band widens to the window's lobe: sines stay true and noise reads higher.
- Tilt is exact.
- 40 and 52 Hz are resolved with a 10.3 dB dip (Balanced); 30 and 36 Hz with 9.1 dB (Precise).
- A 5 kHz tone reaches −3 dB in 33 ms; a 50 Hz tone in 236 ms.
- The hold falls 11.7 dB in a second at 12 dB/s.
- Views, the overlay, resonances (frequency within 1/48 octave, level within 0.3 dB), infinite averaging and freeze all read true.
- Telemetry is identical for any block split, and the audio is untouched.

## As built: Loudness

**K-weighting.** Designed per rate from the BS.1770 analog prototype: the shelf at 1681.97 Hz, +4 dB, Q 0.7072, and the RLB high-pass at 38.135 Hz, Q 0.5003. Mean squares are taken over 100 ms sub-blocks.

**Readings.**
- **Momentary and short-term:** over 4 and 30 sub-blocks.
- **Integrated:** 400 ms blocks every 100 ms, with the absolute gate at −70 LUFS and the relative gate 10 LU down. Gating is exact over every block for the first two hours (storage reserved up front), then falls back to exact energy sums in 0.01 LU histogram bins.
- **Range:** short-term values every 100 ms, gated at −70 LUFS and 20 LU down; the 10th-to-95th-percentile spread.
- **True peak:** 4× oversampled by a 33-tap Blackman-windowed sinc, overall and over the last three seconds, with PSR and PLR. On analytic sines from 20 Hz to 20 kHz at 44.1–96 kHz it reads within 0.006 dB; the Annex 2 example filter it replaced over-read by up to 0.17 dB.

**One core.** K-weighting, the 100 ms steps, gating and true peak are the project's single BS.1770 implementation (`src/native_dsp/dsp/bs1770.hpp`), shared with the export loudness analyser and the master true-peak meter. On the same signal all three report identical integrated, momentary and short-term loudness and sample peak, and true peak to float precision (`native_meter_parity_contract`).

**Lifetime.** Switching Integrate either way clears the statistics. A seek restarts the windows and filters but keeps the integrated measurement.

**Measured.**
- EBU Tech 3341 cases 1–5: within 0.1 LU at 44.1, 48 and 96 kHz.
- EBU Tech 3342 cases 1–4: within 1 LU.
- True peak: the same reading as `StereoTruePeakMeter` (it is the same core), and 0 dBTP found between −3 dB samples.
- The audio is untouched.

## As built: Scope

**Sweep.**
- 480 min/max columns across the window.
- The window is a 1/16 note to 4 bars, re-locked to the beat or bar grid at every control tick, or a free time from 1 ms to 4 s.
- The channel is Mid, Side, Left or Right, with the sidechain overlaid.

**Alignment.**
- The track's mid and the sidechain go into a 170 ms window, low-passed alike below 200 Hz (fourth order) in the Low band so no relative delay is added.
- Every half window the windows are packed into one FFT, multiplied as a·conj(b) and transformed back.
- The cross-correlation and the energies are averaged over about 0.7 s before the peak within ±20 ms is read. A single window that catches only a kick's tail misreads by up to 0.5 ms; averaged, the reading holds at 2.00 ms.
- A parabola refines the delay, the peak's sign gives the polarity, and its height over the energies the correlation.

**Measured.**
- Integer delays (0, ±7 and 240 frames) within 0.05 frames.
- Half a sample reads 0.4999.
- Inversion is found.
- A 909 kick 2 ms late under gated hats reads 2.00 ms in the low band.
- The downbeat sits at column 0 at 100 and 128 BPM, for any block split.
- A free-time impulse lands in its column, and the columns carry the waveform's peaks.
- The audio is untouched.

## As built: Tune

**Instrument and Bass.**
- McLeod's NSDF on 4096 or 8192-sample windows (by FFT), about 23 times a second. The first key maximum within 90% of the highest is the period, refined by a parabola.
- The strongest of the first four harmonics (picked from the same FFT) is measured on two Hann windows a hop apart. Its phase advance pins the frequency.
- Clarity is gated at 0.8, level at −60 dBFS.

**Kick.**
- Onsets on the fourth-order low band below 300 Hz.
- The frequency is measured cycle by cycle at interpolated zero crossings, stamped at each cycle's middle less the filter's group delay, and interpolated onto a 5 ms grid for 400 ms.
- The settled pitch is the median of the last five cycles.

**Measured.**
- Steady sines within 0.1 cent from 30 Hz to 4 kHz at 44.1, 48 and 96 kHz.
- Tones with a fundamental 12 dB under the second harmonic read their own note within a cent.
- A4 = 432 reads true.
- A synthetic 150→50 Hz glide (30 ms) is traced within 6%, settling on 50 Hz.
- Pulse's 909 kick settles at 50.07 Hz and, tuned +12, at 99.77 Hz: 1193 cents. Pulse's body is exact; 909 House layers a second 55 Hz body, and the pair beats.

## As built: Trim

**Stages.** Gain; pan as balance or with the −3, −4.5 or −6 dB law (centre unity in every law); width; mono; swap; polarity; solo left, right, mid or side; a 5 Hz DC block; and phase rotation (two RBJ allpasses at 150 Hz, Q 0.7).

**Exact at rest.** Every stage is bypassed exactly at rest, and ramps snap onto their targets, so Unity is bit-exact, even after a move and back. Toggles glide over 20 ms.

**Measured.**
- Every law, width, mono, swap, polarity and solo is exact to 1e-6.
- The rotation is flat within 0.01 dB from 30 Hz to 18 kHz, and lowers a phase-aligned harmonic stack's crest from 12.0 to 7.9 dB at unchanged level.

## As built: Signal

**Waves.**
- **Sine:** phase-continuous through frequency changes.
- **Log sweep:** Farina's exponential law from 20 Hz to 20 kHz over the sweep time, repeating from zero phase. Telemetry carries its instantaneous frequency and progress.
- **White noise:** uniform, seeded, scaled to its RMS.
- **Pink noise:** white through first-order corners every half octave from 2.5 Hz, alternating pole and zero, by matched z. That law bends near Nyquist, so at creation the top six corners are refitted against 1/f out to Nyquist (Levenberg-Marquardt, about 15 ms on the control thread). Its RMS is set from the filter's exact impulse-response energy.
- **Impulses:** single samples at the level, at the set rate.

**Level** is peak for tones and impulses and RMS for noise; ramps land exactly on their targets. **Routing:** Replace or Add, onto both channels, left, right, alternating each second, or with the right inverted. **Off** is bit-exact; switching fades over 10 ms. A seek restarts the generator.

**Platform.** Signal is the first engine that sounds with no input, so `EffectEngine` gained `generator`. The silence, tail and seek-silences contracts exempt generators; Signal's own contract covers Off and its fade.

**Measured.**
- The sine matches its analytic phase within 1e-6 at 44.1, 48 and 96 kHz.
- The sweep's frequency follows the law within 0.5% up to 5 kHz (limited by the zero-crossing measurement).
- White noise reads its RMS within 0.05 dB and is flat within 0.25 dB.
- Pink noise reads its RMS within 0.1 dB. Its shaping, measured by cross-spectrum against white noise from the same seed, falls 3 dB an octave within 0.1 dB in every octave from 31.5 Hz to 16 kHz. Its own octave bands read level within 0.3 dB.
- Impulses, the five outputs and Add are exact.
