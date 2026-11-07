# Warp recorded-material review

**Status:** three private full-mix excerpts and two reproducible composites of the repository's generated one-shots have objective checks. **Every listening judgment below is pending**; isolated natural drum, vocal and sustained-instrument categories are also pending.

Synthetic oracles (`docs/performance/warp-engine.md`) show the engines behave as designed. They are not evidence of Ableton parity, and neither is any machine-generated score in this document.

## Listening pack and level control — 18 October 2026

`tools/listening/warp/listening_pack.py` prepares a small, peak-safe set for a human
reviewer after the audition harness and independent oracle pass. It checks the
manifest's source digest and every selected output's frame count. It writes
new 48 kHz stereo float WAV files and `index.tsv` to an **empty ignored build
directory**, leaving the originals untouched. The default selection is
Re-Pitch identity, Beats 0.5×, Tones +7, Texture's marker map, Complex 0.5×,
and Complex Pro −7. Any manifest `mode_case` can be chosen with `--cases`.

```sh
python3 tools/listening/warp/listening_pack.py \
  build/release/audition/drums.wav \
  build/release/audition/drums-default \
  build/release/audition/listening-drums
# Play source.wav and the named cases from listening-drums at the same device
# volume. Record listener, date, material, mode/case and observations below.
```

The pack first caps the source's sample peak at 0.9, then matches each render's
whole-file RMS to that reference where a 0.9 sample-peak ceiling permits it.
`index.tsv` reports the applied gain, resulting peak and remaining RMS
difference. A remaining difference means the ceiling prevented a full level
match; account for it when comparing. Whole-file RMS is a simple volume aid,
not perceptual loudness matching, and a time-altered file may have different
silence density. This pack cannot establish transient, formant, stereo or
overall sound quality. Listen to the unaltered source and original renders
too if the gain treatment masks a concern.

On the deterministic generated drum fixture listed below, all six prepared
cases plus the source stayed below 0.9, had the expected frame counts, and
matched the reference RMS within the displayed 0.001 dB. Their measured
peaks ranged from 0.500 to 0.661. This is only a check of the review tooling;
**no listening judgment was made**. The focused Python tests cover the gain
ceiling, manifest digest rejection, float output, frame count and reusing a
nonempty destination.

## User-library excerpt screen — 19 October 2026

Two more 15 s excerpts from a private, read-only MP3 collection were decoded
with FFmpeg (`-ss 60 -t 15 -ar 48000 -ac 2 -c:a pcm_s16le`) into an ignored
build tree. Both are commercial full mixes, one labelled as a vocal mix and
one as an organ track; their labels do not establish that the chosen passages
contain exposed vocal or sustained-organ material. They
therefore extend the full-mix screen, not the isolated vocal or instrument
qualification categories. Neither MP3s, decoded excerpts nor renders are
committed.

Environment: Intel i7-10700K, GCC 15.2.0, Ubuntu kernel 7.0.0-38-generic,
Release `-O3`, 48 kHz stereo PCM16 input, Q0, 128-frame worker pulls. The
unmodified `oscillate_warp_audition` rendered its 34 fixed cases for each
excerpt. `recorded_oracle.py` independently decoded every output with FFmpeg
and found zero frame, finite-sample, manifest or identity mismatches. Every
status was `ok`; Re-Pitch identity decoded sample-identically to its source.

| Full-mix excerpt | Decoded input SHA-256 | Detected transients / pitch marks | Maximum float peak | Cases with samples at/above unity | Samples at/above unity, all cases | Largest render CPU / output time |
|---|---|---:|---:|---:|---:|---:|
| Vocal-titled | `04ccf3701f991c79f39141fdccac89de2862acf48b5d132dd210fdaeb7c0931d` | 13 / 6 | 1.684 | 22/34 | 11,598 | 2.794% |
| Organ-titled | `5cc9489472585e94433a1d248f58b16f5a7e5542a4f7f8d4b59874510bc6871c` | 121 / 520 | 1.391 | 29/34 | 4,594 | 2.884% |

The 34 organ-titled render WAVs were regenerated and compared byte for byte:
zero differed. CPU ratios are single-stream worker measurements, with the
largest case being Complex Pro +7 semitones in both runs. They are not a live
callback or whole-project capacity result. Peak overshoots are retained in
float files and indicate headroom risk; they do not by themselves establish
audible clipping or algorithm quality.

The local listening handoff is under
`build/quick/scratch/warp-recorded-review/` in the canonical checkout. These
ignored files are retained locally for review and are not part of the project.
Each of `vocal_labelled_mix-listening/` and `organ_labelled_mix-listening/`
contains `source.wav`, six named comparison WAVs and an `index.tsv` with
gain and RMS difference. The full raw sets and manifests are in sibling
`*-renders/` directories. The listening-copy peak is approximately 0.9 or lower. Peak-safe
gain could not RMS-match every result: the largest remaining difference was
−5.181 dB for vocal-titled Complex Pro −7, and −3.547 dB for organ-titled
Complex Pro −7. Compare at a fixed device volume, then account for the
`index.tsv` differences and consult the raw float render if a level change
obscures an artefact. **No human listening or subjective rating has occurred.**

Next listening checks: Beats at half speed for attack smear, Complex at half
speed for warble, Texture at the marker seam for discontinuity, and Complex
Pro −7 for formants and headroom. The two title-based excerpts must not close
the vocal, sustained-instrument or natural dry-drum gates.

## Reproducible in-repo material screen — 18 October 2026

`tools/listening/warp/review_fixture.py` arranges the four original procedural
starter sounds into an eight-second dry drum pattern and a drum-plus-pluck
composite. Both are **generated arrangements of generated one-shots**, not
natural recordings, vocal material, or human listening evidence. The script
uses the checked-in PCM16 files and Python's standard library; it writes only
to the ignored worktree build tree. Starter-sound provenance is in
`docs/assets/generated-starter-sounds.md`; its release licence decision remains
open.

Environment: Ubuntu 26.04 (kernel 7.0.0-38-generic), Intel Core i7-10700K,
x86-64, GCC 15.2.0, CMake Release `-O3`,
48 kHz stereo, Q0, 128-frame pulls. This is a per-stream worker render, not a
live callback or whole-project performance test. The local dependency sysroot
used for configure is recorded in the worktree's
`build/release/CMakeCache.txt`; no system package was installed for this run.

Configure/build commands from the worktree root; the local dependency prefix
is machine-specific (`$DEPS` below):

```sh
mkdir -p build/release/scratch
PKG_CONFIG_PATH=$DEPS/unpacked/usr/lib/x86_64-linux-gnu/pkgconfig \
PKG_CONFIG_SYSROOT_DIR=$DEPS/unpacked \
TMPDIR="$PWD/build/release/scratch" cmake --preset release \
  -Dnlohmann_json_DIR=$DEPS/nlohmann/usr/share/cmake/nlohmann_json \
  -DOSCILLATE_BUILD_BENCHES=ON
TMPDIR="$PWD/build/release/scratch" cmake --build build/release \
  --target oscillate_warp_audition -j8
```

Audition commands (repeat for `mix`, replacing `drums`):

```sh
python3 tools/listening/warp/review_fixture.py --kind drums \
  --output build/release/audition/drums.wav
TMPDIR="$PWD/build/release/scratch" build/release/tools/listening/warp/oscillate_warp_audition \
  --input build/release/audition/drums.wav \
  --output-dir build/release/audition/drums-default
python3 tools/listening/warp/recorded_oracle.py \
  build/release/audition/drums.wav build/release/audition/drums-default
TMPDIR="$PWD/build/release/scratch" build/release/tools/listening/warp/oscillate_warp_audition \
  --input build/release/audition/drums.wav \
  --output-dir build/release/audition/drums-extreme --extreme
python3 tools/listening/warp/recorded_oracle.py \
  build/release/audition/drums.wav build/release/audition/drums-extreme
```

| Generated input | Input SHA-256 | Default / extreme | Independent structural failures | Maximum float peak | Samples at/above unity | Maximum render thread CPU / output time |
|---|---|---:|---:|---:|---:|---:|
| Drums | `d3ee25b9e83fa7a21496579cd5f15aba72d9b18c7cabdf248e501b6cbecd947d` | 34/34 / 56/56 `ok` | 0 / 0 | 0.603 / 0.751 | 0 / 0 | 2.922% / 4.327% |
| Drums plus pluck | `17850a0d79861bcdf74a364d08daed257b44b3212d0c4174c651f709c4c38856` | 34/34 / 56/56 `ok` | 0 / 0 | 0.657 / 0.657 | 0 / 0 | 2.881% / 3.910% |

All 180 output lengths match their maps, every output decodes to finite samples,
and Re-Pitch identity decodes sample-identically to its input. A second default
drum run wrote 34/34 byte-identical WAVs; its manifest differs because CPU
timings and command/output paths are run-specific. Regenerating the drum input
also produced byte-identical WAV bytes. The maximum CPU ratios above are the
largest single case's `cpu_seconds / duration_seconds`, not a capacity claim.
The fixed-extreme cases cover 0.25×, 4× and ±12 semitones. These results add
reproducibility and a headroom screen; they do not approve stretch or pitch
quality. The natural recorded and level-matched listening gates below remain
open.

Artifacts in the run's worktree:
`build/release/audition/{drums,mix}.wav`,
`build/release/audition/{drums,mix}-{default,extreme}/manifest.tsv`, and
their sibling float WAV renders. These paths are ignored local build output,
not committed project media.

## Independent structural oracle — 18 October 2026

`tools/listening/warp/recorded_oracle.py` is an opt-in check of a completed audition
set. It uses FFprobe for rate/channel identity and FFmpeg for sample decoding,
independently of `media::WavReader` and `warp_dsp`. It checks the source SHA-256,
all output frame counts, non-finite samples, peaks and samples at or above unity
against `manifest.tsv`. It also requires the Re-Pitch identity render to decode
sample-identically to the source. Run it after the audition harness:

```sh
python3 tools/listening/warp/recorded_oracle.py \
  build/warp-release/audition-selftest/selftest_input.wav \
  build/warp-release/audition-selftest/renders
python3 tools/listening/warp/recorded_oracle.py \
  <private-48k-stereo-input.wav> build/warp-release/audition/full-mix
```

On the private 15 s full-mix input already described below, the new isolated
GCC 15.2.0 Release build rendered **34/34 cases `ok` with exact frames**. The
independent oracle found **zero structural disagreements**, no non-finite
samples, and a sample-identical Re-Pitch identity output. It independently
reproduced the existing headroom evidence:

| Mode | Maximum float peak | Cases above unity | Samples at/above unity | RMS range relative to source |
|---|---:|---:|---:|---:|
| Re-Pitch | 1.056 | 3/4 | 8 | −0.01…+0.00 dB |
| Beats | 1.626 | 5/6 | 827 | −0.47…+0.91 dB |
| Complex | 1.337 | 5/6 | 2,295 | −0.99…0.00 dB |
| Complex Pro | 1.697 | 5/6 | 3,813 | −0.99…0.00 dB |
| Tones | 1.307 | 3/6 | 2,150 | −0.47…+0.13 dB |
| Texture | 1.033 | 4/6 | 33 | −1.19…−0.01 dB |

The same oracle passes the generated self-test set (34/34), with Re-Pitch
identity sample-identical. These measurements confirm structural consistency,
not transparent sound. An independent FFmpeg Rubber Band render of the same
source gave exactly 1,440,000 frames at 0.5×, 360,000 at 2×, and 720,000 at
+7 semitones. Its peaks were 1.283, 1.107 and 1.223 respectively. That is a
timing and headroom comparison only: different algorithms and settings cannot
be ranked by those peak values. The Rubber Band outputs and private input are
retained only in the ignored local build tree.

A second 8 s input arranged the repository's recorded kick, snare and hat
one-shots at a fixed 48 kHz beat grid. It is a deterministic composite of
recorded hits, **not a performance recording**. The harness detected 31
transients, rendered 34/34 cases `ok`, and the independent oracle found zero
structural disagreements and no non-finite or over-unity samples. Its maximum
peaks were 0.612 (Re-Pitch/Beats), 0.618 (Complex), 0.664 (Complex Pro),
0.612 (Tones) and 0.651 (Texture), against input peak 0.612. The input digest
is `a680741f2401103682654457fd86eb21a5723d86140e0cfa2980dc00d33af900`.
This exercises distinct transient content but does not replace the required
listening of a natural dry drum loop.

On this machine (Intel i7-10700K, GCC 15.2, Release `-O3`, 48 kHz, Q0), the
unchanged `oscillate_warp_dsp_bench` measured Re-Pitch at 1× at **0.343% / 0.341%**
of real time for 64/128-frame pulls, and Complex at 0.5× at **1.174% / 1.177%**.
This is a diagnostic per-stream CPU comparison, not whole-project qualification.
No DSP code changed in this review, so no before/after optimization or new
allocation claim is made.

The Beats overlap and the remaining Complex Pro overshoot require level-matched
listening against the source and independent render before changing attacks,
envelopes or limiter policy. No quality gate is closed by the checks above.

## Harness

`oscillate_warp_audition` (`tools/listening/warp/warp_audition.cpp`) is an opt-in, worker-only CLI.

- **Not built by default:** it is `EXCLUDE_FROM_ALL` and not registered with CTest.
- **Input:** parsed and decoded through `media::WavReader`; the manifest hashes
  the input file for run identity. This local harness does not pin a managed
  project source or verify against a saved source record.
  - Only 48 kHz stereo WAV is accepted. Every generated case is capped at 10
    minutes of output, so the input can be at most 5 minutes for the default
    set or 2.5 minutes with `--extreme`.
  - Any other rate or channel count is rejected with a clear message. The harness never resamples or converts channels.
- **Analysis:** runs on the whole input through the public `warp_dsp::analysis` APIs.
  - Transients become Beats slices and Complex/Complex Pro onsets.
  - YIN pitch marks become Tones grains.
- **Rendering:**
  - Every mode is rendered through the public `warp_dsp` stream APIs at the selected tier (default Q0), in 128-frame pulls.
  - Output is 32-bit float stereo WAV at 48 kHz in the chosen directory.
  - Choose that directory inside a build tree. Never commit inputs or renders.

**Cases:** 4 for Re-Pitch, 6 for every other mode (34 renders per input).

| Case | Map | Transpose | Modes |
|---|---|---|---|
| `identity` | unity | 0 | all |
| `half` | 0.5× speed (twice as long) | 0 | all |
| `double` | 2× speed (half as long) | 0 | all |
| `markers` | markers at ⅓ and ⅔ of the source: 1×, then 0.6×, then 1.5× | 0 | all |
| `up7` | unity | +700 cents | Beats, Complex, Complex Pro, Tones, Texture |
| `down7` | unity | −700 cents | Beats, Complex, Complex Pro, Tones, Texture |

Re-Pitch has no separate transpose; its pitch follows speed.

`--extreme` adds four more cases (6 for Re-Pitch and 10 for every other mode, 56 renders per input):

| Case | Map | Transpose |
|---|---|---|
| `quarter` | 0.25× speed | 0 |
| `quadruple` | 4× speed | 0 |
| `up12` | unity | +1200 cents |
| `down12` | unity | −1200 cents |

The manifest header records whether `--extreme` was set.

**Fixed mode settings:**
- **Beats:** Loop Forward, envelope 100.
- **Complex Pro:** formants 100, envelope 128.
- **Tones:** grain 40 ms.
- **Texture:** grain 100 ms, flux 0, seed 1.

**`manifest.tsv`** sits beside the renders.
- **Header:** the exact command, input path, input SHA-256, input frame count, sample rate, tier, transient count, pitch-mark count and pull size.
- **One row per render:** file, mode, case, description, transpose, expected frames, output frames, duration, peak, clipped-sample count (|x| ≥ 1.0), render thread CPU seconds and stream status.
  - A status other than `ok` (`unsupported_map`, `source_failed`, …) or `create_failed: …` is recorded, and the run continues.
- **Determinism:** audio is deterministic for a given input and command. CPU time is not.

**Build and self-test** (Release tree, per `AGENTS.md`):

```sh
mkdir -p build/warp-release/scratch
cmake -DOSCILLATE_BUILD_BENCHES=ON build/warp-release
TMPDIR="$PWD/build/warp-release/scratch" cmake --build build/warp-release \
  --target oscillate_warp_audition
build/warp-release/tools/listening/warp/oscillate_warp_audition \
  --self-test build/warp-release/audition-selftest
```

`--self-test` writes a generated 2 s, 48 kHz, 16-bit stereo fixture: drum hits every 250 ms over a sustained 165 Hz harmonic tone. It then checks:
- all 34 renders report `ok`;
- output frame counts equal each case's exact map length;
- every WAV header reads back as float, 2 channels, 48 kHz, 32-bit, with the right frame count and file size;
- no render is silent;
- the manifest has a 64-hex-digit input digest and one row per render;
- a second run writes byte-identical WAVs;
- an `--extreme` run renders all 56 cases `ok` with exact lengths;
- mono, 44.1 kHz and non-WAV inputs fail with clear messages and write no manifest.

## Run protocol

1. **Inputs:** use licence-clean 48 kHz stereo WAVs (convert outside the harness if needed, and note how). Do not add them to the repository. Record each file's SHA-256 from the manifest.
2. **One render set per category:**
   ```sh
   build/warp-release/tools/listening/warp/oscillate_warp_audition \
     --input <file>.wav --output-dir build/warp-release/audition/<category>
   ```
   - **Drums:** a dry kit loop with clear kick/snare/hat transients, 4–16 bars.
   - **Monophonic vocal:** a sung or spoken phrase with sibilants and sustained vowels.
   - **Sustained instrument:** a pad, strings, organ or held piano chord with a long decay.
   - **Full mix:** a dense stereo mix with drums, bass and vocal.
   - **Extreme ratio:** re-render the drum and sustained-instrument files with `--extreme` into `build/warp-release/audition/<category>-extreme`.
3. **Listening setup:** level-matched headphones, reproducible (note the device). Listen to each mode against the `identity` render and the source.
4. **Rate each render** 1–5 (5 = no audible artefact) on:
   - **transient clarity** (smear, flam, pre-echo);
   - **warble or phasiness** on sustained content;
   - **stereo image** stability;
   - **seams** at the `markers` case rate changes, at ⅓ and ⅔ of the source;
   - **vocal formants** (`up7`/`down7`, vocal only).
5. **Record** each rating and a one-line note per render in the tables below, with the listener's initials, date, device and manifest digest.

## Objective results

### Self-test fixture (generated, not recorded material)

| Input | SHA-256 | Frames | Transients | Pitch marks |
|---|---|---|---|---|
| `selftest_input.wav` (2 s synthetic) | `cbd45de7a6e6b7f0cc70830ae07071ed01a7520a0e266119223128463e010faf` | 96,000 | 7 | 253 |

- **Status, frames, clipping:** all 34 renders `ok`, exact frame counts, no clipped samples.
- **Peaks:** 0.47–0.72 against a source peak of 0.517.
- **CPU:** per render ≤ 0.02 s of thread CPU (Release, i7-10700K, GCC 15.2).

Observations from the manifest:
- **Texture `identity`** peaks at 0.612 against the source's 0.517, so Texture is not transparent at unity speed on this drum-plus-tone fixture.
  - This is consistent with grain-level coherence normalisation and WSOLA shifts acting on the noisy hits.
  - It is recorded for listening, not as a defect.
- **Tones `up7`/`down7`** keep exactly the source peak (0.516571).
  - That fits unvoiced drum grains being copied verbatim at their source positions.
  - Listening should confirm the tone itself is transposed.
  - The synthetic Tones tests already show transpose within 0.01 cent.
- **Complex/Complex Pro** peaks track the source to within about 1e-5 at every non-transposed ratio.

### Recorded material

A 15-second excerpt of a private stereo MP3 was decoded locally to 48 kHz, 16-bit stereo WAV with FFmpeg (`-ss 60 -t 15 -ar 48000 -ac 2`). Neither the source nor the renders are in the repository. The converted input SHA-256 is `17a3c2e34797dfa1123bf4bcd7a9655e71ee7e591ab40add346b9de4ef3bdbb3`; its peak is 0.998. This is one full-mix example, not a category-wide quality decision.

The first run exposed an assertion in Complex when adjacent source onsets' analysis windows overlapped under a half-speed map. Commit `50aa3b7` adds a synthetic regression and follows the uncorrected map when no source interval remains between those onset zones. The rerun completed all 34 cases with `ok` status and exact expected frame counts. The synthetic regression passes for Complex and Complex Pro at different pull sizes.

| Mode | Cases | Maximum float peak | Samples at or above unity across cases |
|---|---:|---:|---:|
| Re-Pitch | 4 | 1.056 | 8 |
| Beats | 6 | 1.626 | 827 |
| Complex | 6 | 1.337 | 2,295 |
| Complex Pro | 6 | 2.198 (1.697 after the fix below) | 15,409 (3,813 after) |
| Tones | 6 | 1.307 | 2,150 |
| Texture | 6 | 1.033 | 33 |

These are float WAV peaks, so values over unity are retained in the audition files. They indicate gain/headroom risk, especially for Complex Pro with downward transpose; they do not establish audible clipping or quality on their own. The output still needs level-matched listening, and integer export needs its normal level and limiter review.

#### Complex Pro headroom (investigated and fixed)

Measured on the same excerpt (same digest), by comparing Complex Pro with Complex at the same transposition:
- Complex Pro `down7` was +1.44 dB louder overall than Complex, and +1.77 dB median (up to +4.2 dB) in 20 ms windows. Its band balance was close to the source's, so the envelope itself was right.
- Formant shaping alone changed each frame's energy by a median +1.77 dB at −700 cents and −1.60 dB at +700 cents. That is 10·log₁₀(1/p): with the envelope restored, a transposed partial-rich frame has 1/p more partials under each formant. Coarsening or refining the envelope detail (half or double) left the peak at about 2.1, and bounding each bin's gain to ±12 dB left it at 1.86, so neither envelope resolution nor a few runaway bins was the cause.
- That level change rode on the crest-factor growth every phase vocoder shows on a limited master (Complex `down7` already peaks at 1.337), giving 2.198.

The fix (Complex Pro algorithm version 2) scales each shaped frame back to its unshaped energy. One positive scalar per frame cannot move the envelope: the synthetic vowel's formant centroid stays within the same −1.4%…+1.7% as before. Formant preservation is therefore level-neutral against the same transposition without formants. Cached Complex Pro renders from version 1 miss and are rebuilt. A synthetic regression (`complex_pro_formants_keep_the_level`, on a band-limited and a bass-heavy mix at ±7 and ±12 semitones) requires the level with formants to stay within 1 dB of the level without, and the peak below 1.25 times it; the old code failed both. CPU is unchanged within run-to-run noise (0.204 → 0.208 s thread CPU for the 15 s `down7` render).

| Complex Pro on the excerpt | Peak | Samples at or above unity | RMS vs Complex |
|---|---:|---:|---:|
| `down7` before | 2.198 | 13,867 | +1.44 dB |
| `down7` after | 1.697 | 1,988 | −0.23 dB |
| `up7` before | 1.016 | 5 | −1.17 dB |
| `up7` after | 1.261 | 288 | +0.21 dB |
| all 6 cases after | 1.697 max | 3,813 total | — |

Going up, Complex Pro is no longer quieter than Complex, so its peak rises to about Complex's own (1.207). The remaining overshoot over the source peak in every mode is the transposition and stretch itself on a limited master, not a gain error. Beats (1.63, at `double`/`up7`/`half`) comes from its deliberate 2 ms tail overlap under each new slice's full-gain attack. Tones (1.31, at the stretches) comes from overlapping pitch-synchronous grains. Changing either trades transient shape for headroom, so it waits for the listening review. Exported integer formats still rely on the user's level and the master limiter.

| Category | Input file | SHA-256 | Frames | Transients | Pitch marks | Non-OK renders | Clipped renders | Output dir |
|---|---|---|---|---|---|---|---|---|
| Drums | starter one-shot composite; natural loop pending | `a680741f2401103682654457fd86eb21a5723d86140e0cfa2980dc00d33af900` | 384,000 | 31 | 48 | 0/34 | 0/34 | ignored local build tree |
| Monophonic vocal | pending | | | | | | | |
| Sustained instrument | pending | | | | | | | |
| Full mix | private 15 s excerpt | `17a3c2e34797dfa1123bf4bcd7a9655e71ee7e591ab40add346b9de4ef3bdbb3` | 720,000 | 121 | 46 | 0/34 | 25/34 over unity | ignored local build tree |
| Full mix | vocal-titled 15 s excerpt | `04ccf3701f991c79f39141fdccac89de2862acf48b5d132dd210fdaeb7c0931d` | 720,000 | 13 | 6 | 0/34 | 22/34 over unity | `build/quick/scratch/warp-recorded-review/vocal_labelled_mix-renders` |
| Full mix | organ-titled 15 s excerpt | `5cc9489472585e94433a1d248f58b16f5a7e5542a4f7f8d4b59874510bc6871c` | 720,000 | 121 | 520 | 0/34 | 29/34 over unity | `build/quick/scratch/warp-recorded-review/organ_labelled_mix-renders` |
| Extreme ratio | pending | | | | | | | |

## Listening judgments

**All pending.** No listener has rated any render. Do not infer ratings from the objective tables.

| Category | Mode | Case | Transients | Warble | Stereo | Seams | Formants | Listener / date | Notes |
|---|---|---|---|---|---|---|---|---|---|
| Drums | all | all | pending | pending | pending | pending | n/a | | |
| Monophonic vocal | all | all | pending | pending | pending | pending | pending | | |
| Sustained instrument | all | all | pending | pending | pending | pending | n/a | | |
| Full mix | all | all | pending | pending | pending | pending | pending | | |
| Extreme ratio | all | all | pending | pending | pending | pending | n/a | | |

## Limits

- **Fixed ratios:** the case set is deliberately fixed so that runs stay comparable. It covers 0.25–4× and ±12 semitones with `--extreme`. Other ratios, and ±24 semitones, need a code change rather than a free-form flag.
- **Inputs:** only 48 kHz stereo WAV. Other formats must be converted outside the harness, and the conversion recorded, because the harness must not resample or remix silently.
- **Fixed settings:** analysis and mode settings are fixed (see Harness). Loop mode, envelope, grain size, flux and formant settings are not swept.
- **Engine paths:** the harness renders from memory through the public stream APIs. It does not exercise the verified source adapter, prefetch rings, live playback or export paths; those have their own gates.
- **Tier:** the default tier is Q0. Q1/Q2 step-down audition needs separate runs with `--tier`.
- **Engine findings:** the private full-mix run exposed the Complex onset-zone assertion described above, now covered by a synthetic regression. If another recorded run produces a non-OK status or an obvious artefact, add a reproducible case here (input digest, command, manifest row) and fix it in a separate engine change.
