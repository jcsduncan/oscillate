# Convolution reverb: isolated multirate prototype evidence

**Status:** Measured DSP candidate, not a DAW effect. 15 October 2026.
**Requirements:** PI-07, ER-08, PRD §§15, 17. [ADR-0020](../decisions/0020-convolution-reverb.md)
sets the candidate and open production gates.

## Reproduction status

This page records the 15 October isolated prototype run. The standalone
prototype is not in this checkout, so its original command line cannot be
run. The equivalent maintained
core is the opt-in convolution target under [native DSP](../../src/native_dsp/).
Its source pin and patch are in the
[PFFFT inventory](../dependencies/pffft-convolution.md). The historical
measurements below should not be represented as a fresh qualification run.

The shared implementation lives in
[`src/native_dsp/convolution.cpp`](../../src/native_dsp/convolution.cpp), with
its public contract in [`convolution.hpp`](../../include/oscillate/native_dsp/convolution.hpp)
and independent oracle in
[`convolution_tests.cpp`](../../tests/native_dsp/convolution_tests.cpp).

## Contract and oracles

The candidate accepts finite dual-mono stereo IRs up to two seconds at
44.1, 48, 88.2 and 96 kHz, with wet level 0–1. The direct FIR head and
three FFT tiers use the following rate-scaled schedule. Power-of-two
partitions round up the 48 kHz physical time span; tier boundaries round up
to a multiple of the next tier's block length. This is a provisional
throughput choice, not a psychoacoustic quality decision.

| Rate | Max taps | Direct head | Tier 1: tap range / block | Tier 2 | Tier 3 |
| ---: | ---: | ---: | --- | --- | --- |
| 44.1 kHz | 88,200 | 0–63 | 64–2,047 / 64 | 2,048–8,191 / 256 | 8,192–88,199 / 1,024 |
| 48 kHz | 96,000 | 0–63 | 64–2,047 / 64 | 2,048–8,191 / 256 | 8,192–95,999 / 1,024 |
| 88.2 kHz | 176,400 | 0–127 | 128–4,095 / 128 | 4,096–16,383 / 512 | 16,384–176,399 / 2,048 |
| 96 kHz | 192,000 | 0–127 | 128–4,095 / 128 | 4,096–16,383 / 512 | 16,384–191,999 / 2,048 |

A completed input block supplies each tier's
current output block, so the direct head produces the first sample with zero
added latency. The exact finite tail is `IR length - 1` frames after the last
input frame. An explicit control-side reset clears state; a mismatched
transport generation outputs silence without advancing state. Non-finite
input is guarded, and a non-finite wet result is bypassed with a counter.
Construction, destruction and reset must occur after callback quiescence.

The independent double-precision direct FIR oracle checks impulse and random
inputs at every direct/tier boundary and the two-second bound at all four
rates. Dense random IRs spanning each tier and dense full-length IRs are
compared under 64-, 128- and 67-frame callback partitions. Maximum accepted absolute error
is 0.0002 per channel (0.0001 on the impulse fixtures). Tests also cover
short/invalid IRs, stereo isolation, exact tail count, generation reset,
non-finite data and callback allocation. The optional GNU linker wrapper
counts `malloc` from C and `new` from C++ during 128 full-IR callbacks: zero
allocations observed. Source review also confirms all PFFFT setup and aligned
allocation calls are outside `process`, with an aligned work buffer passed
to every transform.

Deterministic GNU `malloc` fault injection now covers four construction
allocations: the C++ object, tier storage, PFFFT setup struct and PFFFT
aligned setup data. Every failure returns `resource_exhausted`; a subsequent
preparation succeeds. The two PFFFT cases pass with ASan/UBSan. Unmodified
upstream v1.1.0 dereferences null at `src/pffft_priv_impl.h:1081` and again
after aligned allocation. The vendored copy in `third_party/pffft` carries the
fix as [`allocation-failure.patch`](../../third_party/pffft/patches/allocation-failure.patch).

Focused gates: GCC 15.2 SIMD and scalar release, Clang 21.1.8 C++ release
(PFFFT C compiled by GCC), ASan/UBSan and gcovr pass for the original and
experimental partition schedules. With GNU malloc wrapping enabled in the
coverage build, the processor has 184/185 executable lines covered,
**99.5%**. The one uncovered line is failure of its separate aligned work
buffer allocation. Release and ASan builds inject all four earlier
construction failures, including both patched PFFFT setup paths.
`clang-format` and `clang-tidy` pass on all candidate C++ files.

## Callback CPU, full two-second stereo IR

Machine: Intel Core i7-10700K, 16 logical CPUs, 32 GB class desktop;
Ubuntu x86-64, GCC 15.2.0, `-O3`, no sanitizer or coverage. The benchmark
warms three seconds of 48 kHz material, then measures two seconds of
callbacks with a full dense 96,000-frame stereo IR. Every callback contains
the direct head and all due tier work. `steady_clock` wall time and Linux
thread CPU time are recorded separately; the percentile is over 1,500
64-frame or 750 128-frame callbacks. IR setup and sample generation are
outside the timed callback. Values below are one reproducible run in µs:

| PFFFT path | Frames | Period | CPU median | CPU p99 | CPU worst | Wall worst |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| SIMD (upstream x86 SSE) | 64 | 1,333 | 10.56 | 110.77 | 136.69 | 137.06 |
| SIMD (upstream x86 SSE) | 128 | 2,667 | 25.50 | 109.87 | 120.66 | 120.96 |
| Scalar | 64 | 1,333 | 13.64 | 157.30 | 185.10 | 185.68 |
| Scalar | 128 | 2,667 | 34.32 | 171.36 | 182.47 | 182.96 |

The tier-boundary spike is much larger than the median. Under concurrent
local development activity, separate SIMD runs observed wall worsts up to
1,053 µs for 64 frames (79% of that period) and 518 µs for 128 frames. These
runs were not pinned or controlled qualification runs, and one isolated
processor cannot establish a safe count of simultaneous reverb instances or
the 32-track underrun gate. Further timing on the reference machine must
separate scheduler preemption from cache/CPU contention and set a practical
IR/instance budget. The scalar and SIMD paths passed the same independent
audio oracle; the SIMD candidate improves median CPU and typical tier spikes
in the quiet run, but no production dispatch policy is selected here.

### Multirate and simultaneous-instance follow-up

The follow-up benchmark uses the same host, compiler, dense two-second IR
per independent stereo instance and a three-second warmup followed by two
seconds of measured callbacks. Each timed call processes 1, 4 or 8 separate
instances serially, corresponding to one audio thread servicing that many
reverbs. Sample generation and IR setup remain outside timing. The table
reports the largest observed callback wall time and, after the slash, the
number of measured blocks exceeding their period. The period is 1,451/2,902
µs at 44.1 kHz, 1,333/2,667 µs at 48 kHz, 726/1,451 µs at 88.2 kHz and
667/1,333 µs at 96 kHz for 64/128 frames. `Period - worst wall time` is the
corresponding worst observed headroom; a negative value indicates a
simulated deadline miss. These are **not** device underrun counts.

| kHz | Instances | SIMD 64 worst / over | SIMD 128 worst / over | Scalar 64 worst / over | Scalar 128 worst / over |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 44.1 | 1 | 119 µs / 0 | 178 µs / 0 | 175 µs / 0 | 230 µs / 0 |
| 44.1 | 4 | 2,143 µs / 19 | 3,310 µs / 2 | 926 µs / 0 | 1,972 µs / 0 |
| 44.1 | 8 | 3,022 µs / 19 | 2,256 µs / 0 | 4,073 µs / 86 | 3,393 µs / 7 |
| 48 | 1 | 136 µs / 0 | 145 µs / 0 | 183 µs / 0 | 301 µs / 0 |
| 48 | 4 | 894 µs / 0 | 899 µs / 0 | 1,850 µs / 13 | 4,835 µs / 3 |
| 48 | 8 | 3,278 µs / 49 | 2,922 µs / 2 | 9,966 µs / 98 | 5,949 µs / 92 |
| 88.2 | 1 | 335 µs / 0 | 488 µs / 0 | 1,137 µs / 25 | 1,600 µs / 4 |
| 88.2 | 4 | 5,096 µs / 86 | 3,588 µs / 59 | 6,261 µs / 87 | 3,815 µs / 86 |
| 88.2 | 8 | 5,032 µs / 88 | 6,019 µs / 86 | 15,573 µs / 275 | 11,911 µs / 93 |
| 96 | 1 | 1,001 µs / 6 | 503 µs / 0 | 1,029 µs / 14 | 649 µs / 0 |
| 96 | 4 | 3,836 µs / 94 | 1,902 µs / 94 | 3,235 µs / 94 | 6,490 µs / 94 |
| 96 | 8 | 8,189 µs / 102 | 7,102 µs / 94 | 7,309 µs / 107 | 8,479 µs / 94 |

This measurement rules out an unrestricted number of maximum-length
instances at small buffers. One SIMD instance had positive worst headroom
at 44.1, 48 and 88.2 kHz, but its 96 kHz/64-frame run missed six simulated
deadlines. Four or eight instances missed deadlines in most settings.
`steady_clock` and thread CPU measurements were close, so the observed
peaks are mostly processor/cache work rather than scheduler preemption in
these runs. The current direct-head size originally used dynamic modulo;
replacing it with a power-of-two mask reduced the 48 kHz, one-instance
64-frame CPU median from 51.61 to 9.28 µs in two uninstrumented runs on
this host, without changing oracle output. Run-to-run contention remains
substantial. A bounded instance/IR policy and better tier scheduling need
reference-hardware qualification before any real-time graph integration.

### Long-tier partition experiment

`PROTOTYPE_HALF_LONG_TIER=ON` halves the final partition from 1,024 to 512
frames at 44.1/48 kHz and from 2,048 to 1,024 frames at 88.2/96 kHz.
The direct head and preceding tier stay unchanged; the final tier still
starts at an aligned boundary. The complete independent double FIR oracle,
including 64/128/67-frame callbacks and full two-second IRs, passes. The
option is an experiment and is **off by default**.

The following matched sequential runs use the patched PFFFT SIMD build on
the host above. Each has a three-second warmup and two-second measurement
per rate/instance/buffer combination. A value is worst callback wall time
in µs followed by simulated deadline misses; the latter are periods exceeded
by the measured callback, not audio-device underruns.

| Rate / frames | Instances | Original long tier | Half long tier |
| --- | ---: | ---: | ---: |
| 48 kHz / 64 | 1 | 207 / 0 | 360 / 0 |
| 48 kHz / 64 | 4 | 1,745 / 8 | 1,618 / 2 |
| 48 kHz / 64 | 8 | 2,938 / 94 | 3,534 / 187 |
| 48 kHz / 128 | 4 | 1,016 / 0 | 1,027 / 0 |
| 48 kHz / 128 | 8 | 2,985 / 6 | 2,489 / 0 |
| 96 kHz / 64 | 1 | 805 / 8 | 512 / 0 |
| 96 kHz / 64 | 4 | 4,287 / 94 | 3,383 / 187 |
| 96 kHz / 64 | 8 | 8,354 / 100 | 6,160 / 187 |
| 96 kHz / 128 | 1 | 1,058 / 0 | 286 / 0 |
| 96 kHz / 128 | 4 | 3,258 / 82 | 1,704 / 92 |
| 96 kHz / 128 | 8 | 6,119 / 94 | 5,909 / 187 |

The smaller partition lowers some worst blocks, especially at 96 kHz, but
doubles the number of long-tier transform events and does not eliminate
multi-instance deadline misses. For 48 kHz/8 instances at 64 frames, both
worst time and deadline count increase. These unpinned, serial benchmarks
also include host contention; they establish a concern, not a certified
device limit. The half-tier worst headroom is +155 µs for one instance at
96 kHz/64 frames, but -285 µs for four at 48 kHz/64, -2,201 µs for eight
at 48 kHz/64, and -2,716 µs for four at 96 kHz/64. The experiment is
**not selected for production**.

**Proposed admission limit until reference-machine qualification:** permit at
most one live full two-second stereo convolution per graph at 44.1 or 48 kHz
with a 64-frame buffer, and at most one at 88.2 or 96 kHz with a 128-frame
buffer. At 88.2/96 kHz and 64 frames, require a shorter measured IR budget
or a larger buffer before enabling live convolution. Never silently admit
four or eight full-length instances at either buffer size. Offline export
may use more instances because it has no device callback deadline. This is
a conservative product proposal, not proof that one instance is safe under
the 32-track reference workload; graph-level CPU headroom and a shorter-IR
threshold need measurement before a production admission policy can close.

## ABI, licensing and integration proposal

PFFFT v1.1.0 is a statically compiled C implementation. Direct source
inventory for this wrapper: `include/pffft/pffft.h`, `src/pffft.c`,
`src/pffft_common.c`, `src/pffft_priv_impl.h`, and `src/simd/pf_float.h` plus
its included architecture/scalar headers. `pffft.c` and `pffft_common.c` are
the only compiled upstream translation units. The static library has
unresolved libc and `libm` symbols but no PFFFT shared-library requirement.
The upstream `LICENSE.txt` contains Julien Pommier, Hayati Ayguen, Dario
Mambro and UCAR/NCAR notices, a source/binary notice obligation and a
no-endorsement condition. Preserve that complete text in any source vendor
and binary package materials. The upstream source is vendored in `third_party/pffft`;
the only source patch adds null checks and cleanup to the setup function.
The build-local copy retains the complete header notice, and the upstream
`LICENSE.txt` is unchanged. A release must identify the patched variant,
preserve the full license text and list the patch and compiled files in its
SBOM. The release SBOM must pin its checksum, build flags, exact shipped file
list and notice location. A C++ processor ABI is **not** exported across shared
library boundaries; PFFFT's C setup stays private to this target and version.

The production core: the optional `oscillate::convolution_dsp` target
(`OSCILLATE_BUILD_CONVOLUTION_DSP`, off by default) builds the vendored,
patched PFFFT. The core is `include/oscillate/native_dsp/convolution.hpp`,
`src/native_dsp/convolution.cpp` and the `ConvolutionRuntime` graph adapter.
Projects save IR inserts and offline export renders them; Quick live playback
rejects them.

Listening comparison against licensed speech, percussion and sustained music
IRs, 32-track/64-frame qualification, a reviewed packaged PFFFT patch, graph
transitions, source-rate import/resampling and package notices remain **open**. This
prototype is not ready to close PI-07 or ship as a production feature.
