# Managed-WAV 32-track rehearsal with one VST3 effect

**15 October 2026.** This opt-in extension of the
[managed-WAV rehearsal](streaming-32track-rehearsal.md) keeps its 32 distinct
eight-second managed sources, two groups, two returns, 32 sends, and native
master EQ, compressor, reverb and limiter. It adds one scanned synthetic VST3
gain effect to the first audio track. The scanner-off and scanner-on runs use
the same workload and fake-callback settings; a scanner-on run with the insert
disabled is the direct paired baseline below.

The fixture scans the existing `LoadedGain` test bundle out of process, saves
one insert, closes and reopens the project, and verifies the saved class,
bundle and binary identity against the scanner receipt. It then binds that
receipt to the live routed insert provider. Before timed playback, an
independent direct-route oracle feeds the first track a constant signal and
compares the reopened project with an otherwise identical snapshot whose
insert has been removed. Across the last eight of 16 rendered blocks, the
wet-to-dry output-energy ratio is exactly 2.000. This checks that the effect
actually processes audio rather than merely surviving persistence. Timed and
live runs also reject processor faults, source-missing frames, worker failure
and render failure.

## Local paired fake-callback result

Host: Ubuntu 26.04.1 LTS, Linux 7.0.0-38-generic, Intel Core i7-10700K,
GCC 15.2.0 Release, 48 kHz. Three repetitions per size use 100 warmup blocks
and 2,000 measured blocks, paced at the nominal period. CPU frequency,
background activity and page-cache state were not controlled.

| Insert | Frames | Mean µs/block, each run | p99 µs/block, each run | Worst µs | Blocks over period | Missing/source/worker/render faults | Callback `new` |
| --- | ---: | --- | --- | ---: | ---: | ---: | ---: |
| Off | 64 | 69.945, 68.791, 70.931 | 108.598, 101.161, 113.902 | 159.729 | 0/6,000 | 0 | 0 |
| On | 64 | 69.142, 69.365, 71.553 | 98.436, 100.917, 117.967 | 157.736 | 0/6,000 | 0 | 0 |
| Off | 128 | 137.193, 129.485, 132.950 | 218.626, 192.085, 208.116 | 322.859 | 0/6,000 | 0 | 0 |
| On | 128 | 130.724, 162.519, 139.273 | 182.412, 239.530, 215.910 | 307.386 | 0/6,000 | 0 | 0 |

Each insert-on repetition reported an active insert, zero plugin faults and
the 2.000 oracle ratio. Variation, especially the second 128-frame insert-on
run, is larger than any defensible effect-cost comparison; this data does not
establish a speed benefit. The callback probe counts scalar C++ `new` calls
on that thread only; it does not count aligned `new`, C allocation, or worker
allocations. Over-period blocks are processing-time observations, not device
xruns.

Clang 21.1.8 Release with warnings as errors built the optional scanner path
and passed short 200-block/20-warmup runs at both buffer sizes. Each reported
the 2.000 audio oracle ratio, zero callback `new` calls, source/render/worker
failures and plugin faults. Short runs are correctness checks, not comparable
performance samples.

GCC 15 Debug ASan/UBSan with warnings as errors built and passed
100-block/20-warmup fake-callback checks at both sizes with
`--pace-multiplier 8`, leak detection enabled and no sanitizer diagnostic.
The 2.000 audio oracle, zero callback `new`, and zero source/render/worker or
plugin faults held. Instrumented callback times exceeded the nominal period;
they are correctness evidence only. An attempted GCC 15 Release ASan/UBSan
build stopped before execution at an existing
`src/application/revision_status.cpp`/libstdc++ vector
`-Werror=maybe-uninitialized` diagnostic under `-O3`; no Release sanitizer
result is claimed. The ordinary unsanitized GCC Release warning gate passed.

## Disposable PipeWire sink

The scanner-on insert also played through an explicitly selected disposable
PipeWire null sink, with its monitor captured. A 2,000-block/100-warmup run
observed the requested 64- and 128-frame callback sizes. Completed frames
were 134,784/134,400 target and 269,184/268,800 target respectively. The
monitor recorded 154,368 and 290,688 frames, peak 0.8913 and 140 and 280
active 20 ms windows. Source-missing frames, worker failures, render failures,
oversized or invalid buffers, and plugin faults were all zero. The backend
reported xruns as **unavailable**. Monitor totals include recorder startup
and stop padding.

## Reproduce

Configure the normal local dependency paths and VST3 SDK root before these
commands. This is a standalone opt-in project, excluded from ordinary CTest.

```sh
cmake -S tests/qualification -B build/streaming-32-vst3 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DOSCILLATE_WARNINGS_AS_ERRORS=ON \
  -DOSCILLATE_BUILD_VST3_SCANNER=ON \
  -DOSCILLATE_VST3_SDK_ROOT=/path/to/vst3sdk
cmake --build build/streaming-32-vst3 \
  --target oscillate_streaming_32track_rehearsal
build/streaming-32-vst3/oscillate_streaming_32track_rehearsal \
  --frames 64 --vst3-insert 1
build/streaming-32-vst3/oscillate_streaming_32track_rehearsal \
  --frames 128 --vst3-insert 1
python3 tests/qualification/qualify_live_streaming_32track.py \
  --executable build/streaming-32-vst3/oscillate_streaming_32track_rehearsal \
  --vst3-insert
```

Omit `--vst3-insert` for the paired baseline. The scanner-off build rejects
`--vst3-insert 1` clearly. A short correctness pass can use
`--repetitions 1 --blocks 200 --warmup 20`; sanitizer fake-callback runs need
`--pace-multiplier 8` because instrumented work cannot be interpreted as
nominal-period performance.

## Qualification boundary

This rehearsal has **one synthetic VST3 effect, not a 32-plugin or
representative instrument/effect workload**. The fixture gain processor
delays its audio by the seven frames it reports; this timed rehearsal does
not check compensation alignment. It has no VST3 instrument,
third-party GUI, parameter automation, dynamic graph edit, encoded source,
source-rate conversion or export. Short WAVs can remain in the OS page cache.
The null sink is not physical audio hardware, and its backend has no reliable
per-stream xrun count. This remains a local mixed-path rehearsal; PRD G7
reference-hardware performance and physical-device xrun qualification are
still open.
