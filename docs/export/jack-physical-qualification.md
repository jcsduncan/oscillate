# Opt-in physical JACK real-time WAV check

This is a local development qualification for the production Quick
`RealtimeExportRun` path. It is excluded from ordinary builds and CTest. The
tool has **not** played through a device as part of its implementation review.

The C++ runner creates or opens a saved project, prepares the same private
route graph and JACK capture object used by the DAW, and connects its two
output ports to two **explicitly named** JACK audio input ports. It refuses a
missing server, rate other than 48 kHz, buffer size other than the selected 64
or 128 frames, missing/non-audio destinations, or an additional output
connection. It never changes the server settings or selects the system default.
Its generated fixture contains a quiet managed tone with 0.5 seconds of lead
silence, a Prism MIDI note, and a native master reverb. End bar 1 cuts the
still-playing tone at 2 seconds and captures the prepared release and effect
tail. An existing saved project can be supplied instead; unsupported plugin
or source combinations keep their normal explicit preflight errors.

## Build without playing

From this checkout, configure a worktree-local Release build with the normal
local dependency paths. The project needs Qt Quick, JACK development files,
and the same pinned libraries as the app. For example:

```sh
mkdir -p build/qualification/scratch
TMPDIR="$PWD/build/qualification/scratch" cmake -S tests/qualification \
  -B build/qualification -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DOSCILLATE_BUILD_QUICK_UI=ON
TMPDIR="$PWD/build/qualification/scratch" cmake --build build/qualification \
  --target oscillate_physical_jack_export
python3 tests/qualification/test_physical_jack_export.py -v
```

Configuration may also need local `PKG_CONFIG_PATH`, `PKG_CONFIG_SYSROOT_DIR`,
`Qt6_DIR` and `nlohmann_json_DIR`, as with the main
Quick build. Merely building or running the Python unit tests does not open
JACK or play audio.

## Opt-in playback

List JACK ports and inspect the intended output with `jack_lsp -p`. Set a
comfortable hardware and system volume first. Explicitly name its left and
right input ports in **each** command. The runner does not change JACK's
sample rate or buffer size; set the server to 48 kHz and 64 frames for the
first run, then to 128 frames for the second. The ports' exact names are
system-specific; these examples are placeholders:

```sh
python3 tests/qualification/qualify_physical_jack_export.py \
  --run-physical \
  --executable build/qualification/oscillate_physical_jack_export \
  --artifact-dir build/qualification/jack-device \
  --left-port 'system:playback_1' --right-port 'system:playback_2' \
  --buffer-frames 64 --repetitions 3

# After changing the JACK server to 128 frames:
python3 tests/qualification/qualify_physical_jack_export.py \
  --run-physical \
  --executable build/qualification/oscillate_physical_jack_export \
  --artifact-dir build/qualification/jack-device \
  --left-port 'system:playback_1' --right-port 'system:playback_2' \
  --buffer-frames 128 --repetitions 3
```

`--project /absolute/saved/project` opens a real project instead of creating
the fixture. `--end-bar N` sets the exclusive content end, matching the Quick
Export panel. The qualification bounds it to bars 1–8 and 15 seconds of
project time; the prepared tail has its separate ten-second cap. The artifact
directory must be inside this checkout's `build/` tree. Failed runs retain
their JSON reason and leave an existing target WAV unchanged; a durability
uncertain publication is reported as failure, with the limitation that the
renamed file may already exist.

Each successful repetition records the JACK server format, captured and
expected frames, prepared tail and output latency, callback histogram count,
blocks that exceeded their nominal period, JACK xrun notifications, render
faults, named-port connection checks, and an independently parsed float32 WAV
header, sample peak and SHA-256. The runner rejects an incomplete file,
non-finite samples, an inaudible generated fixture, an xrun, a callback over
period, source-health failure or a changed output connection. It writes
`physical-jack-export-64.json` and `physical-jack-export-128.json` plus the
rendered WAVs in the artifact directory.

This check observes one selected output and the saved fixture or project. It
does not establish the 32-track release workload, automated plugin tail
accuracy, unobserved hardware underruns, listening quality or every possible
JACK session manager's routing behaviour. Port connections are checked after
activation and at capture completion; a transient mid-run reconnection could
escape those two observations. The generated fixture's initial silence allows
the explicit JACK connections to settle before the tone begins.
