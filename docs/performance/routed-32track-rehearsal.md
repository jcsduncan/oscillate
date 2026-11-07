# Routed 32-track local rehearsal

**15 October 2026.** This opt-in executable is a reproducible **engine-only**
rehearsal for the 32-track routing shape. It does not close the PRD's 32-track
performance or audio-device release gate.

## Workload and method

The fixed 48 kHz graph has 32 simultaneous stereo source nodes, two group
buses, two return buses, one send per source, and one master. Each source feeds
one group at gain 0.125 and one return at gain 0.03125. All four buses feed the
master at unity. Every group of four sources contains one sine-like periodic
buffer, one short repeated pulse, one saw-like buffer, and one constant buffer.
The 128-frame source buffers are generated before measurement and repeated;
they do not read managed media or run instruments. The master runs a peaking
EQ, compressor, algorithmic reverb and sample-peak limiter, with the latter's
240-frame latency represented in the route plan. This is a synthetic graph,
not a persisted production project.

Each buffer size runs three independent repetitions. A repetition prepares its
graph off the timed path, warms 2,000 blocks, then measures each of 20,000
blocks with `steady_clock`. The callback allocation probe counts ordinary
scalar C++ `new` calls on this thread during `mix_block`; it does not detect
aligned `new`, direct `malloc`, plugin-owned allocators or allocations on other
threads. The executable checks finite output, render success and zero counted
callback allocations. Timing is observed and reported, never a CTest pass/fail
threshold. A block over the nominal audio period is a processing-cost
observation, **not** a device xrun.

Host: Ubuntu 26.04.1 LTS, Linux 7.0.0-38-generic, x86-64, Intel Core
i7-10700K (8 cores, 16 logical CPUs), GCC 15.2.0, CMake 4.2.3. Release build,
without sanitizer/coverage instrumentation. Background load and CPU frequency
were not controlled. The 20,000 measured blocks represent 26.67 seconds of
audio per 64-frame repetition and 53.33 seconds per 128-frame repetition;
measurement executes as fast as possible rather than pacing a device.

| Frames | Repetition means (µs) | p99 range (µs) | Worst observed (µs) | Blocks over nominal period | Callback `new` | Render failures |
| ---: | --- | --- | ---: | ---: | ---: | ---: |
| 64 | 42.191, 41.951, 41.854 | 46.114–48.845 | 70.103 | 0/60,000 | 0 | 0 |
| 128 | 82.285, 83.860, 82.184 | 87.215–113.505 | 124.451 | 0/60,000 | 0 | 0 |

The nominal periods are 1,333.333 µs and 2,666.667 µs. The largest observed
block was below either period in these unpaced runs. No source-failure count is
available because source buffers are already in memory. No xrun count is
available because this executable does not open PipeWire, JACK or a physical
device. The graph also excludes VST3 effects and instruments, decoded-media
prefetch, source-rate conversion, automation, graph edits, UI/background load
and physical-device scheduling. A representative mixed project and controlled
reference-hardware/device repetitions remain required for the release gate.

## Reproduce locally

Set `PKG_CONFIG_PATH`, `PKG_CONFIG_SYSROOT_DIR` and `CMAKE_PREFIX_PATH` if the
usual local dependencies are unpacked outside system paths. This is separate
from ordinary CTest:

```sh
cmake -S tests/qualification -B build/routed-32track-rehearsal -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DOSCILLATE_WARNINGS_AS_ERRORS=ON
cmake --build build/routed-32track-rehearsal \
  --target oscillate_routed_32track_rehearsal
build/routed-32track-rehearsal/oscillate_routed_32track_rehearsal \
  --frames 64 --repetitions 3
build/routed-32track-rehearsal/oscillate_routed_32track_rehearsal \
  --frames 128 --repetitions 3
```

For a short correctness run, pass `--blocks 200 --warmup 20 --repetitions 1`.
The Clang 21.1.8 Release build with warnings as errors ran both block sizes
without allocation or render failures. GCC ASan/UBSan with warnings as errors
also ran both sizes, with no sanitizer finding; its timings are deliberately
excluded from the table. Repository `clang-format` and focused `clang-tidy`
passed. The existing routed mixer contract tests cover route validation,
sample-level mixing, PDC and callback allocations independently of these
timing observations.

After merging, the same GCC Release target was rebuilt and rerun on the same
host while other local builds were active. Three 64-frame repetitions averaged
41.62–43.65 µs with a 110.87 µs worst block. Three 128-frame repetitions
averaged 82.90–87.82 µs; one 1,230.40 µs scheduling outlier appeared, still
below the 2,666.67 µs nominal period. Both sizes again reported zero blocks
over period, zero counted scalar C++ allocations and zero render failures over
60,000 measured blocks each. This uncontrolled repeat reinforces the engine
rehearsal only; it does not measure device xruns or production-project load.
