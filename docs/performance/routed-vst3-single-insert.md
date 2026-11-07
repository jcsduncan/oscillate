# Routed VST3 single-insert baseline

**Date:** 15 October 2026. **Scope:** bounded one-effect routed-live slice,
PI-04/08 and ER-01/03/08/09/10. This is a local microbenchmark, not a device
underrun, third-party compatibility or 32-track qualification result.

Intel Core i7-10700K (16 logical CPUs), Ubuntu x86-64, GCC 15.2.0 Release,
48 kHz. The workload uses two stereo source tracks through the published
routed mixer; one has the synthetic scanned/verified VST3 gain effect restored
to 0.5, and the other is dry with seven frames of edge compensation from the
fixture's declared initial plugin latency. Both sources provide constant
0.25/-0.25 samples. The test warms 1,000 blocks and times 20,000 consecutive
blocks in the same thread. An overridden allocator counts callback allocations.

| Block size | Mean complete callback | Observed allocations |
| --- | ---: | ---: |
| 64 frames | 1,159 ns/block | 0 |
| 128 frames | 2,296 ns/block | 0 |

Run with `OSCILLATE_BENCHMARK_ROUTED_VST3=1` on the Release
`oscillate_vst3_routed_insert_tests` executable. The fixture delays its
audio by the seven frames it reports; this baseline was measured without that
delay, so rebenchmark before comparing current callback time against it. The independent
application route test uses a delay processor for the parallel-path
impulse-alignment oracle. The VST3 test proves
selective track processing, saved component-state restoration, runtime-history
reuse, missing/mismatched scan rejection, separate process-fault diagnostics
and zero observed callback allocations at both block sizes.
