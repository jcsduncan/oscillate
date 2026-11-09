# ADR-0110 — Heavy projects across the helpers

**Status:** Accepted; implemented. **Date:** 9 November 2026.
**Builds on:** ADR-0040 (native effect platform, Forge), ADR-0048 (audio worker pool), ADR-0061 (performance tracing), ADR-0085 (nested voice rendering).
**Supersedes:** ADR-0048's default worker count.

## Context

The reference projects (`docs/performance/reference-projects.md`) showed the heavy project overrunning every block, by 273% of the period at 128 frames and 328% at 64, with whole-process CPU pinned at one core. Its trace gave four causes:

- **The player had no helpers.** The reference player and the 32-track rehearsal took their render source without a worker pool, so every source and every graph level ran on the callback alone. Live playback passes its pool, and the engine already renders sources and graph levels in parallel.
- **SMT threads sat idle.** The default helper count was one fewer than the physical cores, assumed to be half the hardware threads. A four-thread runner (two cores, two threads each) got one helper.
- **Forge cost 225 µs a block** per instance at 128 frames, 58% of a core across the project's 15. At 16× it evaluated two log-cosh antiderivatives per oversampled step per channel through the library's `exp` and `log1p`, chose its curve on every step, and resampled each channel on its own.
- **Shape's display spectra lined up.** Every Shape ran two 4096-point transforms every 1024 frames for its panel, and EQs created together analysed in the same block: the project's 17 Shapes spiked one callback in eight.

## Decision

1. **The qualification players render with the live helpers.** The reference player and the 32-track rehearsal give their render source a pool of `AudioWorkerPool::default_worker_count()` helpers, as live playback does. The fake callback starts them as ordinary threads, and a sink starts them at the callback's priority. `--workers N` sets another count, and results report the count.
2. **A helper for every hardware thread but the callback's, up to seven.** `default_workers_for(cpus)` is one fewer than the CPUs in the process's affinity mask, SMT siblings included, capped at `default_worker_cap` (7). The cap is the count ADR-0048 and ADR-0085 measured on the reference machine (16 threads, 7 helpers before and after this change); ADR-0048's table stopped improving at 3 to 7. `OSCILLATE_AUDIO_WORKERS` still allows up to 15.
3. **Forge does the same arithmetic with less work.**
   - The antiderivative at the previous input is kept from the step that computed it, so each step evaluates one antiderivative instead of two.
   - Both channels go through the half-band resamplers together, as `StereoSample` pairs.
   - The curve is chosen once per block: the sample loop is compiled once per curve.
   - A held input reuses the curve's value at the last midpoint, so silence at the bias point costs no transcendental.
   - The log-cosh antiderivatives of Tape, Tube and Rectify are evaluated for a whole oversampled frame at once, both channels together (32 values). The polynomial uses a Cody–Waite reduction and e^r through r^13/13! for the exponential, and 2 atanh(z / (2 + z)) through s^33 for log1p. It has no branches or calls, so each step runs over every value and vectorises. It is within 3e-16 of the exact value for |u| ≤ 1, and 3e-16 |u| beyond, as accurate as the library's.

   `the_kernel_matches_the_reference_shaper` renders every preset, and every curve biased with colour, tone and a partial mix, through the kernel and through the shaper as first written (one channel at a time, library `exp` and `log1p`). They agree within 1e-6 on noise and a chord with silent gaps.
4. **Shape's two spectra share one transform**, the input as its real part and the output as its imaginary part, separated as X[k] = (Z[k] + Z*[N − k]) / 2 and Y[k] = (Z[k] − Z*[N − k]) / 2i. Each instance starts its hops at its own point of the window, spread by the golden ratio and kept across resets, so EQs started together analyse in different blocks.
5. **Drift's shimmer shifters share one sweep.** The reverb's eight octave shifters have the same window and phase, so `OctaveSweep` works out the two taps and the crossfade once a frame instead of eight times. The output is bit-identical.

## Measured

GitHub-hosted runner: four logical CPUs (two cores with two threads each), paced fake callback, 48 kHz, with the trace on. Before: main, 20 s per run. After: this change, 60 s per run, with the default three helpers.

| Reference | Frames | p99 before | p99 after | Blocks over period, before | Blocks over period, after | Process CPU before | Process CPU after |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| light | 64 | 199 µs (15%) | 292 µs (22%) | 0 | 0 | 13.6% | 17.1% |
| light | 128 | 292 µs (11%) | 415 µs (16%) | 0 | 0 | 11.2% | 12.0% |
| heavy | 64 | 4374 µs (328%) | 907 µs (68%) | all 15,000 | 17 of 45,000 | 101.8% | 172.6% |
| heavy | 128 | 7272 µs (273%) | 1570 µs (59%) | all 7,500 | 8 of 22,500 | 101.5% | 164.7% |

- **Steps, heavy at 128 frames.** The player with one helper: 181% p99. Forge as above: 117%. Three helpers: 83%. Shape's spectra: 65%.
- **Per instance at 128 frames:** Forge 225 → 114 µs, Shape's p99 98 → 75 µs, Drift 227 → 232 µs (unchanged within the runner's noise), Nova 131 → 135 µs.
- **Light project.** Waking three helpers instead of none costs it about 100 µs at p99. It stays under a quarter of its period.

## Consequences

- **The reference machine is unchanged.** It keeps seven helpers, so its device runs (#92) remain the release evidence. A machine with four cores and SMT now gets seven helpers instead of three.
- **Still over 5% of a core per instance at 128 frames:** Drift (232 µs, nine voices of grain clouds and a shimmer reverb) and Nova (135 µs, its voices already spread over the helpers by ADR-0085).
- **Forge's sound** differs from the first implementation only in the last bits of the log-cosh antiderivatives; the test bounds the output difference at 1e-6. Every other curve, the resamplers and the dry path compute the same values as before.
