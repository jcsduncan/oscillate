# ADR-0020 — Convolution reverb core and FFT boundary

**Status:** Accepted; implemented as an opt-in build (`OSCILLATE_BUILD_CONVOLUTION_DSP`): offline export renders a saved convolution insert; the Quick app refuses live playback of a project that holds one. Release packages leave it out.
**Date:** 15 October 2026
**Requirements:** PRD §§5, 7, 15, 17, 21–22; FS-06 native processing and export tails.

## Decision

Use the single-precision real FFT and frequency-domain multiply operations of
[PFFFT v1.1.0](https://github.com/marton78/pffft/tree/v1.1.0), peeled at commit
`1fdcb508e5082052ab406d560b7c4622b8b4b0dd`, as the candidate for a
**CPU-only, stereo, zero-added-latency** convolution reverb. The first core
accepts two independent, sample-rate-matched finite IR channels, at most two
seconds long, and a bounded linear dry/wet control. It preserves the original
IR and records its hash, rate, length and preprocessing version in the owning
project/asset layer; that persistence layer is outside this decision.

The DAW owns partition scheduling and processor runtime state. PFFFT is an FFT
primitive, not a graph, state, cache, routing or project dependency. IR loading,
resampling, trimming, normalisation policy, FFT preparation and all allocations
run off the audio callback. Processing may only read prepared aligned spectra,
bounded rings and preallocated scratch. No callback lock, heap allocation, I/O,
GPU call or PFFFT setup/destruction is permitted. SIMD builds must retain a
tested scalar baseline; any wider ISA variant needs runtime dispatch and an
output oracle. No CUDA path is selected for live convolution.

## Partition and timing contract

At 48 kHz the proposed non-uniform schedule is:

| IR tap range | Method | Partition | Transform | Maximum partitions |
| --- | --- | ---: | ---: | ---: |
| 0–63 | direct stereo FIR | sample | none | 64 taps |
| 64–2,047 | uniform frequency-domain | 64 frames | 128 real | 31 |
| 2,048–8,191 | uniform frequency-domain | 256 frames | 512 real | 24 |
| 8,192–95,999 | uniform frequency-domain | 1,024 frames | 2,048 real | 86 |

The ranges denote **absolute IR tap positions**, so each frequency-domain
tier begins at least one of its own partition lengths after tap zero. A tier
uses only completed input blocks when producing its next output block. The
direct head supplies the current sample. This should yield exactly
`y[n] = Σ h[k]x[n-k]` from the first input sample, without adding algorithmic
latency, even across 64- and 128-frame callbacks. The implementation must prove
that equation against an independently coded double-precision direct FIR
oracle for impulse, random and boundary-spanning fixtures; block partitioning
and silence flushing must not change output beyond a stated float tolerance.
The tiers and absolute tap boundaries scale by sample rate only after measuring
44.1, 48, 88.2 and 96 kHz; no implicit callback-rate switch may alter an IR.

The maximum deterministic wet tail is `IR length - 1` frames after the final
nonzero source sample. The processor reports zero added latency, and export
renders exactly the policy-required tail after the last source frame. The
effect must not truncate tails when used on master or processed stems. Topology
edits reuse compatible live runtime state; an incompatible IR replacement
prepares a new runtime off callback, then uses the graph's visible transition
policy. The old tail is retained or explicitly faded/drained within a measured
budget. A failed preparation leaves the active runtime and saved IR reference
untouched, reports the failure, and never silently substitutes another IR.

The initial channel mode is **dual mono stereo**: left IR applies to left
input, right IR to right input. A four-IR true-stereo matrix is a separate
quality/CPU decision. Imported IRs with wrong rate, channel count, non-finite
samples, empty data, more than two seconds, or unsupported format are rejected
before publishing a runtime. Wet output is guarded against non-finite values;
the callback records a bounded fault indicator and bypasses the wet path rather
than letting invalid samples enter the graph. The exact behaviour of a sudden
runtime fault needs a fixture before integration.

## Why this candidate

[PFFFT's license](https://github.com/marton78/pffft/blob/v1.1.0/LICENSE.txt)
permits source and binary redistribution with attribution and no endorsement;
the same repository provides the real FFT and multiply primitives. Its bundled
`PFFASTCONV` helper is **not** selected: its setup chooses an FFT at least as
large as the entire filter and `pffastconv_apply` does not implement the
non-uniform partition schedule required for a long IR at small callback sizes.
[KissFFT](https://github.com/mborgerding/kissfft/blob/master/COPYING) is
BSD-3-Clause and a viable scalar comparison, but its current SIMD path and
long-IR throughput have not been qualified here. [FFTW](https://www.fftw.org/)
is GPL licensed and is excluded from the selected redistribution path unless
a different product-licence decision is approved. The PFFFT source and exact
transitive/packaging inventory are in the [dependency note](../dependencies/pffft-convolution.md).

## Measured evidence and implementation gate

An uninstrumented FFT-only spike on the i7-10700K at 48 kHz measured median
single-transform times of 0.117 µs (128 real), 0.410 µs (512 real) and
1.798 µs (2,048 real) with PFFFT's enabled SIMD path; its scalar build measured
0.259, 0.473 and 2.025 µs respectively. These numbers **do not** measure a
convolver, frequency-domain multiply accumulation, cache pressure, input copy,
or graph scheduling. They establish only that the selected transforms merit a
prototype; no processor CPU or underrun claim follows.

Before integration, the owning native-DSP stream must publish:

1. A pinned source/build path and complete SBOM/notice inventory for the
   distributed Ubuntu package and AppImage. The upstream v1.1.0 CMake build
   failed locally with `INSTALL_PFFFT=OFF` due to an unknown export target, so
   the integration must not rely on that setting without a tested fix.
   As built, the pinned source is vendored at `third_party/pffft` with the
   DAW's local fix in `third_party/pffft/patches/allocation-failure.patch`,
   so no fetch or external source path is involved.
2. A standalone runtime with IR bound, preallocated state, explicit reset and
   fault reporting; measured worst-case and median 48 kHz CPU at 64 and 128
   frames, including a full two-second stereo IR and both scalar/SIMD builds.
   The maximum callback work at tier boundaries must fit a stated share of the
   buffer period on the reference machine. Measure 44.1/88.2/96 kHz too.
3. Independent FIR, tail, partition, generation/reset, malformed-IR,
   interruption, allocation, ASan/UBSan, Clang/GCC and coverage evidence.
   Listening tests must include speech, percussion and sustained musical
   sources with more than one licensed reference IR.
4. A graph/export integration test proving zero added latency, tail inclusion,
   runtime reuse and failure visibility. The project format and UI must not
   claim convolution reverb until this gate closes.

This ADR selects the prototype direction and bounds it. It does not claim a
production convolution reverb or close the PRD feature.

## Native core follow-up (15 October 2026)

The measured processor now has a public native-DSP interface and an optional
`oscillate::convolution_dsp` target. Its standalone oracle and optional
repository target compile the same implementation. The normal application
does not link it. Each call accepts at most 128 stereo frames. Preparation
accepts only 44.1/48/88.2/96 kHz, a finite dual-mono IR of at most two seconds,
and a wet level of 0–1. An oversized call fails closed to silence and
increments the fault count. IR preparation, reset and destruction remain
control-side operations after callback quiescence. See the
[native-core evidence](../performance/convolution-native-core.md).

This closes the standalone runtime and test portion of gates 2/3 above; it
does not close listening quality, safe multi-instance admission, graph/export
tail integration, project IR ownership, runtime-dispatched SIMD or distribution
inventory. The compile-time scalar/SSE choice is not an ISA dispatch policy
for a distributable binary.
