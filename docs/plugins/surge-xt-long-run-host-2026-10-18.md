# Surge XT bounded host stress, 18 October 2026

**Scope:** PI-03/PI-04 evidence for the installed Surge XT 1.3.4 VST3
instrument and effects binaries. This is an opt-in, in-process plugin-host
probe. It does not qualify the DAW graph, audio device, editor, or a 32-track
project. The earlier [short host check](surge-xt-compatibility-2026-10-18.md)
records the one-block setup and class metadata.

The probe ran on Ubuntu 26.04.1, Linux 7.0.0-38-generic x86-64, an Intel
Core i7-10700K (8 cores/16 threads), and GCC 15.2.0. The local CMake Release
build was uninstrumented. It prepared each plugin for a maximum of 128 frames
and sent actual 64- or 128-frame stereo blocks at 48 kHz. Each of the four
class/block combinations ran in a separate process with a 120-second timeout.

| Class | Class ID | Installed binary SHA-256 |
| --- | --- | --- |
| Instrument | `abcdef019182faeb566d624153675854` | `b8584398f314819241ca7287117d5559ed838d37ef08faab9847f27ba2993cec` |
| Effects | `abcdef019182faeb566d624153465854` | `9b2da5f1b4a81a02ad55c88eba55257eb4a7f1747a1fd0866b8eaf5b7c8d0f45` |

For each run, a scanner verified the bundle out of process. The host opened
the class, processed a starting block and delivered one continuous parameter
event at half a block. The instrument received two C4 note-ons from those
setup calls and then held them; Effects received constant stereo input of
`+0.25/-0.25`. The sustained phase processed 16,000 consecutive blocks:
21.33 seconds of represented audio at 64 frames or 42.67 seconds at 128.
There was no sleep or audio-device pacing. The host then saved opaque
component state, created a fresh instance from it, processed 1,024 more
blocks, saved/reopened a project with `ProjectStore`, rebound another fresh
instance, and processed 1,024 more blocks. The project reopen comparison
checked plugin identity and opaque state exactly. Both controllers returned
`not_implemented` for optional controller-state synchronisation.

Every process call returned `ok`; every output sample was finite. All phases
produced nonzero output, both state transitions passed, and no measured
process-call wall interval exceeded the nominal device period. The table
reports the 16,000-block phase. Timing surrounds each host process call using
`CLOCK_THREAD_CPUTIME_ID` and a steady wall clock; it excludes finite-output
scanning, state saves, project I/O and scanner work. The CPU clock is read just
outside the wall-clock interval, so a CPU maximum can be slightly greater
than the corresponding wall maximum.

| Class | Frames | Thread CPU p50 / p99 / max (µs) | Wall max (µs) | Nominal period (µs) | Over period |
| --- | ---: | ---: | ---: | ---: | ---: |
| Instrument | 64 | 10.24 / 14.55 / 789.50 | 789.08 | 1,333.33 | 0/16,000 |
| Instrument | 128 | 19.59 / 47.80 / 971.51 | 970.93 | 2,666.67 | 0/16,000 |
| Effects | 64 | 2.25 / 2.46 / 33.95 | 33.69 | 1,333.33 | 0/16,000 |
| Effects | 128 | 3.88 / 4.21 / 8.80 | 24.43 | 2,666.67 | 0/16,000 |

The maximum instrument calls were far above the p99 despite staying within
the period in this fast-forward run. This is a single local run per
combination, under an uncontrolled desktop load. The probe did not measure
plugin allocations, a callback under device scheduling pressure, backend
xruns, other presets, polyphony beyond the two setup notes, parameter
interpolation precision, an interactive factory-preset change, or crash
isolation. It therefore supports a narrow **tested host** status for these
binary hashes and conditions; general Surge/third-party compatibility and
whole-app real-time qualification remain open.

## Reproduce

Configure a local Release build with the pinned VST3 SDK and dependencies,
then run the focused fixture checks and one opt-in real-plugin invocation per
class and block size. Keep `TMPDIR` inside that build tree. Example:

```sh
mkdir -p build/release/scratch
TMPDIR="$PWD/build/release/scratch" cmake --build build/release \
  --target oscillate_vst3_compatibility_probe
TMPDIR="$PWD/build/release/scratch" \
  build/release/tests/plugins/oscillate_vst3_compatibility_probe --self-test
TMPDIR="$PWD/build/release/scratch" \
  build/release/tests/plugins/oscillate_vst3_compatibility_probe --self-test-stress
TMPDIR="$PWD/build/release/scratch" timeout 120s \
  build/release/tests/plugins/oscillate_vst3_compatibility_probe --stress \
  '/usr/lib/vst3/Surge XT.vst3' abcdef019182faeb566d624153675854 \
  instrument "$PWD/build/release/scratch" 64 16000
```

Repeat the last command with `128`, then with the Effects bundle and class
ID above at both frame sizes. The command returns JSON with all three phase
measurements; this run's raw JSON files stayed in the ignored local build
directory. This optional stress command is neither a default build target
dependency nor a CTest case.
