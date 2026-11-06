# Opt-in VST3 instrument release-tail probe

`oscillate_vst3_instrument_tail_probe` loads one scanned VST3 instrument binary
at 48 kHz in real-time processing mode. It captures the default component and
independent controller state, or restores raw bytes from `--state-file` and
optional `--controller-state-file` (maximum 4 MiB each). The JSON record
includes the exact binary SHA-256 and size, class/vendor/name/version, full
component and optional controller state bytes as hex, declared latency and
tail, and `production_admitted: false`. A null controller field means absent;
an empty string means a present zero-byte chunk.

For each 64- and 128-frame callback size, two fresh instrument instances play
MIDI note 60 at velocity 0.8. A short note ends at sample 257, inside a block;
a long note ends at sample 4,096, on a block boundary. The probe feeds 2,048
guard frames beyond the declared release plus latency. It checks finite
output, audible response, peak past the declared drain (limit `1e-6` linear),
exact repeatability after fresh state restore, stable declared tail and
latency, plugin restart requests, process faults and C++ `new` calls inside
callbacks. Infinite or unavailable tails, and latency or tail above two
seconds, stop the probe. `--parameter ID` additionally probes values 0, 0.5
and 1 at the start of the long note for one writable automatable parameter.

Build and run from the canonical configured build tree:

```sh
mkdir -p build/quick/scratch
TMPDIR="$PWD/build/quick/scratch" cmake --build build/quick --target oscillate_vst3_instrument_tail_probe
TMPDIR="$PWD/build/quick/scratch" build/quick/tests/plugins/oscillate_vst3_instrument_tail_probe --self-test
TMPDIR="$PWD/build/quick/scratch" build/quick/tests/plugins/oscillate_vst3_instrument_tail_probe \
  --bundle /path/to/Instrument.vst3 --class 0123456789abcdef0123456789abcdef \
  --state-file /path/to/raw-component-state \
  --controller-state-file /path/to/raw-controller-state \
  --output build/quick/scratch/instrument-tail-evidence.json
```

The self-test uses TimedInstrument with an honest eight-frame note-off
release and both absent and captured controller-state cases, then a deliberately dishonest zero-tail declaration whose audio
continues for eight frames. The first must pass and the second must fail.
Exit code 0 means the screen passed, 2 means it ran and found a violation, and
1 means invalid inputs, scan/load failure or unavailable state.

This is a local compatibility screen, not an allowlist. Raw state files must
come from the exact saved project component and controller states. The record's
`saved_parameters` array is empty because the command does not import project
parameter references; a project with such references needs separate exact
parameter/state evidence before a provider receipt can be issued. The screen
does not cover every pitch, velocity, polyphony pattern, sustain/controller
event, preset, GUI edit, sample rate or transport mode. The allocation counter
does not observe direct `malloc`, custom allocators or other threads. Run
third-party binaries under a separate timeout and resource limit because they
execute in this process. Human review, wider MIDI/state coverage and control
of external editor mutation are required before any production admission.
