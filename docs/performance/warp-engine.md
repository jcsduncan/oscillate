# Warp engine evidence

Running record for the in-house warp engines proposed in [ADR-0026](../decisions/0026-continuous-warp-stream.md).

**How measurements are taken**
- **Machine:** Intel i7-10700K.
- **Compiler:** GCC 15.2.0, `-O3 -DNDEBUG`, x86-64 baseline ISA (SSE2), no fast-math, no FMA contraction.
- **Build:** a separate uninstrumented Release tree (`build/warp-release`).
- **Tool:** `oscillate_warp_dsp_bench`.
- **Metric:** thread CPU as a percentage of real time, per stereo stream at 48 kHz, for 30 s of noise source with 64- and 128-frame `render()` pulls. The 64- and 128-frame results agree within noise, so only 128 is shown.

## A1 — time map and Re-Pitch

**Correctness** (`warp_dsp.time_map`, `warp_dsp.repitch`)
- **Map evaluation:**
  - The quotient/remainder evaluation and its extrapolation equal an independent 128-bit limb oracle across 2,000 random segments of up to 2^31 frames.
  - The incremental stepper equals direct evaluation at every frame.
- **Accuracy:**
  - Unity speed at whole frames is bit-exact on all tiers.
  - Q0 sine error stays at or below −80 dB re amplitude from 0.25× to 7.9×.
  - The 10 kHz tone stays at or below −70 dB; Q2 stays at or below −45 dB.
  - An 8 kHz tone read at 4× (it would alias to 32 kHz) is suppressed by more than 70 dB.
- **Timing:**
  - Impulses at a marker and inside fast or slow segments peak on the exact mapped output frame.
  - Output is bit-identical for 1-, 63-, 64-, 128- and random-frame pulls and after seeks.
  - A splice holds a constant level and matches a direct read after the 64-frame crossfade.
  - `start()`/`render()` perform zero heap allocations.
- **Optimised read vs reference:** the polyphase/fixed-point read matches the reference two-wing read within 2e-6 over 60,000 random probes.

**Cost:** CPU % of real time per stereo stream, 128-frame pulls.

| Tier | Speed | Baseline (two-wing, per-frame division) | Optimised (stepper, polyphase rows, fixed-point taps) |
|---|---|---|---|
| Q0 | 0.5–1.0 | 0.51 | 0.36 |
| Q0 | 2.0 | 1.33 | 0.95 |
| Q0 | 4.0 | 2.55 | 1.91 |
| Q1 | 1.0 | 0.29 | 0.20 |
| Q2 | 1.0 | 0.17 | 0.14 |

Above unity speed, the anti-aliasing kernel widens in proportion to speed, so cost grows linearly. That is inherent to band-limited decimation.

The 8-lane polyphase loop vectorises with SSE2. Explicit lane order keeps it deterministic with no reassociation.

AVX2/FMA runtime dispatch is deferred to milestone A8, which will measure it across all modes together.

## A2 — FFT and transient detection

**FFT wrapper**
- `RealFft` wraps the reviewed, pinned and patched PFFFT v1.1.0 target (`docs/dependencies/pffft-convolution.md`). warp_dsp creates that target only if convolution has not already done so.
- Forward transforms match a naive double-precision DFT within 2e-6·n for sizes 32 to 4096, including the 3- and 5-factor sizes.
- The inverse round-trips within 1e-5.

**Detector** (`analysis::detect_transients`)
- **Onset function:**
  - SuperFlux (Böck & Widmer 2013): Hann STFT of about 10.7 ms with a 1.33 ms hop. The size doubles at 88.2/96 kHz, so time resolution is the same at every rate.
  - Spectra pass through a 24-band-per-octave triangular log filterbank from 40 Hz to 16 kHz.
  - Flux is rectified against a 3-band max-filtered spectrum four hops earlier.
- **Peak picking:**
  - Uses SuperFlux windows: ±30 ms maximum, −100/+70 ms mean, 30 ms minimum gap.
  - The offset is max(2.5, 0.08 × strongest onset) × the user scale.
  - The absolute floor is needed because a vibrato pad's flux tops out at 1.3, while sharp onsets measure 50–140. Without the floor, a relative-only threshold produced 39 false transients on the pad.
- **Attack refinement:**
  - Uses an AIC change-point picker on a 16th-order linear-prediction residual. The residual model is fitted to 512 samples before the search span.
  - A plain-signal AIC placed legato plucks with a 3.4 ms mean error; the residual version brought that to 0.01 ms.
- **Source end:** analysis stops before any window leaves the source, because the cut to padding registered as an onset.

**Results** (`warp_dsp.transients`, 10 ms match tolerance, −80 dB noise floor)

| Fixture | Rates | Precision | Recall | Mean / worst placement error |
|---|---|---|---|---|
| Clicks | 44.1–96 kHz | 1.000 | 1.000 | 0 / 0 samples |
| Drum hits (kick/snare, 1 ms attack) | 44.1–96 kHz | 0.976–1.000 | 1.000 | 0.12–0.14 / ≤0.45 ms |
| Legato plucks over ringing notes | 44.1–96 kHz | 1.000 | 1.000 | 0.007–0.013 / ≤0.045 ms |
| Vibrato pad with fade-in | 48 kHz | no onsets expected | — | 0 found |

- The one extra drum detection, at 96 kHz, is a weak (strength 0.106) fluctuation in a noise tail.
- Analysis runs at 708× real time on 30 s of dense noise (Release, 48 kHz).
- ASan/UBSan pass on all warp_dsp suites.

**Still open:** a recorded-material evaluation and listening review. That comes with the A0 recorded-loop fixtures.

## A3 — Beats

**Design**
- **Slices:**
  - Preserve points (transients or grid positions, as source frames) split the source into slices.
  - A slice starts sounding at the first output frame where the map reaches its start; the exact inverse search on the integer map finds that frame.
  - The slice then plays unstretched, resampled only by transpose.
  - Every slice event is derived from the output frame alone, so seek and partition invariance hold by construction.
- **Gaps** (the slice runs out before its successor):
  - **Off** fades to silence over 2 ms.
  - **Forward** loops the second half of the slice with a 2 ms crossfade into the audio leading up to the loop start.
  - **Back-and-forth** ping-pongs over the same span.
- **Envelope:** below 100, each slice decays linearly to silence over that percentage of its output duration.
- **Cut-short slices:**
  - A slice that is cut short keeps sounding for a 2 ms overlap fade under its successor, and the successor's attack is untouched.
  - The first design faded out before the boundary instead. That left a dip ahead of every attack, which the independent detector consistently placed 95 samples early at 1.25×.
  - The same overlap gives loop splices a crossfade.
- **Pitch:** transpose is fixed per stream, so a `FixedRateReader` precomputes a contiguous polyphase bank for that cutoff once. Whole-frame unity-band reads short-circuit to the exact sample (proven bit-identical); the same shortcut now also serves Re-Pitch.

**Correctness** (`warp_dsp.beats`, 48 kHz)
- **Placement:** at 0.5, 0.8, 1.25, 2 and 3×, every slice after its predecessor's 2 ms overlap is a bit-exact copy of the source from its exact mapped attack frame up to the next attack.
- **Independent detector:** at 0.5–1.25× (sensitivity 0.5), all 22/22 attacks are found within 0.5 ms. At 2×, 22/22 are within 5 ms and 14 within 0.5 ms; low kicks under a loud cut snare tail are perceived up to 3.4 ms after their first sample.
  - At 3× (83 ms spacing), masked kicks fall below the detector threshold. Placement there rests on the exact-copy oracle.
  - The detector's handling of dense, masked material is a known limit to revisit with recorded loops.
- **Unity:** a unity map is bit-exact for the whole clip.
- **Loop modes:** Off leaves exact silence in gaps. Forward and back-and-forth fill gaps at the material's level (RMS ≈ 0.11). No mode produces a sample step larger than the largest step in the source.
- **Envelope:** a 40% envelope silences each slice exactly after 40% of its duration.
- **Transpose:** +1200 cents measures exactly 880.00 Hz from 440 Hz.
- **Invariance:** output is bit-identical across 1, 64, 8,192 and random pulls and after seeks across slow, fast and splice segments, for every loop mode with envelope and transpose.
- **Dense slices:** slices every 111 or 250 ms at 0.37, 0.5 and 1.6×, with −1900 to +2400 cents, render completely. A regression case covers a forward-loop wrap inside the overlap fade, which originally exhausted a source window.
- **Safety:** zero allocation in `start()`/`render()`; invalid settings are rejected; ASan/UBSan are clean.

**Cost** (Release, % of a core per stereo stream, 128-frame pulls, slices every 250 ms)

| Map speed | Transpose | Off | Forward | Back-and-forth |
|---|---|---|---|---|
| 0.5× | 0 | 0.054 | 0.073 | 0.089 |
| 1.25× | 0 | 0.055 | 0.073 | 0.060 |
| 2.0× | 0 | 0.057 | 0.063 | 0.062 |
| 0.5× | +7 st | 0.22 | 0.61 | 0.61 |
| 1.25–2× | +7 st | 0.50–0.58 | 0.58–0.59 | 0.58 |

- Before the fixed-rate bank and the whole-frame shortcut, unity Beats cost 0.41% and +7 st cost 0.79%.
- Upward transposition costs more because a 1.5× read needs 96 anti-aliased taps at Q0; the Q1 tier halves that.
- With these changes, unity Re-Pitch measured 0.35%.

**Detector follow-up:** a low attack following loud noise-like material placed late by about 3 ms. Attack refinement now applies the linear-prediction residual only when the context's prediction gain is at least 6 dB, and uses the raw signal otherwise. Base transient results are unchanged.

## A4 — Complex (phase vocoder)

**Design**
- **Frames:**
  - Hann frames of N = 4096 (8192 at 88.2/96 kHz), with a synthesis hop of N/8 (N/4 at Q2).
  - Frames sit on a fixed output grid and are read at the map's source position.
  - Overlap-add is normalised by the window-product sum that actually contributed to each sample, so masks never change level.
- **Instantaneous frequency:** taken from a fixed-lag pair (the frame and one hop earlier), so it is valid at any analysis hop from freeze up to 64×.
- **Phase locking:**
  - Identity phase locking (Laroche & Dolson 1999) on the channel sum.
  - Each peak's rotation is applied to its whole region of influence in both channels, which preserves the stereo image.
  - Trigonometry runs per peak only.
- **Anchors:**
  - Clip start, clip end, onsets and loop splices are anchors.
  - Frames within half a window of an anchor are aligned at unity speed and masked at its source and output position.
  - The phase resets at the first frame of the start zone, the first frame at or after an onset or splice, and on entry to the end zone. Without the end-zone reset, accumulated rotations dispersed a final-frame impulse to 0.006 of its level.
  - Splices crossfade over ±64 frames.
- **Between anchors (transient-region time shifting):**
  - Frames follow the map with a correction that runs linearly from the source excess consumed at unity in one zone to the deficit before the next, clamped outside both zones.
  - Analysis windows therefore meet anchors only at their zero-weight edges.
  - Before this, frames whose window held an anchor mid-window produced a sample step 2.4× the source's at a mask edge, and repeated a click at −21 dB at 0.37×.

**Correctness** (`warp_dsp.complex`, 48 kHz)

| Check | Result |
|---|---|
| Unity reconstruction (legato plucks + noise) | 79.9 dB SNR |
| 440 Hz tone at 0.25, 0.5, 0.8, 1.25, 2, 3× | pitch within 0.01 cent; level 0.00 dB; 100 ms RMS ripple ≤ 0.01 dB |
| Anchored clicks at 0.37–3× | 0-frame placement error; pre-echo ≤ −156 dB; post-echo (3–60 ms) ≤ −107.6 dB |
| Impulses on the first and last source frame at 0.5–3× | land intact (0.900 of 0.900) on the first and last output frame |
| Ratio change at a marker plus a loop splice (220 Hz) | worst sample step 0.0144, equal to the source's own |
| Partition | bit-identical for 1, 64, 8192 and random pulls |
| Seek 5000 frames after an onset | bit-identical |
| Seek 9 s into a pad with no epochs | 100 ms RMS within 0.5 dB of continuous render |
| Allocation, sanitisers | zero allocation in `start()`/`render()`; ASan/UBSan clean |

**Cost** (Release, % of a core per stereo stream, onsets every 250 ms; 64- and 128-frame pulls equal within noise)

| Tier | 0.5× | 1× | 2× |
|---|---|---|---|
| Q0 | 1.22 | 1.18 | 1.15 |
| Q1 | 1.22 | 1.18 | 1.15 |
| Q2 | 0.61 | 0.60 | 0.58 |

Q0 is within the 1.5% target.

**Open:**
- Q1 is currently Q0 for Complex. Its separate role comes with Complex Pro.
- Mask and anchor evaluation per frame are measured candidates for A8.
- A recorded-material listening review and a black-box reference comparison are still to come.

## A5 — Transpose, formants and Complex Pro

**Transpose**
- **How it works:**
  - Complex and Complex Pro run the vocoder on an intermediate timeline u = t × p, where p = 2^(cents/1200).
  - Anchors map to u = T × p.
  - The output reads the intermediate stream at u through a fixed-rate band-limited reader at speed p.
  - The intermediate buffer slides with the output, and transient-region shifting and epochs work unchanged in the intermediate domain.
- **Unity:** with no transpose, both conversions are exact identities and the reader is bypassed, so every A4 result stands bit-for-bit.
- **Pitch accuracy:** a 440 Hz tone at −2400, −1200, −700, +700, +1200 and +2400 cents, each at 0.5, 1 and 2×, lands within 0.01 cent of the target with RMS ripple ≤ 0.03 dB.
- **Clicks:** transposed clicks (−1200 and +700 cents at 0.6×) land on their mapped frames with 0 offset.
- **Determinism:** output is bit-identical across render partitions and after a seek near an epoch.

**Formants (Complex Pro)**
- **Gains:** the intermediate spectrum is scaled by (Env(f·p)/Env(f))^(formants/100), so after resampling the source envelope sits back in place.
- **Envelope estimator:** a true envelope (Röbel & Rodet 2005): 8 iterations of cepstral smoothing on max(log spectrum, previous envelope).
  - The log spectrum is floored 60 dB below its peak.
  - Estimation runs on a 4× max-pooled spectrum with quarter-size transforms.
- **Envelope parameter:** maps to a cepstral cutoff of envelope / 2 samples at 44.1 kHz. The default of 128 resolves envelopes of voices up to about 340 Hz.
- **What each step fixed:**
  - Plain cepstral smoothing averaged harmonic peaks with the valleys between them and left the first formant unpreserved.
  - Without the floor, a −46 dB hole below the fundamental pushed the lowest output harmonic 15 dB high and the worst centroid shift reached 12%.
  - Max-pooling both cut the cost and improved accuracy.
- **Oracle:** amplitude-weighted harmonic centroid of a synthetic vowel (150 Hz; resonances at 700 and 1200 Hz) between 200 and 3000 Hz, compared with the source.

| Transpose | Centroid shift, formants 100 | Formants 0 (vs pure pitch scaling) |
|---|---|---|
| −700 cents | −1.36% | 0.00% (−33.3%) |
| −300 cents | −0.83% | 0.00% (−15.9%) |
| +300 cents | +1.72% | 0.00% (+18.9%) |
| +700 cents | +0.99% | 0.00% (+49.8%) |

**Cost** (Release, Q0, 0.8× map, onsets every 250 ms, % of a core per stereo stream)

| Mode | −7 st | +7 st |
|---|---|---|
| Complex | 1.20 | 2.34 |
| Complex Pro | 1.45 | 2.98 |

- Before decimating the envelope estimation, Complex Pro cost 1.80 and 3.73.
- Upward transposition costs more because the vocoder runs p times more frames on the intermediate timeline.
- All four cases fall within the 5% Complex Pro budget.

**Open, by measurement:**
- Phase-gradient heap integration and harmonic/percussive separation for Complex Pro will be adopted only if a polyphonic ground-truth stretch metric and a recorded-material listening review show a gain over identity locking, which already measures near-ideal on tones.

## A6 — Pitch analysis, Tones and Texture

**Pitch tracking (YIN, de Cheveigné & Kawahara 2002)**
- **Method:**
  - The difference function is built from one FFT cross-correlation per frame plus prefix energy sums.
  - It uses the cumulative-mean-normalised difference with a 0.15 threshold and parabolic refinement.
  - Window and hop scale with the longest lag.
- **Accuracy:** harmonic tones from 65 to 880 Hz at 44.1, 48 and 96 kHz are voiced in every frame, with worst error 0.015%.
- **Rejection:** white noise is 0/375 frames voiced, and silence is never voiced.

**Pitch marks**
- Marks walk each voiced run one period at a time.
- Each next mark is the shift within ±¼ period that best correlates with the previous period, so marks stay on the same point of the cycle.
- The first version took the largest peak instead and jumped 16 frames on a 330 Hz tone. Spacing error is now ≤ 0.55 frames at 98, 150 and 330 Hz.

**Grain engine** (shared by Tones and Texture)
- Overlap-add of tabulated raised-cosine grains.
- Sources are masked per run; splices crossfade over ±64 frames.
- **Coherence-aware normalisation:** output divides by √(Σw² + ρ̄·((Σw)² − Σw²)), where ρ is each grain's correlation with its predecessor. This is amplitude normalisation for seamless reads and power normalisation for decorrelated grains. With plain window-sum normalisation, a 0.25× texture lost 4.7 dB.

**Tones (TD-PSOLA)**
- **Grains:**
  - Output marks are laid out arithmetically from each run's start, at period/p.
  - Each mark takes a two-period grain (or a P/p grain when transposing down) from the nearest source mark.
  - Unvoiced material uses grains of `grain_size`.
- **Formants:** transposition keeps them, as PSOLA does.
- **Determinism:** seeks are bit-exact, because the mark sequence is pure arithmetic from the run start.
- **Results** (150 Hz harmonic tone, autocorrelation within a fifth of the target):

| Case | Result |
|---|---|
| 0.5–2× stretch | pitch within 0.06 cent, level 0.00 dB |
| −700 to +700 cents | within 0.01 cent |

**Texture (granular + WSOLA)**
- **Grains:** a fixed grid at 4× overlap. Fluctuation jitters each grain's source position by up to half a grain, keyed on index and seed.
- **Alignment:**
  - WSOLA (Verhelst & Roelands 1993) shifts each grain by up to ±10 ms, to the best normalised correlation with its predecessor's continuation.
  - The correlation is computed by FFT.
- **Why alignment:**
  - Without it, stretching a 150 Hz tone produced a spectral line shifted by the grain rate (80 Hz instead of 75 Hz an octave down).
  - A ±2.5 ms search still left −17 cents, because it is shorter than half a period below 200 Hz.
- **Determinism:** the alignment chain restarts per run. A seek replays at most 2 s, so it is exact within that span and converges beyond it.
- **Results:**

| Case | Result |
|---|---|
| Pad level at 0.25, 0.5, 2× | within 0.24 dB |
| Tone at 0.5, 0.8, 2× | spectral line at 150.00 Hz |
| −1200 cents | 75.00 Hz |
| +700 cents | −1.27 cents (0.25 Hz probe step) |
| Partition, seek, flux, seed | partition- and seek-invariant within the pre-roll; flux and seed change the output |

**Cost** (Release, Q0, 0.8× map, 128-frame pulls, % of a core per stereo stream)

| Mode | 0 st | +7 st |
|---|---|---|
| Tones | 0.10 | 0.13 |
| Texture | 0.22 | 0.86 |

Texture first cost 3.25% at 0 st and 4.48% at +7 st. Three changes brought it down:
- FFT correlation in place of direct correlation for the WSOLA search;
- whole-frame grain positions at unity pitch, which make every read an exact copy;
- the Q2 kernel for transposed grains (decorrelated texture does not need a 100 dB stopband).

A tabulated window, with its division hoisted to once per grain, also brought Tones from 0.14% to 0.10%.

## P1 — Source-rate conversion for processed clips

`warp_dsp::RateConvertedSourceView` (ADR-0026 point 6) presents a managed source at another rate to every warp mode at the project rate.

**Design.**
- An exact-rational polyphase bank has one row per residue of `n·S mod P/gcd(S,P)`: 147–320 rows for the 44.1/48/88.2/96 kHz pairs. Output frame `n` reads source position `n·S/P` with no rounding and no state.
- The filter is a Kaiser-windowed sinc (β 10) with 96 zero crossings at the lower rate and a cutoff at 0.967 of the lower Nyquist. Rows are normalised to unity DC.
- Equal rates bypass the filter bit-exactly.
- `processing::make_warp_stream_map` scales plan anchors to `round(s·P·65536/S)` in integer arithmetic. `processing::ProjectRateSourceView` wraps the verified decoder.

**Gates** (`tests/warp_dsp/rate_view_tests.cpp`, all 16 source × project pairs of 44.1/48/88.2/96 kHz):
- **Passband:** analytic sines at 2.3%, 45% and 90% of the lower Nyquist reproduce pitch, phase and level within 1.6e-5 of the 0.5-amplitude tone (−90 dB), comparing each output frame with the sine at exactly `n/P`.
- **Stopband:** when converting down, tones at 1.02×, 1.3× and 1.9× the project Nyquist leave less than 1.6e-5 (−90 dB) of alias.
- **Oracle:** the float bank matches a direct double-precision evaluation within 2e-6, including both ends.
- **Seek and partition:** 40 random seeks and lengths per pair, straddling both ends and the 2,048-frame block edge, are bit-identical to one continuous read. Frames outside `[0, frames())` are silent.
- **Trim edges:** frames at interior trim points convert from the neighbouring source on both sides.
- **Rejection:** unsupported rates and phase periods over 4,096 are rejected before any source is opened. A wrapped read failure returns false with silence.
- **Allocation:** reads do not allocate.
- **Stream:** a 44.1 kHz 440 Hz tone through a 48 kHz Re-Pitch identity map keeps 48,000 frames and its pitch and phase within 1e-4.
- **Map:** the adapter tests cover every pair. An unwarped trimmed clip, planned by `prepare_clip_frame_plan` with the import duration rule, reads at unity speed in project frames. A musical marker keeps its output time and source frame.
- **Verified view:** a staged 44.1 kHz managed WAV opens as a verified 48 kHz view that matches the oracle, and passes through bit-exactly at 44.1 kHz. Truncation after verification is reported as a decoder error with silence.
- **Sanitizers:** the converter, map and verified-view tests pass under ASan/UBSan with warnings as errors.

**CPU** (Release `-O3`, GCC 15.2, i7-10700K, thread CPU time as a percentage of real time per stereo clip, 30 s per row; worker-side only):

| Pair (kHz) | Taps | 64-frame pulls | 128-frame pulls | 2,048-frame pulls |
|---|---|---|---|---|
| 44.1 → 48 | 192 | 0.84% | 0.83% | 0.83% |
| 48 → 44.1 | 216 | 0.86% | 0.85% | 0.84% |
| 44.1 → 96 | 192 | 1.73% | 1.69% | 1.70% |
| 96 → 44.1 | 424 | 1.60% | 1.59% | 1.58% |
| 88.2 → 48 | 360 | 1.50% | 1.48% | 1.46% |
| 48 → 96 | 192 | 1.64% | 1.63% | 1.62% |

**Limits.**
- Externally linked sources are still refused for processing.
- The render cache reads different-rate sources through `RateConvertedSourceView`; `rate_view_algorithm` and its version are part of the key.
- Unprocessed clips keep the optional libsamplerate reader.

## P2 — Processed-clip render cache (LDW2)

`processing::render_warp_to_cache` renders any of the six saved modes for one clip. It runs from `warp_render_request()`, which resolves the clip through `prepare_clip_frame_plan`. The path reads source through `ProjectRateSourceView` and maps it with `make_warp_stream_map`.

**Analysis per render.** Analysis runs on the whole project-rate source view, so its results depend on the source alone.
- Beats, Complex and Complex Pro detect transients: 698× real time (Release, 48 kHz, 30 s of dense noise).
- Tones tracks pitch and places marks: 162× real time on a voiced tone.
- A ten-minute source therefore adds under one second (transients) or four seconds (pitch) of worker time per render.
- Stream costs are the per-mode figures above.
- No callback work changed.

**LDW2 payload.** A 96-byte header is followed by interleaved f32 stereo:
- magic and format version 2;
- frame count;
- mode, tier and channel count (2);
- the 64-hex-digit key digest;
- zeroed reserved fields.

The reader checks every header field, then that every sample is finite, before it allows a seek read. Any mismatch is a miss, including a version 1 header, LDW1 magic or another key's digest.

**Gates** (`tests/processing/warp_render_cache_tests.cpp`; also under ASan/UBSan and TSan):
- **Cents:** `clip_transpose_cents` matches an exact extended-precision oracle for every thousandth of a semitone in ±48, and for both neighbouring doubles of each. A specified table covers stored doubles just above and below half a cent.
- **Intent rules:** Re-Pitch transpose is inactive in both key and audio (bit-identical). Complex Pro with formant intent off has the same key and audio as formants 0, and transposed pitch stays within 5 cents either way.
- **Key sensitivity:** the key changes with source identity, project rate, clip, generation, tier, transpose, mode, each mode's own parameters, a moved marker and a trimmed source start. It ignores inactive modes' parameters.
- **Duration and pitch:** every mode keeps the exact planned duration. Stretching a 440 Hz tone 1.25× keeps pitch within 5 cents in Beats, Tones, Texture, Complex and Complex Pro; Re-Pitch drops by exactly 1.25×. +7.004 semitones lands within 5 cents of +700 cents in Complex, Complex Pro and Tones.
- **Different rate:** a 44.1 kHz source in a 48 kHz project keeps its elapsed duration and pitch (within 2 cents) in Complex and Re-Pitch, and unity Re-Pitch matches the analytic tone within 1e-3.
- **Corruption:** wrong mode, tier, channel count, digest, version or reserved bytes; a NaN sample; truncation; and LDW1 entries all miss. The executor work renders over an LDW1 entry.
- **Failures:** superseded tickets, another key's ticket, cancellation and a damaged source identity all fail and leave the previous valid entry. An over-speed Re-Pitch map fails as `unsupported`.
- **Queue:** resubmitting unchanged inputs keeps the job. A new settings generation cancels the old claim and installs only the latest result. A reopened queue reuses a valid entry without rewriting it (same inode) and rebuilds a corrupt one.

**Found and fixed on the way:** `place_pitch_marks` gave no marks to a run voiced from the source's first frame. Tones then fell back to plain grains and drifted −86 cents on a pure tone. This is fixed, with a regression test.

**Limits.**
- Transient and pitch analysis is recomputed for each render. A shared source-analysis cache (PA-07) would remove that cost.
- Grid-based Beats preserve points are not implemented; Beats slices at transients.
- Texture uses the fixed seed `texture_render_seed`.
- Playback (P3) and export (P4) use these renders.

## P3 — Live processed-clip playback

`application::RoutedAudioSource` plays saved processed clips through `processing::WarpLiveReader` on its prefetch worker (ADR-0026 point 8).

**Callback cost.** The callback path is unchanged except for two relaxed atomic stores per clip consume (the consumer position) and one comparison for planned loop wraps. The routed managed-audio callback benchmark (Release, i7-10700K, GCC 15.2, one clip through the routed graph):

| Block | Unfaded | Faded | Callback allocations |
|---|---|---|---|
| 64 frames | 11.9 µs | 13.7 µs | 0 |
| 128 frames | 19.1 µs | 22.5 µs | 0 |

No routed callback baseline had been recorded before this change.

**Worker cost.**
- A cache-backed clip reads 8 bytes per frame.
- A live clip costs its mode's stream figures above (for example Complex Pro Q0 under 5% of a core per stereo clip at 48 kHz).
- Preparation runs the mode's analysis (P2) once per prepared source.
- The reader allocates all three tiers' streams at preparation, so a step-down is a restart of a prepared stream at the next frame, not an allocation.

**Gates** (`tests/application/routed_warp_playback_tests.cpp`, ASan/UBSan and TSan clean):
- **Every mode, live:** Re-Pitch, Beats, Tones, Texture, Complex and Complex Pro, at 64- and 128-frame callbacks with zero callback allocations. Output is bit-identical to an independent continuous offline Q0 render over the whole clip, with silence before and after it.
- **Markers:** an impulse marked to output frame 20,000 of a clip placed at 500 sounds exactly at project frame 20,500 in Re-Pitch and Beats.
- **Seek and loop wrap:** a seek resumes bit-identically in Re-Pitch, Beats, indexed Tones and cache-backed Complex. A planned loop wrap plays its first block at once and stays bit-identical across the loop body, with no underrun or step-down. A loop armed too late reports missed.
- **Cache:** a valid cache render is used. A corrupted or cleared cache falls back to live rendering with identical output.
- **Different rate:** a 48 kHz source in a project changed to 44.1 kHz keeps its pitch (within 2 cents) and elapsed duration.
- **Source failure:** truncation after preparation fails the worker with a reported error, counted missing frames and silence.
- **Falling behind:** a starved worker underruns, which is reported per clip. On recovery it steps to Q1 (`reduced_quality_clips` 1, one step-down) and plays on. A starved unprocessed clip recovers the same way without a quality step. Without the ring forward skip, both stay silent (mutation check).
- **Saved edits:** a mode edit while playing is captured as an audio clip edit. The old source still reports the old mode and the control reports the edit pending until handoff. The candidate source renders the new mode and primes the exact handoff block. The saved edit reopens and plays bit-identically to its offline render.
- An unprocessed clip still takes the raw source path.

**Limits.**
- Bit-identity after a seek holds for Re-Pitch, Beats, Tones and any cache-backed clip. Live Complex, Complex Pro and Texture converge after their bounded pre-roll, as the engine contract states.
- Quality never steps back up within one prepared source.
- Leaving an armed loop mid-pass without a seek is not supported.
- Clip ring capacity is still the existing per-source buffer budget divided by clip count.
- These are synthetic correctness gates, not recorded listening approval.

## P4 — Offline export of processed clips

Export renders each processed clip with `processing::OfflineWarpClip`, opened through `exporting::prepare_processed_export_clip`.
- **Source and analysis:** the verified managed descriptor is bound with `ProjectRateSourceView::from_descriptor`, and the mode's analysis runs once per clip.
- **Rendering:** one Q0 stream is read in order in up to 8,192-frame chunks, so export cost per clip is the mode's Q0 stream figure plus its analysis (P2).
- **Cache:** none is consulted. A cache entry can make live playback cheaper, but never makes export succeed.
- **Callback:** no callback code changed.

**Gates:**
- `tests/export/warp_export_tests.cpp` (also clean under ASan/UBSan):
  - **Every mode:** Re-Pitch, Beats, Tones, Texture, Complex and Complex Pro, stretched 1.25× with +2 semitones and a marker. Master and processed-stem PCM are bit-identical to an independent direct Q0 stream, opened through the project path and pulled in alternating 64- and 128-frame blocks. Silence before and after the clip; frame-exact length; repeat renders byte-identical.
  - **Markers:** a marked impulse lands at exactly project frame 20,480 in Re-Pitch and Beats at 48 kHz. After the project changes to 44.1 kHz it lands at exactly 18,816, with the rescaled duration and bit-identity to the oracle.
  - **Pitch and formants:** +7.004 semitones exports within 5 cents of +700. Re-Pitch transpose is inactive (byte-identical). Complex Pro with formants off equals formants 0 and differs from formants 100.
  - **Routing and raw clips:** a processed stem beside a raw clip still matches the oracle; the raw clip's stem is its source; the master is the sum. Through a group bus with a reverb insert, the tail continues after the processed clip, byte-identical on repeat.
  - **Source failures:** changed source bytes and a missing source fail without replacing an existing target.
- `tests/processing/verified_source_view_tests.cpp`: a descriptor-bound view keeps reading the verified bytes after the managed path is moved and replaced. Truncating the pinned file is a reported decoder error, and mismatched metadata or an external link is refused.
- `tests/application/vst3/vst3_mixed_export_tests.cpp`: a processed audio stem in the mixed VST3 instrument variant follows the routed direct stream (dry plus 0.5 send) within 1e-6. Stretched clips, including converted-rate and one-source-frame extremes, render in the mixed and VST3 effect variants.
- **Mutation check:** forcing export to Q1 fails the master oracle at frame 0.
