# ADR-0129 — Meters and display spectra off the callback; no fused oversampling

**Status:** Accepted and implemented. **Date:** 11 November 2026.
**Builds on:** ADR-0040 (native effect platform), ADR-0042 (Loudness), ADR-0108 (Edge), ADR-0117 (Ceiling), ADR-0122 (Gild), ADR-0125 (the master chain's cost; Lens analyses when read), ADR-0126 (the realtime FFT).
**Contract:** `TelemetryAnalysis`, `EffectKernel::analysis`, `analyse`, `analysed_telemetry` and `analysis_backlogged` (`include/oscillate/native_dsp/effects/effect.hpp`); the analysis worker (`src/native_dsp/effects/analysis_worker.hpp`); `CaptureQueue::backlogged`. No schema change.

## Context

After ADR-0126 the heavy master chain cost 1.65 ms of serial time per 1024-frame block. Devices in a chain look as if they repeat each other's work: Gild and Ceiling each oversample, and several devices meter true peak, loudness, spectra and levels. Before building anything shared, each device's cost was split into resampling, metering and the rest.

## Measured duplication

`oscillate_master_chain_bench` (the heavy master on ten seconds of a mix), i7-10700K, GCC 15.2, Release, one core. Shares were timed by interposing the library's resampler, clipper, gain stage and guard calls, and by switching single analyses off.

| Device, 1024 frames | Total | Up/downsampling | Metering and display analysis | The rest |
| --- | ---: | ---: | ---: | --- |
| Ceiling (Loud Club) | 0.59 ms | 0.12 (8× clipper: up 0.05, down 0.07) | 0.05 (output 4× true peak, K-weighted loudness) | gain stage's 8× peak lookahead 0.21, guard 0.08, curve 0.06 |
| Gild (Warm Mids, 4×) | 0.53 ms | 0.07 (one band awake: up 0.03, down 0.04) | 0.01 (display spectrum) | band split, curve, level match |
| Shape (Presence) | 0.085 ms | — | 0.065 (display spectra) | the EQ |
| Loudness | 0.063 ms | — | 0.058 (all of it) | the copy |
| Temper, Keel | 0.21, 0.17 ms | — | — | their analyses drive their filters |

A 4× true peak costs 0.057 ms per 1024 stereo frames, K-weighting and its windows 0.005 ms, and a held sample peak 0.002 ms. Ceiling's reduction-band display measured under 0.002 ms. Keel's tracker and Temper's detector are the analyses their filters follow, so they are not duplicates.

## Decision

1. **Meters and display spectra run off the callback.** A kernel declares where its telemetry is computed (`TelemetryAnalysis`):
   - `callback`: as before, after each block.
   - `on_read`: the callback copies what the analysis needs into a `CaptureQueue`, and the thread that reads the telemetry analyses it, as Lens has since ADR-0125. A capture left unread for its length is dropped and the analysis starts afresh.
   - `continuous`: as `on_read`, and the platform's analysis worker also drains it every 20 ms, so readings that accumulate (integrated loudness, the highest true peak) see every frame whether or not a panel reads them.

   A kernel may publish part of its telemetry from the callback: the reader takes the callback's newest frame, and the kernel's `analysed_telemetry` fills in the analysed values. The reader and the worker take turns under one lock per runtime; the callback only writes its queue. A runtime clears its kernel from the worker's slot, under that lock, before the kernel goes.
2. **Nothing is dropped off a realtime thread.** On a thread that is not marked realtime (an offline render, a test), `process()` drains a continuous analysis itself whenever its capture is more than half full. On the callback and the audio helpers it never waits.
3. **Which devices.**
   - Ceiling's output meters (true peak, momentary, short-term and integrated loudness), Loudness entirely, and Edge's output true peak are `continuous`, each with two seconds of capture. A Gain change in Ceiling and an Integrate switch in Loudness travel with the frame they apply to.
   - Shape's and Gild's display spectra are `on_read`, with a second of capture. Lens stays `on_read`.
   - The captures are the float frames the device outputs, so the meters read the audio as the next device hears it, within float rounding of the doubles they read before.
4. **Not built, from the measurements:**
   - **A fused oversampled domain.** Fusing Gild and Ceiling would save at most one downsampler and one upsampler: 0.09 ms per 1024 frames, 5% of the chain. Neither device is oversampled from input to output. Gild oversamples each band's residue (curve minus input), which its level match then mixes with the band at the base rate. Ceiling's lookahead gain stage runs on the base rate before its clipper, and its guard after. The two run at different factors (4× and 8×). Running those base-rate stages at the top rate would cost more than the filters save (Ceiling's gain stage alone is 0.21 ms at the base rate). Edge, Forge, Strike's clip and Contour likewise put base-rate stages (gain, mix, tone, guard) around an oversampled curve, and Forge and Contour use their own IIR resamplers, not Edge's linear-phase ones.
   - **One analysis shared by several devices at a signal point.** Once off the callback, a second true peak and loudness at the same point (Loudness straight after Ceiling or Edge) costs the worker 0.06 ms per 1024 frames, 0.3% of a core.
   - **Dropping input meters that repeat the previous device's output meter.** Each is a held sample peak: 0.002 ms per 1024 frames.
   - **Ceiling's reduction-band display off the callback.** Under 0.002 ms.
5. **The gate is tightened** to lock the gain in (below).

## Measured after

`oscillate_master_chain_bench`, mean and p99 ms a block, best of three, before and after back to back:

| Device | 256 before | 256 after | 512 before | 512 after | 1024 before | 1024 after |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Ceiling (Loud Club) | 0.148 | 0.137 | 0.296 | 0.272 | 0.590 | 0.543 |
| Gild (Warm Mids, 4x) | 0.133 | 0.130 | 0.265 | 0.259 | 0.529 | 0.517 |
| Shape (Presence) | 0.022 | 0.005 | 0.043 | 0.010 | 0.085 | 0.021 |
| Loudness | 0.016 | 0.001 | 0.032 | 0.003 | 0.063 | 0.005 |
| **Chain** | **0.416** | **0.372** | **0.831** | **0.737** | **1.654** | **1.471** |
| Chain, p99 | 0.520 | 0.458 | 0.977 | 0.857 | 1.856 | 1.659 |

Temper, Lens and Keel did not change. The chain's serial time fell by 11%. The analyses moved to the worker cost what they cost on the callback: about 0.12 ms per 1024 frames for the heavy master's Ceiling and Loudness, and Shape's and Gild's spectra only while their panels read them.

The reference player, light and heavy references with the heavy master, fake callback, one helper, pinned to two cores, before and after back to back. Process CPU includes the analysis worker:

| Reference, frames | Master before | Master after | p99 before | p99 after | Process CPU before | after |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| light, 256 | 0.46 ms (8.7%) | 0.40 ms (7.6%) | 14.2% | 12.6% | 16.4% | 16.0% |
| light, 512 | 0.90 ms (8.5%) | 0.79 ms (7.4%) | 13.6% | 11.8% | 15.6% | 15.2% |
| light, 1024 | 1.76 ms (8.2%) | 1.57 ms (7.3%) | 12.1% | 11.3% | 15.0% | 14.6% |
| heavy, 256 | 0.46 ms (8.6%) | 0.41 ms (7.6%) | 95.0% | 90.4% | 148.7% | 140.8% |
| heavy, 512 | 0.90 ms (8.5%) | 0.80 ms (7.5%) | 94.6% | 86.7% | 145.0% | 137.9% |
| heavy, 1024 | 1.78 ms (8.4%) | 1.60 ms (7.5%) | 88.2% | 121.4% | 143.4% | 139.6% |

On two cores the heavy reference's tracks fill the period, so its p99 is set by them and varies from run to run by more than the master changed, as in ADR-0126.

## Consequences

- Each continuous device keeps one capture of about two seconds (1–1.5 MB at 48 kHz); each `on_read` spectrum one of about a second (0.5 MB).
- If the worker falls more than two seconds behind, as a stalled machine might, the capture is dropped and the meters start afresh, as after a seek.
- The process has one more thread, which sleeps while no continuous analysis exists.
- Kernels hosted without a runtime (Stick's Shape) capture, nobody reads, and the capture stops at its length: they no longer compute display spectra nobody sees.
