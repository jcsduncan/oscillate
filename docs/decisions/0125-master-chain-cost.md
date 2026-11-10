# ADR-0125 — The master chain's serial cost: analysis off the callback, block oversampling, and a gate

**Status:** Accepted and implemented. **Date:** 10 November 2026.
**Builds on:** ADR-0040 (native effect platform), ADR-0042 (Lens), ADR-0048 (audio helpers), ADR-0061 (performance tracing), ADR-0064 (Temper), ADR-0108 (Edge's oversampler), ADR-0110 (heavy projects across the helpers), ADR-0117 (Ceiling), ADR-0119 (Keel), ADR-0120 (Lens's band meter), ADR-0122 (Gild).
**Contract:** `EffectKernel::analyses_off_callback` and `analyse_telemetry`, `NativeEffectRuntime::read_telemetry` (`include/oscillate/native_dsp/effects/effect.hpp`); `CaptureQueue` (`src/native_dsp/effects/capture_queue.hpp`); `edge_dsp::Resampler`'s block interface and `edge_dsp::block_frames`; the reference player's `--master` and its 256–1024-frame blocks; `qualify_reference_projects.py --master --gate-master-percent --gate-p99-percent`. No schema change.

## Context

On 0.5.0 a mastered project clicked and stuttered at 1024 frames. The master runs on the callback after the tracks, which run in parallel on the helpers, so its time adds to every block. A user's trace showed it at 5.4 ms a block on average and 11 ms at worst (0.22 ms on 26 October): Temper 1.59 ms, Ceiling 1.55, Gild 1.00, Lens 0.57, Keel 0.30, Shape 0.16 and Loudness 0.10. The callback's p99 was 94% of the 21.3 ms period, and 0.33% of blocks were late (0.00–0.07% on 0.3.0). Each device had been measured alone; none had been measured in a master chain on a heavy project.

## Decision

1. **Lens analyses only when it is read.** On the callback its kernel copies each frame (input and sidechain, as floats) into a `CaptureQueue` and does nothing else. The queue is single-producer, single-consumer and never waits: a block that does not fit (2 s of audio) is dropped, and the next one that fits starts a new run, as does a reset (a seek). The spectra, the averages, the resonances and the band meter run in `analyse_telemetry`, on the control thread that calls `read_telemetry`, over everything captured since the last read, hop by hop on the same grid as before; a new run starts them afresh. Values reach the analysis through atomics, at the read. Nothing is analysed while no panel, strip or match reads Lens, and its callback cost is a copy.
2. **Edge's oversampler runs a block at a time.** `Resampler::up` and `down` take up to 32 frames. Each half-band stage filters its whole stream at once, eight outputs together in SSE2 registers, each summed in tap order, so every output is the per-sample dot product's, bit for bit, for any block split. Edge, Ceiling's clipper, Gild's bands and Strike's clip feed it blocks: the per-frame work before the curve, the curve over the top-rate block, then the per-frame work after. Gild runs each band's oversampler over the stretches it is awake for. The response, aliasing floor, latency and output are unchanged.
3. **The same arithmetic, faster.** The shared FFT runs its butterflies in SSE2, two stages per pass over groups of four values, reorders by a list of swaps and scales its inverse by the exact reciprocal of its power-of-two size: the same operations on the same values, so every transform is bit-identical. Ceiling's 8x fine peak, the BS.1770 4x true peak and Keel's eleven filters (both channels as one SSE2 pair) keep each sum's order.
4. **Temper's hop** keeps its design and response:
   - its overlap-add convolves each hop's block with that hop's filter in eight partitions a hop long, each through a two-hop transform, summing each landing hop's products in the frequency domain and transforming each hop back once (ten 512-point transforms a hop instead of three 4096-point ones);
   - fully wet, the minimum-phase design takes each bin's log gain straight from its dB, and computes each bin once for the mirrored half;
   - complex products, `i` times a value and `e^z` are written out in real arithmetic, without the library's checks for infinities;
   - a single processed channel designs one filter, not two;
   - the per-band ballistic coefficients, the sensitivity curve and the display's band mapping are kept until their settings change.
   The filters differ from the previous ones by rounding only.
5. **A gate in CI.** The reference player gains `--master heavy`, a finished master's chain (Temper, Ceiling at Loud Club, Gild at Warm Mids, 4x, Lens, Keel at Gentle Polish, Shape at Presence, Loudness), and blocks of 256, 512 and 1024 frames. The performance workflow's master job plays the light reference with it at 1024 frames and fails when the master node's mean time passes 20% of the period or the callback's p99 passes 35%. The light reference leaves most of each block to the master, so its cost shows plainly; on the four-core hosted runners the heavy reference alone fills the period, so it is measured at each size and reported, not gated. The workflow runs on pull requests that change the effects' DSP, the shared DSP or the engine.
6. **The master is not pipelined.** Rendering block n's master while the helpers render block n + 1's tracks would take it off the critical path for one block of output latency. It is investigated below and not shipped.

## Measured

`oscillate_master_chain_bench`: the heavy master chain on ten seconds of a mix (bass, chord partials, kicks, hiss), each device's mean milliseconds a block, best of three; i7-10700K, GCC 15.2, Release, one core:

| Device | 256 before | 256 after | 512 before | 512 after | 1024 before | 1024 after |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Temper (Default) | 0.529 | 0.266 | 1.063 | 0.530 | 2.110 | 1.044 |
| Ceiling (Loud Club) | 0.370 | 0.155 | 0.739 | 0.307 | 1.472 | 0.604 |
| Gild (Warm Mids, 4x) | 0.254 | 0.140 | 0.506 | 0.277 | 1.004 | 0.543 |
| Lens | 0.118 | 0.000 | 0.232 | 0.001 | 0.452 | 0.001 |
| Keel (Gentle Polish) | 0.065 | 0.055 | 0.130 | 0.108 | 0.256 | 0.211 |
| Shape (Presence) | 0.028 | 0.026 | 0.054 | 0.049 | 0.105 | 0.095 |
| Loudness | 0.023 | 0.017 | 0.045 | 0.033 | 0.088 | 0.064 |
| **Chain** | **1.387** | **0.659** | **2.770** | **1.306** | **5.487** | **2.563** |
| Chain, p99 | 1.842 | 0.798 | 3.237 | 1.549 | 6.014 | 2.780 |

Before, the chain costs what the user's trace showed (5.4 ms at 1024 frames). The reference player, light reference with the heavy master, fake callback, one helper (two threads, capped at two cores), the master node's mean and the callback's p99 as shares of the period, same machine:

| Frames | Master before | Master after | p99 before | p99 after |
| ---: | ---: | ---: | ---: | ---: |
| 256 | 1.43 ms (26.9%) | 0.71 ms (13.2%) | 40.0% | 22.4% |
| 512 | 2.84 ms (26.6%) | 1.35 ms (12.6%) | 34.0% | 17.3% |
| 1024 | 5.60 ms (26.3%) | 2.70 ms (12.7%) | 30.9% | 17.0% |

On a hosted four-core runner (three helpers) with 0.5.0's devices the master took 30.3–32.1% of the period at every size, light or heavy; the light reference's p99 at 1024 frames was 43.2% of the period and the heavy reference's 109.7%, 291 blocks late in 20 s.

**Pipelining, investigated.** Rendering the master of block n − 1 on the callback thread while the helpers render block n's tracks costs one block of output latency (5.3 ms at 256 frames, 21.3 ms at 1024) and takes one lane from the tracks. From the measured spans, the pipelined block is about the larger of the tracks' time on one lane fewer and the master's time. On the user's trace (eight lanes, callback median 14.7 ms, master 5.4 ms) that would have saved about 28%; with this ADR's devices (master 2.6 ms) about 11%. On the four-core runner, where the tracks fill the block, it saves under 8% before and less after. Live edits, seeks, meters and the reported latency would all have to account for the extra block. Not shipped: the device work removes most of the master's time without the latency, and a pipelined master is worth revisiting only as an option for large buffers if masters grow again.

## Consequences

- A Lens no panel reads costs the callback a copy; reading it costs the reader's thread the analysis, a burst of up to one hop's transforms per poll. A panel that stalls for more than 2 s starts its averages again.
- The oversampler's state is per block, so a resampler's memory grew from rings to lines of a block plus history (about 40 KB per channel).
- The gate's limits are shares of the period on hosted runners: they catch a master that costs several times more, not a few percent. Device tables in the workflow summary show where a regression lands.
