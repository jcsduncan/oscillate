# Optional native convolution core: local qualification

**Status:** Reusable DSP primitive only, 15 October 2026. **Requirements:**
PI-07, ER-08, PRD §§15, 17. See [ADR-0020](../decisions/0020-convolution-reverb.md).

## Build and behavior

The default presets leave the convolution core out. To build it,
configure with `OSCILLATE_BUILD_CONVOLUTION_DSP=ON`. It links the vendored
PFFFT v1.1.0 in `third_party/pffft`, which carries the reviewed
allocation-failure patch. The target is
`oscillate::convolution_dsp`; focused tests are
`native_convolution_contract`. `OSCILLATE_CONVOLUTION_SIMD=OFF` selects the
scalar comparison build.

`Convolution::create` validates a finite stereo IR with 1 to 2×sample-rate
frames, one of the four supported rates (44.1, 48, 88.2, 96 kHz), and linear
wet level 0–1. Left and right paths are independent. Preparation either
returns a complete runtime or a validation/resource error. The caller keeps
the current runtime on failure. `process` accepts at most 128 frames from a
single callback owner and does no allocation, lock or I/O. Wrong transport
generation or an oversized call outputs silence and increments a bounded
fault count. Non-finite input is zeroed; non-finite wet output is bypassed and
counted. Reset/destruction require callback quiescence.

Reported latency is zero frames. For an `N`-frame IR the deterministic tail
is `N-1` frames after the final source frame. Offline callers must render
those extra frames in blocks of at most 128. This API does not yet implement
project IR identity/storage, graph runtime reuse, replacement tail drain or
an effect editor. It must not be exposed as a completed v1 effect.

## Optional graph adapter

`ConvolutionRuntime` implements the engine's `ProcessorRuntime` interface and
splits graph blocks of at most 8192 frames into at most 128-frame calls to the
same core. It allocates no callback memory. The core owns the IR and mutable
FFT history; a compatible graph revision reuses the runtime and its tail.
The adapter reports zero latency and the exact `IR length - 1` finite tail.
Preparation, destruction and `reset_after_quiesce(generation)` remain on the
control side. A seek/new transport generation **silences** the adapter until
the callback has quiesced and that reset completes. The current graph
publication API has no callback-safe asynchronous reset handoff for this
large FFT history, so this adapter is still opt-in and is not connected to
live project insert chains or export.

The focused contract now additionally renders an 8192-frame block through
the adapter, verifies the split against known FIR taps and an across-block
tail, counts zero callback C/C++ allocations, discards oversized input without
walking it, checks quiescent generation reset, and publishes a second graph revision to prove a
running convolution tail survives compatible runtime reuse. This does not
close IR asset ownership, seek handoff, incompatible-replacement tail drain,
UI/editor, listening quality, or multi-instance CPU admission.

### Adapter cost probe

The optional `oscillate_convolution_runtime_bench` compares a direct core
call with the graph adapter using a dense two-second stereo IR at 48 kHz and
64/128-frame blocks. It runs each case in a fresh process (one second warmup,
one second measured) and reports callback wall time. Intel i7-10700K,
GCC 15.2 Release, PFFFT SSE, three runs per case, 16 October 2026. Median
total microseconds across the three runs for one second of audio:

| Instances | Frames | Direct core | Adapter | Resident increase per fresh process |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 64 | 12,285 | 11,738 | 3,457,024 bytes (both) |
| 1 | 128 | 11,332 | 11,628 | 3,457,024 bytes (both) |
| 4 | 64 | 93,133 | 98,899 | 13,643,776 bytes (both) |
| 4 | 128 | 102,519 | 96,009 | 13,643,776 bytes (both) |

The callback spread is larger than these small differences, so this is not
evidence of an adapter speedup or a regression. The adapter adds no block-sized
scratch or FFT storage; the resident measurements are page-granularity
`/proc/self/statm` deltas after IR input creation. At four instances, measured
worst callbacks crossed the 64- and 128-frame periods in some runs. Runtime
admission still needs a measured graph budget on reference hardware; this
adapter does not make a four-instance configuration safe by itself.

## Local gates

The test uses an independent double-precision direct FIR oracle for impulse,
random and dense maximum-length IRs at all four rates. It compares 64-,
128- and 67-frame partitions, checks tail/latency, channel isolation,
non-finite input/output, generation reset, invalid/oversized calls, injected
PFFFT setup allocation failures, and callback `new`/`malloc` counts. The
normal-tree target and standalone prototype compile the same processor source.

| Gate | Result |
| --- | --- |
| GCC 15.2 Debug with warnings as errors | 1/1 focused CTest pass |
| Clang 21.1.8 C++ with GCC 15.2 C | 1/1 focused CTest pass |
| GCC 15.2 scalar PFFFT standalone | 1/1 focused CTest pass |
| GCC ASan/UBSan | 1/1 focused CTest pass |
| GCC TSan | 1/1 focused CTest pass; there is no concurrent runtime mutation in this primitive |
| Source line coverage | 183/184 (99.5%); uncovered line is a separate aligned-buffer allocation failure |
| Callback allocation probe | 0 C/C++ allocations in 128 full-IR callbacks per supported rate |
| clang-format / clang-tidy | Focused source and test files pass |
| Graph adapter focused Debug and ASan/UBSan | 1/1 `native_convolution_contract` pass in each build |
| Graph adapter formatting / clang-tidy | Changed C++ files pass |

## Uninstrumented callback cost

Intel Core i7-10700K, GCC 15.2.0 Release (`-O3`), upstream x86 SSE PFFFT,
dense two-second stereo IR per processor. Three seconds of warmup followed by
two seconds measured per case. The table records one local run with the
benchmark's wall-clock percentiles; the period is sample-buffer duration.
This is serial processing without a device, graph, plugins or competing
production workload. Counts are measured callbacks longer than their period,
not device xruns.

| Rate | Instances | Frames | Period µs | Median µs | p99 µs | Worst µs | Over-period callbacks |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 48 kHz | 1 | 64 | 1,333 | 9.47 | 106.54 | 190.05 | 0 |
| 48 kHz | 1 | 128 | 2,667 | 23.82 | 113.29 | 174.82 | 0 |
| 48 kHz | 4 | 64 | 1,333 | 39.44 | 645.05 | 831.95 | 0 |
| 48 kHz | 8 | 64 | 1,333 | 81.80 | 2,602.41 | 3,807.03 | 92 |
| 96 kHz | 1 | 64 | 667 | 17.64 | 287.83 | 550.29 | 0 |
| 96 kHz | 4 | 64 | 667 | 72.84 | 1,496.66 | 2,809.24 | 94 |

One run is insufficient to admit even one instance under the 32-track
reference workload. The eight-instance and high-rate cases show substantial
deadline risk. The graph owner must enforce a measured IR/instance budget
before live activation and separately test tail preservation during graph
edits. Listening quality with licensed speech/percussion/music and reference
IRs, runtime ISA dispatch, package notices and AppImage SBOM remain open.
