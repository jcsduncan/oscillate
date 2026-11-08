# Reference projects

Two generated projects measure what a realistic session costs. Both are built
from scratch by `oscillate_reference_project`
([source](../../tests/qualification/reference_project.cpp)), saved, reopened
and played at 48 kHz. Nothing is read from a user's projects.

| | Light | Heavy |
| --- | --- | --- |
| Source tracks | 32 audio | 16 audio, 15 built-in synths, 1 VST3 instrument |
| Synths | none | Nova ×6, Prism ×2, Ember, Drift, Halo, Pulse, Torque, Anvil, Knock |
| Track effects | none | EQ and compressor per audio track; saturator and delay per synth; one VST3 effect |
| MIDI | none | Bar-long four-note chords or an eighth-note line, at 120 BPM |
| Routing | 2 groups, 2 returns, one send per track | the same |
| Master | EQ, compressor, reverb, limiter | the same |

The audio tracks play generated WAVs (two partials and a click per beat). The
VST3 parts default to the repository's synthetic fixtures; on the reference
machine the heavy project uses Surge XT and Surge XT Effects instead. A build
without the VST3 host plays a second Ember in place of the VST3 instrument.

## What a run reports

[`qualify_reference_projects.py`](../../tests/qualification/qualify_reference_projects.py)
plays each project at 64 and 128 frames after 100 warm-up blocks and writes
`results.json` and `results.md`:

- **Callback time:** p50, p99 and maximum of each block's render, and the
  blocks over their period. Through the paced fake callback the player times
  each block itself; on a PipeWire sink the times come from the trace's
  `audio.render` spans.
- **Whole-process CPU:** the process's CPU time over the measured run, as a
  percentage of one core, from every thread (callback, audio workers, source
  readers). The run is also cut into 100 ms windows, and their p50, p99 and
  maximum are reported.
- **Peak memory:** the process's maximum resident set.
- **Devices** (with `--trace`): from the [performance trace](../decisions/0061-performance-tracing.md),
  each synth and effect kind's share of one core, instance count and
  per-block mean, p99 and maximum, on the callback and worker threads.
- **Correctness:** a run fails on any render failure, missing source frame,
  worker failure, non-finite sample or silent output.

On a sink, the run also reports PipeWire's xrun count where the backend
exposes it, and the trace's xrun instants.

## In CI

The [reference performance workflow](../../.github/workflows/performance.yml)
runs both projects through the fake callback weekly on `main`, on demand
(`gh workflow run performance.yml -f seconds=60`) and on pull requests that
change the harness. Its summary is `results.md`; the `reference-performance`
artifact holds both files and the logs. Hosted runners are shared four-core
machines, so their numbers show trends between runs, not release headroom.

## On the reference machine

The reference machine is the maintainer's desktop: Intel Core i7-10700K
(8 cores, 16 threads), 30 GiB of memory, Ubuntu 26.04 with PipeWire. Build the player in
the normal release build directory:

```sh
cmake -DOSCILLATE_BUILD_QUALIFICATION=ON build/release
scripts/build.sh build/release --target oscillate_reference_project
```

Fake callback, both projects, three runs of a minute each, with Surge XT in
the heavy project:

```sh
python3 -I tests/qualification/qualify_reference_projects.py \
  --executable build/release/tests/qualification/oscillate_reference_project \
  --output build/release/scratch/reference-fake \
  --seconds 60 --repetitions 3 --trace \
  --vst3-instrument "/usr/lib/vst3/Surge XT.vst3" \
  --vst3-effect "/usr/lib/vst3/Surge XT Effects.vst3"
```

The same on a physical output. Name the sink explicitly (the runner never
picks the default device) and set a safe listening level first; list the
sinks with:

```sh
pw-dump | python3 -I -c 'import json, sys; [print(n["info"]["props"]["node.name"]) for n in json.load(sys.stdin) if n.get("type") == "PipeWire:Interface:Node" and n["info"]["props"].get("media.class") == "Audio/Sink"]'
python3 -I tests/qualification/qualify_reference_projects.py \
  --executable build/release/tests/qualification/oscillate_reference_project \
  --output build/release/scratch/reference-device \
  --seconds 60 --repetitions 3 \
  --target-node 'alsa_output.SINK-NAME' \
  --vst3-instrument "/usr/lib/vst3/Surge XT.vst3" \
  --vst3-effect "/usr/lib/vst3/Surge XT Effects.vst3"
```

Close other audio and heavy applications before a run, since whole-process
CPU and callback time both include contention from the rest of the machine.
The physical-output gates of the 32-track mixed rehearsal are described in the
[G7 runbook](g7-physical-device-runbook.md).
