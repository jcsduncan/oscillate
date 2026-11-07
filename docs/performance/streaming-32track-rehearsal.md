# Managed-media 32-track local rehearsal

**15 October 2026.** This opt-in workload extends the in-memory
[routed graph rehearsal](routed-32track-rehearsal.md) through actual managed
WAV assets, the routed source worker and a paced fake stereo callback. It is
local evidence for the streaming/routing contracts, not PRD G7 release
qualification or an audio-device xrun measurement.

## Workload

The fixture creates a project with 32 audio tracks, two group buses, two
returns and one send per audio track. Each audio track imports a distinct,
generated eight-second 48 kHz mono PCM16 waveform as its own managed source. Half the
tracks route to each group and each track also sends at gain 0.125 to its
corresponding return. The saved master chain runs a peaking EQ, compressor,
algorithmic reverb and sample-peak limiter. The fixture closes and reopens the
project, checks the saved workload shape, verifies the media through
`RoutedAudioSource::prepare`, and primes 32 separate source rings. One worker
streams all 32 readers; the callback reads their prefetch rings, mixes the
routed graph and interleaves stereo output.

For each 64- or 128-frame size, the default run prepares a fresh control,
source and worker three times. It warms 100 blocks, then paces 2,000 blocks
at the nominal 48 kHz period with `sleep_until`. Each callback is timed using
`steady_clock`; the scalar C++ `new` allocation probe covers that callback
thread. There is no timing pass/fail threshold. The executable does require
successful rendering, finite and audible output, an exact completed transport
position, zero source-missing frames, zero worker failures and zero counted
callback allocations. It reports blocks whose processing time exceeded the
nominal period, which is **not** a count of device xruns.

## Local result

Host: Ubuntu 26.04.1 LTS, Linux 7.0.0-38-generic, Intel Core i7-10700K
(8 cores, 16 logical CPUs), GCC 15.2.0 Release, 48 kHz. CPU frequency and
background activity were not controlled. The three repetitions render 2.8
seconds of audio each at 64 frames and 5.6 seconds each at 128 frames.

| Frames | Mean processing time per repetition (µs/block) | p99 range (µs) | Worst block (µs) | Over nominal period | Missing frames | Callback `new` | Failures |
| ---: | --- | --- | ---: | ---: | ---: | ---: | ---: |
| 64 | 85.481, 81.270, 85.734 | 138.404–156.214 | 1,049.417 | 0/6,000 | 0 | 0 | 0 |
| 128 | 173.525, 163.706, 155.215 | 243.850–255.434 | 986.381 | 0/6,000 | 0 | 0 | 0 |

The nominal periods are 1,333.333 µs and 2,666.667 µs. The observed
processing times include routed mixing, native master processing and copying
from managed-source rings; worker-side disk reads and decoding happen
concurrently and are excluded from callback time. The probe does not count
aligned `new`, direct C allocation or allocation on the worker thread. The
generated waveforms are deliberately simple, and all are short files that can
reside in the OS page cache. This does not establish cold-cache disk contention
behaviour for 32 distinct long media files.

The fake callback uses host thread scheduling and does not open PipeWire,
JACK or a physical device. The baseline above has no VST3 insert; an optional
single verified VST3 effect is covered in the
[mixed-insert extension](streaming-32track-vst3-rehearsal.md). Source-rate
conversion, encoded media, automation, dynamic
graph edits, UI load and third-party plugins are also absent. A representative
mixed project, physical-device xrun/error-counter evidence and controlled
reference-hardware repetitions remain open release gates.

## Disposable-target PipeWire rehearsal

An opt-in runner opens the same saved 32-track managed-WAV workload through
`PipeWireOutput` instead of the paced fake callback. It creates a named
low-priority null sink, selects that sink by its PipeWire object serial, records
its monitor at stereo 48 kHz float32, and removes the sink after the run. The
target serial is mandatory for the executable's live mode; the default output
is never selected. One run per requested 64 and 128 frames uses 100 warmup
blocks and 2,000 measured blocks by default. These are bounded playback
windows, not statistical repetitions or a 32-track release qualification.

The executable records the actually observed callback size, negotiated client
rate, observed graph rate, completed transport frames, render failures, source
missing frames, worker failure, oversized callback blocks and invalid buffers.
The runner checks active playback links to its sink and a finite, audible
monitor recording. The backend's `OutputStatus::xruns` is unavailable because
the PipeWire stream API does not provide a trustworthy per-stream xrun count.
Zero source/render failures must therefore never be reported as zero xruns.

Local rerun on 15 October 2026 after schema-1.13 fixture repair used the same
Intel i7-10700K host, GCC 15.2 Release and a named disposable null sink. At
both 64 and 128 frames, the plain 32-track run completed 2,000 measured blocks
plus 100 warmup blocks at 48 kHz, with zero source-missing frames, worker
failures, render failures, oversized buffers and invalid buffers. The monitor
was finite and audible, and the sink was removed. With one freshly scanned
synthetic VST3 gain effect on the first track, both block sizes also passed:
the independent wet/dry probe measured gain ratio 2.00037, and the live run
reported zero plugin faults. Per-stream xruns remained unavailable. These are
one-run development rehearsals with generated eight-second WAV sources and one
synthetic plug-in; they do not qualify the reference 32-track production mix.

```sh
python3 tests/qualification/qualify_live_streaming_32track.py
# Short integration run of one requested size:
python3 tests/qualification/qualify_live_streaming_32track.py \
  --frames 64 --blocks 200 --warmup 20
```

The runner expects the Release target built below and a running PipeWire
server with WirePlumber, `pw-cli`, `pw-dump` and `pw-record`. The monitor is
captured from a virtual sink, not a physical DAC. CPU frequency, external
load, stream scheduling, graph quantum and disk cache are not controlled. A
reference-hardware run with a defined plugin workload, observed physical
device xruns and repetitions remains a separate release gate.

Local run on 15 October 2026: Ubuntu 26.04.1 LTS, Linux 7.0.0-38-generic,
Intel i7-10700K, GCC 15.2.0 Release, PipeWire 1.6.2, WirePlumber 0.5.13 and
a 48 kHz PipeWire null sink. The 64-frame
request observed 64-frame callbacks, completed 134,464 frames against a
134,400-frame target, and captured 154,048 monitor frames with 140 audible
20 ms windows and a 0.8913 peak. The 128-frame request observed 128-frame
callbacks, completed 268,928 frames against a 268,800-frame target, and
captured 292,608 monitor frames with 281 audible windows and the same peak.
Both runs reported zero render failures, missing source frames, worker
failures, oversized blocks and invalid buffers. Backend xruns were reported
as **unavailable**. Monitor frame totals include recorder startup and stop
padding, so they are not an exact callback count. A short 64-frame,
200-block/20-warmup integration run also passed. This host's CPU frequency,
PipeWire scheduling and background load were uncontrolled.
Clang 21.1.8 Release repeated both live sizes with zero reported source,
render or buffer failures, finite audible monitor captures and the requested
callback sizes observed. The last observed callback size is an atomic status
sample, not a histogram of every callback; the harness would also fail if an
oversized callback exceeded the prepared graph size.
The GCC ASan/UBSan build passed a paced 64-frame fake-callback correctness
run with zero missing frames or render/worker failures. Two short attempts to
run its live 128-frame path ended with external SIGKILL (`-9`) after stream
readiness and without a sanitizer diagnostic. Other local builds were active
and swap was full, so this is **no live sanitizer result**; it is neither a
performance result nor evidence of a DAW defect. Revisit that check with an
uncontended machine before relying on instrumented live coverage.

## Reproduce

Configure the local dependency paths as in the main build when needed. This
target is not registered with ordinary CTest:

```sh
cmake -S tests/qualification -B build/streaming-32 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DOSCILLATE_WARNINGS_AS_ERRORS=ON
cmake --build build/streaming-32 --target oscillate_streaming_32track_rehearsal
build/streaming-32/oscillate_streaming_32track_rehearsal --frames 64
build/streaming-32/oscillate_streaming_32track_rehearsal --frames 128
```

For a short contract run, pass `--blocks 200 --warmup 20 --repetitions 1`.
Clang 21.1.8 Release built with warnings as errors and passed a short
128-frame run. GCC ASan/UBSan built with warnings as errors and passed a short
64-frame run with `--pace-multiplier 8`; no sanitizer diagnostic appeared.
The instrumented invocation was:

```sh
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  build/streaming-32-sanitize/oscillate_streaming_32track_rehearsal \
  --frames 64 --repetitions 1 --blocks 200 --warmup 20 --pace-multiplier 8
```

That option slows the fake caller for instrumented correctness testing and
defaults to 1 for the Release measurements above. At nominal 64-frame pacing,
the instrumented build ran slower than the real-time period and reported
350,400 missing track-frames over the short run. Sanitizer timing is excluded
from the performance table; the failure demonstrates why its worker and
callback speed cannot be used for a release performance claim.

The playback source's separate fake-device tests cover exact sample oracles,
seek and worker-failure paths. This rehearsal is intentionally one workload
observation, not another copy of those unit tests.

## 18 October 2026 local rerun

The existing `build/streaming-32` GCC 15.2 Release target was rebuilt with
warnings as errors after the live final-stage graph handoff change. A GCC
`maybe-uninitialized` diagnostic in the unrelated collection path was removed
by comparing immutable source fields directly, without copying optional link
state. The focused collection test passed in the canonical Quick build.

The disposable PipeWire null-sink runner completed one 2,000-block measured
run plus 100 warmup blocks at each size. The plain 32-track project and the
combined project with one synthetic VST3 effect **and** one synthetic VST3
instrument each observed the requested client block size and 48 kHz client and
graph rates. All four runs reported zero render failures, missing source
frames, worker failures, invalid or oversized blocks, callback blocks over the
nominal period, and VST3 faults. The worst measured callback times were 570.2
and 851.8 µs for plain 64/128, and 529.9 and 1,092.6 µs for the combined
64/128 case. The synthetic effect wet/dry energy ratio was 2.00036 and the
instrument produced audio from silent audio input. All monitor captures were
finite and audible, and each disposable sink was removed. PipeWire again
reported attributable per-stream xruns as **unavailable**.

These are single local runs with generated, cached WAV sources and simple
synthetic plugins. They neither establish the reference 32-track production
workload nor physical-device xrun qualification.
