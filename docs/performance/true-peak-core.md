# Stereo true-peak estimation core

**Requirement:** AM-05; PRD §15. **Status:** Native DSP primitive, float32 WAV master/stem report and Quick master peak hold; device qualification remains open.

The interpolator, K-weighting and gating live in one private core, `src/native_dsp/dsp/bs1770.hpp`, used by this meter, the export loudness analyser and the Loudness effect alike; `native_meter_parity_contract` checks that all three report the same values on the same signal. On analytic sines from 20 Hz to 20 kHz at 44.1, 48 and 96 kHz the 33-tap kernel reads within 0.006 dB, where the BS.1770 Annex 2 example filter over-reads by up to 0.17 dB.

`StereoTruePeakMeter` measures the largest absolute input sample and an estimated intersample peak in each stereo channel. It accepts 44.1, 48, 88.2 and 96 kHz. A control-side `prepare()` computes three DC-normalised fractional-delay kernels. The callback path evaluates phases at ¼, ½ and ¾ sample using a 33-tap Blackman-windowed sinc; the input samples supply the fourth phase. A fixed 16-frame lookahead means the latest intersample estimates lag the sample count. Offline callers drain those pending estimates with `finish()`; live callers may read the current running maximum without a drain. `reset()` clears history after a seek or discontinuity. The caller owns the meter on one thread and must arrange safe publication to any UI reader.

Each `process()` accepts at most 4,096 stereo frames and either consumes the entire block or rejects it without changing state. Processing has fixed-size arrays and no allocation, locks, file I/O or trigonometry. Nonfinite channel samples become zero and increment an invalid-frame count once per stereo frame. Finite input cannot publish an infinite peak: reconstructed values are clamped to float range. This meter does not change audio. It is separate from the existing sample-peak/RMS meter and digital sample-peak limiter.

## Signal and failure evidence

`native_true_peak_contract` uses an independently specified continuous sine at one-quarter the sample rate with π/4 phase. Its digital samples peak at `A/√2`, while the analytic intersample peak is `A`; the test checks both channels and all four supported rates. The fixture fades its boundaries so finite-stream edge ringing does not obscure the steady-state oracle. Other checks cover different block partitions, the final lookahead drain, repeated finish, reset, unsupported rates, atomic oversized-block rejection, malformed samples, finite float extremes and zero callback allocations. The focused Dev and ASan/UBSan tests pass.

## Local callback cost

Measured on an Intel Core i7-10700K (3.80 GHz), GCC 15.2.0, `-O3 -DNDEBUG`, x86-64, 48 kHz, stereo sinusoidal input, 20,000 warmed callback measurements using `steady_clock`. The scratch harness is under `build/dev/scratch`; these are standalone processor figures, not whole-graph or underrun qualification. Clock noise affects tails.

| Implementation | Block | Median | p99 | Allocations |
| --- | ---: | ---: | ---: | ---: |
| Ring index with `% 33` | 64 frames | 12.19 µs | 14.80 µs | 0 |
| Ring index with `% 33` | 128 frames | 24.38 µs | 39.02 µs | 0 |
| Bounded increment and wrap | 64 frames | 5.46 µs | 5.62 µs | 0 |
| Bounded increment and wrap | 128 frames | 10.67 µs | 12.67 µs | 0 |

The changed index operation preserves the signal oracle and block-partition result. This evidence supports the local index optimization only; a shared graph benchmark is still needed before attaching the meter to every channel.

## Limits and integration gate

This is a 4× windowed-sinc **estimator**, not an ITU-R BS.1770 or EBU R128 certified measurement. Filter-response, tolerance, programme corpus and reference-meter comparisons are still needed before claiming either standard, and a dedicated validation should cover near-Nyquist and adversarial transients. The float32 WAV export tap measures samples after mixing, effects, latency removal and optional output-rate conversion. It does not describe later dither, integer quantisation or lossy encoding. Quick's final-output callback copies at most 1,024 frames into a 64-slot SPSC queue; a worker owns interpolation. The UI shows a transport-scoped peak hold, invalidates it on seek/stop/restart and reports unavailable if blocks are dropped or a same-generation gap occurs. Focused callback-allocation, queue-loss, transport and ThreadSanitizer tests pass. Its [paced callback-copy measurement](live-true-peak-monitor.md) has no dropped blocks. Real-device CPU/underrun qualification and per-track metering remain open. The 16-frame tail drain is an offline-only operation at stream end, not an extra live processing delay.
