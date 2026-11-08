# Live tier

The `live` test tier needs running audio servers, so ordinary CTest runs and
the pull-request build leave it out (`ctest -LE live`).
[`scripts/live-tier.sh`](../../scripts/live-tier.sh) runs it against
whatever PipeWire (and JACK) the machine has:

| Check | What it plays to |
| --- | --- |
| `audio_io_pipewire_live` | Silence to the default sink and the first listed sink, at 64 and 128 frames. |
| `audio_io_jack_live*` | An isolated `jackd` dummy server the script starts (needs JACK in the build and `jackd`). |
| 32-track streaming rehearsal | A disposable null sink, monitored by `pw-record`. |
| Reference projects, live | Another disposable null sink: the light and heavy [reference projects](../performance/reference-projects.md) at 64 and 128 frames. |

The script enables `OSCILLATE_AUDIO_IO_LIVE_TESTS`, `OSCILLATE_BUILD_QUALIFICATION`
and the VST3 host in the given build directory, builds only the programs it
runs, and writes `summary.txt`, one log per check and the reference results
into the output directory. It keeps going after a failed check and exits
non-zero if any failed.

## In CI

The [live tier workflow](../../.github/workflows/live-tier.yml) starts a
session bus, PipeWire and WirePlumber in its container and runs the script
on the release preset. Its summary lists each check, and the `live-tier`
artifact holds the logs. A container has no sound card, so this proves the
backends and the harness, not device delivery.

## On the reference machine

From the repository root, with the normal release build. The script turns on
the VST3 host, so the build needs `OSCILLATE_VST3_SDK_ROOT` set to the pinned
SDK ([local development](../dependencies/local-development.md)):

```sh
scripts/live-tier.sh build/release build/release/scratch/live-tier
```

Physical outputs are qualified separately, each against a sink named on the
command line, never the default device:

- the reference projects on a physical sink
  ([commands](../performance/reference-projects.md#on-the-reference-machine));
- the 32-track mixed rehearsal with `pw-top` error sampling
  ([G7 runbook](../performance/g7-physical-device-runbook.md));
- real-time export from physical JACK ports
  ([JACK qualification](../export/jack-physical-qualification.md)).
