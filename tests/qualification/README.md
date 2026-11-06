# Opt-in live qualification

The [physical JACK real-time export check](../../docs/export/jack-physical-qualification.md)
builds a separate, opt-in executable that calls the production Quick export
path on explicitly named JACK input ports. Its Python runner records 64/128
callback, xrun and WAV evidence. Building and running its unit tests do not
play audio.

## Long FLAC/MP3 source seeking

Configure this standalone qualification project with
`OSCILLATE_BUILD_CODEC_ADAPTER=ON` and `OSCILLATE_BUILD_SOURCE_RESAMPLER=ON`,
then build `oscillate_compressed_seek_rehearsal` in Release mode. Use the same
local dependency paths as the codec-enabled app. Run:

```sh
python3 tests/qualification/qualify_compressed_seek.py
```

The runner needs FFmpeg with FLAC and libmp3lame encoders. It generates two
ten-minute files in a temporary directory, compares random reads with a
sequential decode, checks cancellation and bounded requests, and reports seek
latency. No media is stored in the repository. The
[measurement note](../../docs/performance/source-rate-worker.md#long-compressed-source-seek-qualification)
records the test machine, observed tails and release limits. This is an
explicit performance rehearsal, excluded from ordinary CTest.

The [32-track routed rehearsal](../../docs/performance/routed-32track-rehearsal.md)
is an opt-in engine-only performance fixture in this CMake project. It measures
mixed synthetic sources through group buses, return sends and the native master
chain at 64 and 128 frames. It does not use a live audio device and is not an
ordinary CTest or release-performance gate.

The [managed-WAV 32-track rehearsal](../../docs/performance/streaming-32track-rehearsal.md)
also has an opt-in live PipeWire mode. The runner creates a named disposable
null sink, targets its object serial explicitly, captures its monitor, and
removes the sink after the bounded run. It requests and checks both 64- and
128-frame callback sizes and reports source, output and captured-audio evidence:

```sh
cmake -S tests/qualification -B build/streaming-32 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/streaming-32 --target oscillate_streaming_32track_rehearsal
python3 tests/qualification/qualify_live_streaming_32track.py
```

The executable can be used with `--pipewire-target SERIAL`, but it never
selects the default device in that mode. The Python runner obtains the serial
from its own disposable sink. The backend does not expose a trustworthy
per-stream xrun count, so this rehearsal cannot establish zero xruns. It
reports measured process-callback blocks, blocks whose callback wall time
exceeded their nominal audio period, and the longest callback. These are
processing-budget observations, not backend xruns; see the
[PipeWire observability note](../../docs/performance/pipewire-observability.md).

With `OSCILLATE_BUILD_VST3_SCANNER=ON`, the executable and Python runner
accept `--vst3-insert 1` and `--vst3-insert`, respectively. This adds one
scanned synthetic VST3 gain effect to the first audio track, verifies its saved
identity and scan receipt after reopen, and compares rendered output with and
without the insert. The [mixed-insert measurement](../../docs/performance/streaming-32track-vst3-rehearsal.md)
records the paired baseline and limitations. The current routed host supports
one bounded VST3 audio effect and one separate instrument source in this path;
this is not a representative third-party plugin workload or physical-device
xrun qualification.

The same opt-in target also accepts `--vst3-instrument 1` (Python:
`--vst3-instrument`). It scans the synthetic `TimedInstrument` fixture, saves
four MIDI clips and routes that instrument alongside the 32 managed audio
tracks, groups, returns, sends and saved send automation. A silent-audio-input
probe confirms the instrument actually produces routed sound. Effect and
instrument flags may be used separately or together. The combined run checks
both saved identities, independent audio oracles, and the common live graph.
See the [G7 mixed-production rehearsal](../../docs/performance/g7-mixed-production-rehearsal.md)
for exact commands, results and the remaining release gate.
The [repeatable virtual-device runner](../../docs/performance/g7-mixed-virtual-repeat.md)
uses that same combined fixture three times at both buffer sizes and writes
machine-readable local evidence. It still cannot establish physical-device or
backend-xrun qualification.

For a local CPU/headroom probe that also runs both integrated native synths,
build the opt-in Release target and run:

```sh
python3 tests/qualification/qualify_mixed_cpu.py \
  --executable build/native-synth-qualification/oscillate_streaming_32track_rehearsal \
  --output build/native-synth-qualification/native-synth-mixed.json \
  --native-synths
```

This replaces two of the 32 WAV sources with Prism and Ember MIDI tracks, routes
each through a group and return send, and checks their saved identities and
active voices after reopen. It measures the same paced fake callback at 48 kHz
and 64/128 frames, reports per-block percentiles and `new` allocations on the
callback thread, and requires finite audible mixed output. The native synth
voice count is a coarse participation check; the separate routed synth and
export-parity tests provide sample-level audio oracles. This variant keeps the
synthetic VST3 gain effect but omits the synthetic VST3 instrument: the current
provider refuses another instrument source in that path. Worker-thread
allocations, other allocator APIs, device xruns, recorded-material quality and
reference-hardware release thresholds remain outside this probe.

The [physical-output runbook](../../docs/performance/g7-physical-device-runbook.md)
adds an opt-in runner for one explicitly selected ALSA sink, paired `pw-top`
node/driver error samples, local thresholds and a JSON evidence file. It never
selects a physical output automatically; the release G7 gate remains open.

## Desktop playback controller

```sh
python3 tests/qualification/qualify_desktop_playback.py
```

Set `PKG_CONFIG_PATH` and `CMAKE_PREFIX_PATH` if local development dependencies require them. The opt-in fixture imports the bundled starter WAV as an audio clip and targets the desktop application's `InstrumentPlayback` controller at a disposable PipeWire sink. It primes the persistent prefetch worker, records the live output, verifies zero source underruns and render failures, and checks safe worker retirement and stopped-export gate release after the callback is joined. The harness checks finite audible output and cleans up the sink and recorder. It tests the application controller used by the Qt Play/Stop controls; ordinary offscreen UI tests separately cover stale worker preparation and save-failure visibility. It does not exercise a live Qt click path, physical-device loss, or the 32-track performance gate.

## Live routed edit handoff

Build the separate opt-in fixture and player, then run the disposable PipeWire sink check:

```sh
cmake -S tests/qualification -B build/routed-live-qualification -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/routed-live-qualification --target oscillate_routed_live_fixture oscillate_routed_player
python3 tests/qualification/qualify_live_routed_edit.py
```

Use the same local `PKG_CONFIG_PATH` and `CMAKE_PREFIX_PATH` needed by the normal build. The fixture creates a managed WAV project with a group route and return send. The player reports the model and active graph revisions, changes the send gain while PipeWire plays, deliberately submits an unsupported master EQ edit, retries, then undoes it. The harness checks the callback hold at the completed transport sample and generation, finite monitor audio before and after the gain change, visible failure status while the old graph remains audible, recovery after undo, zero render failures and source missing frames, and saved project state after reopen. This is a bounded-ring, worker-streamed audio-only live check on a disposable sink; it does not qualify the Qt Quick playback path, plugin state, physical-device recovery, or the reference workload. The [streaming note](../../docs/performance/routed-streaming.md) records the worker and seek-generation fixtures, a longer-source test, benchmark limits and two local captures.

## Live encoded routed playback

With `OSCILLATE_BUILD_CODEC_ADAPTER=ON` in the separate qualification build, run:

```sh
python3 tests/qualification/qualify_live_routed_encoded.py
python3 tests/qualification/qualify_live_routed_encoded.py --format aiff
python3 tests/qualification/qualify_live_routed_encoded.py --format mp3
```

Each run creates a five-second 48 kHz managed source in the selected format, plays it through the same bounded worker and disposable PipeWire sink, captures audible monitor audio and checks the reopened route. Local AIFF and MP3 runs reported zero render failures, missing source frames and worker failures, with 201 and 200 audible 20 ms windows respectively. Deterministic fake-device tests also cover seek generation and damaged asset rejection. Long-media compressed seek latency, sample-rate conversion, plugin workloads and physical-device loss remain unqualified.

## Qt Quick live PipeWire playback

Build the Quick app and `oscillate_routed_live_fixture` with the local development dependencies, then run:

```sh
python3 tests/qualification/qualify_quick_pipewire.py \
  --quick build/quick/apps/daw_quick/oscillate \
  --fixture build/routed-live-qualification/oscillate_routed_live_fixture

# Run just one requested buffer size instead of the default 64 and 128 sweep:
python3 tests/qualification/qualify_quick_pipewire.py --buffer-frames 64

# Save a master limiter through Quick and verify live limiting at both requests:
python3 tests/qualification/qualify_quick_pipewire.py --limiter

# With OSCILLATE_BUILD_SOURCE_RESAMPLER=ON in both builds:
python3 tests/qualification/qualify_quick_pipewire.py --source-rate 44100
```

This opt-in harness creates and reopens a routed project with a six-second managed WAV, removes the original import file, selects a disposable PipeWire null sink through the Quick bridge, and exercises its normal Play/Stop path. By default it requests both 64 and 128 frames in separate runs and prints the observed callback size when available. It checks that the selected sink has active playback links, the UI reports the saved revision as active for at least two seconds, the completed sample advances, no source or output failures are reported, Stop releases the playback gate, and monitor capture contains the expected finite routed signal. The `--qualify-pipewire` launcher hook is only for local qualification and uses the real output factory; it does not alter normal startup or enter ordinary CTest. The harness isolates `QSettings` under a temporary directory and cleans up the sink and recorder. This measures a selected PipeWire target, not a fixed quantum guarantee or the 32-track reference workload. It does not qualify physical device loss, per-stream PipeWire xrun counts, or whole-project production capability.

With `--limiter`, Quick saves a -12 dB sample-peak ceiling on that routed
project before Play. The monitor is checked against the resulting ceiling,
and Stop must report that the live lookahead tail was rendered to the output. A separate
offscreen fake-output test covers limiter playback for an audio-only project
without buses, plus UI keyboard controls, undo, pending/active revisions and
save/reopen. The offscreen output intentionally stops issuing callbacks after
startup, so it checks the bounded incomplete-tail fallback. The stepped fake
output contract separately checks the complete delayed samples and transport
gate lifetime. These runs do not qualify instrument plus limiter playback.

In the local limiter run, revision 18 became active at both requested sizes.
The selected sink reported observed blocks of 64 and 128 frames respectively;
both captures had at least 100 matching 20 ms windows and a 0.2512 sample
peak. Both Stops reported rendered lookahead before the gate was released. No
source or output failure was reported.

In the local two-size run, the 64 request observed 64 frames and the 128 request observed 128 frames. Both monitor captures had 100 matching 20 ms windows and peak 0.3750, with no reported source or output failures.

On the local 15 October 2026 run, GCC, Clang 21 with warnings as errors, and ASan/UBSan builds each reported active saved revision 6, advanced beyond sample 95,000, and stopped with the gate released. Each monitor had 101 matching 20 ms windows and peak 0.3750; no source or output failures were reported. The ASan/UBSan live run used `ASAN_OPTIONS=detect_leaks=0` because the installed PipeWire module-loading path has a separately documented LeakSanitizer report. Ordinary Quick CTest passed 5/5 and focused `clang-tidy` plus repository `clang-format` passed.

An integrated source-converter-enabled run used a 44.1 kHz managed WAV in the
48 kHz project. The Quick path activated revision 6, advanced beyond sample
95,000 and stopped cleanly; 101 steady monitor windows matched the expected
routed level. The hard source edge produced a 0.3983 sample peak from sinc
overshoot, while the same-rate run peaked at 0.3750. Neither run reported a
source or output failure. This does not qualify multitrack converter headroom.
