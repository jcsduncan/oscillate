# ADR-0126 — One realtime FFT for the audio thread

**Status:** Accepted and implemented. **Date:** 11 November 2026.
**Builds on:** ADR-0026 (warp), ADR-0064 (Temper), ADR-0082 (Strata's linear bank), ADR-0115 (Cast), ADR-0119 (Keel), ADR-0125 (the master chain's cost and its gate).
**Contract:** `warp_dsp::RealFft`, `warp_dsp::StagedRealFft`, `warp_dsp::AlignedBuffer` and `warp_dsp::bin_power` (`include/oscillate/warp_dsp/real_fft.hpp`); `native_dsp::dsp::OfflineFft` (`src/native_dsp/dsp/offline_fft.hpp`); `native_dsp::dsp::vector_math` (`src/native_dsp/dsp/vector_math.hpp`); the master gate's limits in `.github/workflows/performance.yml`. No schema change.

## Context

After ADR-0125 the heavy master chain cost 2.56 ms of serial time per 1024-frame block, and Temper took 1.04 ms of it. Every effect that analysed or convolved on the callback used `dsp::Fft`: a double-precision complex radix-2 transform, fed real signals, often two at a time packed as one complex signal. These were Temper, Keel, Gild, Strata, Shape, the linear-phase bank (Strata, Strike, Gild), Scope, Tune, and Nova's Convolve module and FX scope. Warp and Cast already used the pinned PFFFT, through two separate wrappers.

## Decision

1. **One realtime transform.** `warp_dsp::RealFft` wraps PFFFT's single-precision real SIMD transform. Its header is public in `warp_dsp`, the lowest DSP module, so the native DSP and Clay can link it.
   - A transform allocates nothing, takes no lock, and gives the same result for any split of the audio into callbacks. Only construction allocates, on control.
   - Each instance owns its work buffer. Buffers are `AlignedBuffer`s or 16-byte offsets into one; debug builds assert the alignment.
   - **Ordered spectra** (DC, Nyquist, then bins 1 to N/2 − 1) are for paths that read bins.
   - **Unordered spectra** with `multiply` and `multiply_accumulate` (PFFFT's z-domain products) are for paths that only convolve or correlate. They skip the reordering.
   - Warp, Clay and Cast share this wrapper. Cast's own wrapper is gone, and the native effects reach PFFFT only through `warp_dsp`.
2. **A staged transform for Keel.** `StagedRealFft` runs `RealFft::forward` in steps of about equal cost, so a long transform can be spread over many callbacks.
   - It treats the N/2 complex points as rows by columns (the four-step transform): a short PFFFT complex transform per column, twiddles, a short transform per row, then the real spectrum split off in chunks.
   - At 16,384 points that is 256 steps; at 65,536 points (192 kHz) it is 512.
3. **The double transform is offline only.** It is renamed `dsp::OfflineFft`, and its staged interface is removed. Its users are:
   - wavetables and Nova's tables;
   - key, tempo, mix role and mix check analysis;
   - the spectrum analyser and tonal analysis;
   - Lens, which has analysed on the reading thread since ADR-0125;
   - Cast's IR design.

   Debug builds assert that no thread marked by `foundation::RealtimeThreadScope` (the callback and the audio helpers) runs it.
4. **Every audio-thread user moved.** Each takes a real transform per signal instead of packing two:
   - **Temper** analyses each processed channel with one transform. Each channel's minimum-phase design is three real transforms. The overlap-add's partition products accumulate on unordered spectra.
   - **The linear bank** takes one forward transform per channel and hop, then one product and one inverse per band except the last. The last band is the delayed input less the others, so the bands sum to the delayed input to double rounding, whatever the transform's rounding. The last band also needs no transform.
   - **Keel** windows each channel in pieces and runs `StagedRealFft` step by step, a share every 16 frames. Each channel adds half its band powers.
   - **Scope** correlates a's spectrum with that of b reversed in time, which is conj(B).
   - **Tune** takes its NSDF's autocorrelation as the inverse of the power spectrum.
   - **Nova's Convolve** keeps its partitions as unordered spectra scaled by 1/n.
   - **Gild's, Strata's, Shape's and Nova's display spectra** each take one ordered transform per signal.
5. **Temper's hop.** The 4,098 complex exponentials per hop are e^z of the folded cepstrum's spectrum, which is data. They cannot be precomputed or rotated incrementally.
   - `vector_math` evaluates them four at a time in SSE2, using Cephes's single-precision exp and sincos (within 4·10⁻⁷ of the library's). This takes about 3.6 ns per value. The same header gives Temper's level logarithms.
   - Fully linked channels (Link 100%, the default) design one filter. Link's blend now lands exactly on the louder channel, not within a rounding of it. Equal cuts and band gains are detected bitwise, and one design and one set of partition spectra serve both channels.
   - Other per-hop work removed:
     - each design bin's place on the grid is computed once;
     - only switched-on dynamic bands are added;
     - Detail's widths are kept until Detail or Tilt move;
     - the cut's skirts spread only from bands that cut;
     - the soft threshold's spread is summed four ways;
     - only Low Latency keeps the previous filters;
     - a fully wet display skips its dB round trip.
6. **The gate is tightened** to lock the gain in (see Measured).

## Measured

i7-10700K, GCC 15.2, Release, one core unless stated.

`oscillate_fft_bench`: one forward and one inverse transform, in µs, best of five. "Offline" is the offline complex transform of N points on a real signal, as the effects ran it. "Realtime" is the real transform, ordered and unordered.

| N | Offline | Realtime | Unordered | Speed-up |
| ---: | ---: | ---: | ---: | ---: |
| 256 | 2.31 | 0.45 | 0.36 | 5.1× |
| 512 | 5.15 | 0.89 | 0.82 | 5.8× |
| 1024 | 11.1 | 1.71 | 1.63 | 6.5× |
| 2048 | 25.3 | 3.96 | 3.31 | 6.4× |
| 4096 | 65.9 | 8.57 | 7.19 | 7.7× |
| 8192 | 162 | 22.9 | 21.9 | 7.1× |
| 16384 | 358 | 48.6 | 47.1 | 7.4× |

`oscillate_master_chain_bench`: the heavy master chain on ten seconds of a mix, in mean ms per block, best of three.

| Device | 256 before | 256 after | 512 before | 512 after | 1024 before | 1024 after |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Temper (Default) | 0.260 | 0.054 | 0.519 | 0.106 | 1.095 | 0.212 |
| Ceiling (Loud Club) | 0.150 | 0.154 | 0.298 | 0.306 | 0.631 | 0.612 |
| Gild (Warm Mids, 4x) | 0.137 | 0.139 | 0.271 | 0.275 | 0.572 | 0.545 |
| Lens | 0.000 | 0.000 | 0.001 | 0.001 | 0.001 | 0.001 |
| Keel (Gentle Polish) | 0.053 | 0.044 | 0.105 | 0.088 | 0.234 | 0.176 |
| Shape (Presence) | 0.025 | 0.023 | 0.048 | 0.045 | 0.101 | 0.088 |
| Loudness | 0.016 | 0.017 | 0.032 | 0.033 | 0.067 | 0.065 |
| **Chain** | **0.641** | **0.491** | **1.274** | **0.975** | **2.703** | **1.936** |

Temper went from 1.095 ms to 0.212 ms per 1024 frames, and the chain's serial time fell by 28%. The remaining serial time is mostly Ceiling and Gild, which are oversampled waveshapers and use no transform.

## Consequences

- Transforms on the callback are float. The effects' outputs change by rounding: about 10⁻⁷ of the signal, far below audibility.
- The linear bank's null is now exact by construction, where before it rested on the double transform's rounding.
- `dsp_primitives` links `foundation`, for the realtime-thread mark its debug assertion reads.
