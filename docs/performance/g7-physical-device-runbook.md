# G7 physical-output development runbook

**Status, 18 October 2026:** a short opt-in development run played through the
user-selected GO Work USB Analogue Stereo sink at both target buffer sizes.
The G7 release qualification remains open.

The existing [mixed-production rehearsal](g7-mixed-production-rehearsal.md)
builds and reopens 32 managed WAV tracks, two groups, two returns, 33 sends,
saved send automation, EQ, compressor, algorithmic reverb, limiter, one
synthetic VST3 effect and one synthetic VST3 instrument. Its independent
audio probes run before live playback. The physical runner uses the same
Release executable and targets one **explicit** ALSA sink object serial. It
never changes the system default output.

## Local host and observed readiness

Ubuntu 26.04.1, kernel 7.0.0-38-generic, Intel i7-10700K (8 cores, 16 logical
CPUs), 30 GiB RAM and SSD. PipeWire 1.6.2 currently exposes three physical
ALSA sink names: `alsa_output.pci-0000_01_00.1.hdmi-stereo`,
`alsa_output.pci-0000_00_1f.3.analog-stereo`, and
`alsa_output.usb-Jieli_Technology_GO_Work_USB_551437959872453F-00.analog-stereo`.
The GO Work USB Analogue Stereo output was selected for this test. Its
PipeWire sink volume was 0.47 immediately before the run. The test did not
change the system default output.

## GO Work USB development run

The local Release target ran three repetitions at each buffer size on the
selected sink at 48 kHz. Each repetition represented about six seconds of
the generated 32-track fixture. The complete machine-readable record is
`build/qualification/g7-go-work/physical-32track.json` in this checkout.

| Frames | Repetitions | Worst callback across runs | Callback blocks over period | Player/sink `pw-top` ERR delta | Missing frames / render or plugin faults |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 3 | 312.665 µs | 0 | 0 | 0 |
| 128 | 3 | 846.012 µs | 0 | 0 | 0 |

Every run negotiated the requested block size and 48 kHz, reached its target
interval and reported the saved graph active. The synthetic VST3 effect,
instrument and send-automation audio probes passed. PipeWire still reported
per-stream xruns as **unavailable**. This is a passed development slice, not a
zero-xrun claim or G7 release sign-off. Background load was uncontrolled, the
VST3s were synthetic, and the fixture used short generated media. The callback
maximum at 128 frames was below its 2,666.67 µs nominal period, but that
timing does not measure scheduling or hardware delivery.

The five focused parser/readiness/gate tests pass. The fresh worktree Release
target passed one 2,000-block fake-callback repetition at each size:

| Frames | Mean µs | p99 µs | Worst µs | Over period | Callback `new` | Missing frames |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 78.410 | 116.559 | 252.142 | 0/2,000 | 0 | 0 |
| 128 | 203.856 | 342.059 | 418.812 | 0/2,000 | 0 | 0 |

The independent effect, instrument and send-automation audio probes passed
in both. This was an uncontrolled development check; the 128-frame run
overlapped other local virtual playback and is not comparable CPU evidence.

The physical runner's core node-match, `pw-top` sample and result gates were
also exercised against a disposable virtual sink at both sizes, with the
physical-target restriction replaced **only in the test process**. Those
6-second checks completed 294,720/294,400 frames at 64 and 294,784/294,400
frames at 128, with zero callback bodies over period and zero sampled ERR
deltas for the linked playback node and virtual sink. PipeWire stream xruns
were **unavailable**. These checks prove the runner can follow a real
PipeWire link and parse live telemetry; they are not physical-output evidence
or a new G7 pass.

## Reproduce on a selected physical output

Build the standalone qualification target in a worktree-local Release build,
using the development dependency paths from the [qualification README](../../tests/qualification/README.md).
Set `TMPDIR` to a directory inside that build before configuring, building or
running. List possible sinks with:

```sh
pw-dump | jq -r '.[] | select(.type=="PipeWire:Interface:Node" and .info.props["media.class"]=="Audio/Sink" and .info.props["device.api"]=="alsa") | .info.props["node.name"]'
```

Choose the sink connected to the intended listening device and set a safe
listening level. Then run, for example:

```sh
mkdir -p build/qualification/scratch build/qualification/g7-device
TMPDIR="$PWD/build/qualification/scratch" \
python3 tests/qualification/qualify_physical_streaming_32track.py \
  --target-node 'alsa_output.pci-0000_00_1f.3.analog-stereo' \
  --executable build/qualification/oscillate_streaming_32track_rehearsal \
  --artifact-dir build/qualification/g7-device
python3 tests/qualification/test_physical_streaming_32track.py -v
```

The named sink is illustrative, not a recommendation. The runner writes
`physical-32track.json` after every repetition, including a failure reason
if a repetition fails. Generated 32-track projects stay in the specified
build-tree scratch directory and are removed by the executable. Runs use 100
warmup blocks and 4,500 measured 64-frame or 2,200 measured 128-frame blocks,
roughly six seconds of audio per repetition, three repetitions by default.
The short default generates eight-second source media.

For an explicitly requested longer development run, pass
`--measured-seconds 60` (up to 90). This changes neither the default nor any
ordinary CTest run. The wrapper computes whole callback blocks for at least
the requested measured interval plus 100 warmup blocks, then asks the fixture
for just enough whole seconds of source media. The fixture caps generated
media at 120 seconds per track and writes 8 KiB chunks instead of buffering
whole tracks. A 60-second request creates 61-second source WAVs: roughly
179 MiB of originals across 32 tracks, with managed import copies adding
roughly the same amount in build-tree scratch space. The synthetic MIDI
instrument also receives one saved clip per two-second bar throughout the
long source rather than falling silent after its original four clips.

```sh
TMPDIR="$PWD/build/qualification/scratch" \
python3 tests/qualification/qualify_physical_streaming_32track.py \
  --target-node 'YOUR_EXACT_PHYSICAL_ALSA_SINK_NAME' \
  --executable build/qualification/oscillate_streaming_32track_rehearsal \
  --artifact-dir build/qualification/g7-long-device \
  --measured-seconds 60
```

This command deliberately plays the selected physical output; run it only
after choosing the sink and listening level. The longer option is a fixture
capability, not physical-device or release qualification evidence. The same
strict callback, source, graph, plugin and sampled `pw-top` gates apply.

An isolated Release **fake-callback** check of the 60-second option at 64
frames passed on 18 October 2026. It used 61-second sources, 100 warmup and
45,000 measured blocks. The independent VST3 effect, instrument and send
automation probes passed; 45,097 blocks were audible. Mean callback work was
93.823 µs, p99 was 387.197 µs and maximum was 583.511 µs. No measured body
exceeded its 1,333.333 µs period; callback allocations, missing frames,
render/worker failures and plugin faults were zero. Short 100-block
fake-callback runs also passed at 64 and 128 frames. This was uncontrolled
local CPU evidence with no PipeWire stream, device delivery or xrun counter.

## Development slice gates and interpretation

Each repetition must negotiate 48 kHz and exactly the requested 64 or 128
frames; finish the target interval; report the saved graph active; and have
zero missing source frames, render/worker/plugin failures, invalid or
oversized buffers and callback bodies over the nominal period. It must find
one linked player node owned by this run and two complete `pw-top` samples
for that node and the selected ALSA sink. Neither `pw-top` ERR counter may
decrease or increase between samples, and the sink ID and serial must remain
stable. An exposed stream-xrun counter, if later available, must be zero;
today the PipeWire adapter reports it as unknown.

These thresholds are **provisional development gates**. `pw-top` ERR combines
node/driver errors and xruns; a zero sampled delta is not proof of zero
device xruns. The two samples cover a window within the run, not the entire
project or every driver event. Callback wall time excludes scheduling before
entry and hardware delivery after exit. The fixture uses synthetic VST3
plugins, generated audio and uncontrolled background load. The runner
does not capture physical acoustic output or evaluate listening quality.

G7 release sign-off still needs an agreed reference device/plugin set,
controlled background load, longer representative media and run duration,
repeat-count and stress thresholds, device-specific xrun evidence, real
third-party VST3 compatibility and explicit review of the results at both
buffer sizes. A failed run remains failed evidence; it must not be promoted
to a pass by substituting callback timing for backend observations.
