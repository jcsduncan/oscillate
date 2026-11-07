# G7 managed-project mixed-production rehearsal

## 18 October 2026 current-core check

Rebuilt the opt-in Release rehearsal against current `main` in
`build/streaming-32` and ran the combined VST3 effect + instrument workload
once at each buffer size. Each run had 100 warmup and 2,000 paced measured
blocks. The independent effect-gain, instrument-output and send-automation
oracles still passed. The run excluded a physical audio device and used no
controlled background load, so it is a development check rather than the G7
release qualification.

| Frames | Mean µs | p99 µs | Worst µs | Over period | Callback `new` | Missing source frames |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 104.448 | 420.673 | 585.666 | 0/2,000 | 0 | 0 |
| 128 | 238.000 | 888.353 | 1,067.000 | 0/2,000 | 0 | 0 |

Both runs reported zero render failures, worker failures and plugin faults.
The measured callback ends before the device backend, so the over-period count
is not an underrun count. This single repetition cannot establish a p99
threshold under controlled load.

## 15 October 2026 baseline

Opt-in development evidence; G7 release qualification remains open.

## Reproducible workload and oracles

The standalone `oscillate_streaming_32track_rehearsal` generates 32 distinct
eight-second mono WAVs at 48 kHz, imports them into a managed project, saves,
closes and reopens it, then streams all 32 tracks through two group buses, two
return buses and a send from every audio track. The first send has four saved
musical-time gain points. Its route-level probe compares bar zero with bar one
using a constant input and no master effects: measured energy ratio 1.33289,
against the independent `(1 + 0.5) / (1 + 0.125) = 4/3` oracle. The timed
graph includes native master EQ, compressor, algorithmic reverb and limiter.

The scanner-enabled target offers two separate verified VST3 scenarios:

- `--vst3-insert 1` scans `LoadedGain`, saves one audio-track effect, verifies
  class/bundle/SHA-256 identity after reopen, and checks a 2.00036 wet/dry
  route-energy ratio against the fixture's 2× gain.
- `--vst3-instrument 1` scans `TimedInstrument`, saves one instrument track
  with four MIDI clips and one note per clip, routes it through group A and
  return A, and verifies its identity after reopen. With every managed audio
  input silent, the routed probe measures 634.606 nonzero output energy.
- Supplying both flags scans and saves both distinct identities in one project,
  verifies both receipts after reopen, and binds the effect and instrument to
  separate graph nodes. The independent effect gain ratio is 2.00036; the
  instrument produces 632.883 energy with all audio source slots silent; the
  saved send automation ratio is 1.33289 against the 4/3 route oracle. These
  probes remove the other role where necessary to isolate the measured source.

The combined project has 33 sends including the instrument's own. The harness
creates the instrument before adding the effect. The application preflight also
accepts the reverse creation order on separate tracks; its focused service
contract checks that path and retains rejection of two active effects.

## Local uninstrumented measurement

Reference development host: Ubuntu 26.04.1 LTS, Linux 7.0.0-38-generic,
Intel Core i7-10700K (16 logical CPUs), SSD, GCC 15.2.0 C++20 Release with
warnings as errors. The project and generated media were temporary. CPU
frequency, thermal state, page cache and background system activity were not
fixed. The main build had finished before these runs. Each repetition used
100 warmup blocks and 2,000 measured blocks, paced at the nominal period;
three repetitions at each 64/128-frame size and each VST3 scenario. At 48 kHz,
the measured part spans 2.667 seconds at 64 frames and 5.333 seconds at 128
frames per repetition. Callback timing uses `steady_clock` around the render
call, excluding the sleep and setup.

| VST3 role | Frames | Means, µs/block (runs 1–3) | p99, µs/block (runs 1–3) | Worst µs | Over-period blocks |
| --- | ---: | --- | --- | ---: | ---: |
| Effect | 64 | 69.900, 98.047, 73.001 | 119.313, 400.378, 142.423 | 540.814 | 0/6,000 |
| Instrument | 64 | 114.744, 110.210, 116.739 | 390.000, 388.283, 415.417 | 639.497 | 0/6,000 |
| Effect | 128 | 199.410, 168.897, 250.620 | 769.684, 732.344, 852.435 | 1031.714 | 0/6,000 |
| Instrument | 128 | 206.035, 188.591, 180.570 | 799.477, 784.379, 749.338 | 1019.995 | 0/6,000 |
| Effect + instrument | 64 | 118.095, 113.717, 120.625 | 182.404, 176.173, 180.279 | 236.297 | 0/6,000 |
| Effect + instrument | 128 | 218.219, 220.551, 232.116 | 299.802, 282.609, 331.681 | 487.776 | 0/6,000 |

Across all 36,000 measured blocks: zero counted scalar callback C++ `new`
calls, source-missing frames, worker failures, render failures, plugin faults
and nonfinite output. All repetitions produced audible blocks and activated
the saved graph revision. The allocation probe does not count aligned `new`,
C allocation or worker-thread allocation. The callback's over-period count is
processing time only; it is **not** a backend xrun count. Variation in the
effect runs makes a cross-scenario CPU-cost claim unjustified.

## Local correctness and backend evidence

Clang 21.1.8 Debug, warnings as errors, built the scanner-enabled target.
Short 100-block/20-warmup checks passed for effect at 64 frames and instrument
at 128 frames with the same audio oracles, zero callback allocations, missing
frames, render/worker failures and plugin faults. Debug times are not
compared with Release measurements.

The combined scenario passed the same short Clang Debug checks at both sizes
with 8× callback pacing, zero missing frames and the same independent oracles.
An initial unpaced 64-frame Debug run had 60,416 missing source frames while
its callback exceeded the nominal period; this is a source-prefetch underrun
under an unrepresentative Debug workload, not Release performance evidence.

GCC 15 Debug ASan/UBSan, warnings as errors, passed the same two 100-block
checks with `--pace-multiplier 8`, leak detection enabled and no sanitizer
diagnostic. Its measured callback times exceeded the nominal period because
of instrumentation and are correctness evidence only. Focused clang-tidy and
clang-format checks passed; no production source changed in this slice, so
the integrated production coverage percentage is unchanged by this branch.

The combined scenario also passed short GCC 15 ASan/UBSan checks at 64 and
128 frames with 8× pacing and leak detection enabled: the same three audio
oracles passed, and callback allocations, missing frames, render/worker
failures and plugin faults were zero. Instrumented callbacks exceeded the
nominal period, as expected for this correctness run; no sanitizer diagnostic
appeared.

The first combined ASan build exhausted local `/tmp` tmpfs during compiler
assembly output. The retry used a disk-backed build and compiler temporary
directory; the first failure produced no code diagnostic.

The fake callback has no backend xrun source. An optional PipeWire runner
targets a named disposable null sink by object serial, checks negotiated
48 kHz and requested 64/128-frame blocks, captures an audible monitor, and
reports `xruns=unavailable` if the adapter supplies no per-stream counter.
That observability gap prevents a zero-xrun assertion. A virtual sink cannot
qualify physical-device behaviour.

The instrument scenario passed disposable-sink runs at both sizes: requested
and observed 64/128 frames at 48 kHz, 134,784/134,400 and 269,184/268,800
completed/target frames, zero render failures, missing frames, worker failure,
oversized/invalid buffers and plugin faults. Monitor captures held 162,624 and
303,232 frames with peak 0.8913 and 140/280 audible 20 ms windows. A separate
64-frame effect run passed with 134,848/134,400 completed/target frames, zero
fault counters and an audible monitor (159,552 frames, 141 active windows).
Each run reported backend xruns as **unavailable** and removed its sink.

The combined effect/instrument scenario also passed disposable-sink runs at
both sizes. At 64 frames, the adapter observed 64 frames and 48 kHz, completed
134,784/134,400 target frames, measured 2,140 callbacks with zero over the
nominal processing period and a 202,024 ns maximum; the audible monitor held
177,984 frames, peak 0.8913 and 141 active 20 ms windows. At 128 frames it
observed 128 frames and 48 kHz, completed 268,928/268,800 target frames,
measured 2,133 callbacks with zero over period and a 445,156 ns maximum; the
monitor held 313,088 frames, peak 0.8913 and 280 active windows. Both runs
reported zero render failures, missing frames, worker failures,
oversized/invalid buffers and plugin faults. Both again reported
`xruns=unavailable` and cleaned up their named null sink. This is virtual-sink
observability, not a physical device or zero-backend-xrun result.

## Reproduce

Configure the repository's local PipeWire headers, nlohmann JSON package and
pinned VST3 SDK as described by the existing qualification setup. The target
is excluded from ordinary CTest.

```sh
cmake -S tests/qualification -B build/g7-rehearsal -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DOSCILLATE_WARNINGS_AS_ERRORS=ON \
  -DOSCILLATE_BUILD_VST3_SCANNER=ON \
  -DOSCILLATE_VST3_SDK_ROOT=/path/to/vst3sdk
cmake --build build/g7-rehearsal --target oscillate_streaming_32track_rehearsal
for role in --vst3-insert --vst3-instrument; do
  for frames in 64 128; do
    build/g7-rehearsal/oscillate_streaming_32track_rehearsal \
      --frames "$frames" "$role" 1 --repetitions 3 --blocks 2000 --warmup 100
  done
done
build/g7-rehearsal/oscillate_streaming_32track_rehearsal \
  --frames 64 --vst3-insert 1 --vst3-instrument 1 \
  --repetitions 3 --blocks 2000 --warmup 100
build/g7-rehearsal/oscillate_streaming_32track_rehearsal \
  --frames 128 --vst3-insert 1 --vst3-instrument 1 \
  --repetitions 3 --blocks 2000 --warmup 100
python3 tests/qualification/qualify_live_streaming_32track.py \
  --executable build/g7-rehearsal/oscillate_streaming_32track_rehearsal \
  --vst3-insert --vst3-instrument
```

Run the Python target with either single-role flag for the separate scenarios.

## Release boundary

This is a local development rehearsal with short, easily cached sources and
one or two trivial synthetic plugins per run. It lacks representative
third-party VST3 instances and UI work,
physical PipeWire/JACK device scheduling, measured backend xruns, a controlled
background-load matrix and the agreed reference workload duration. Master
effect tails, PDC and offline stems have separate focused tests but are not
qualified by this timed harness. The PRD G7 performance, audio-correctness
and physical-device release gates remain open.
