# Surge XT 1.3.4 VST3 host compatibility check

**Date:** 18 October 2026. **Host source:** `b93e356` plus the opt-in probe's
64/128-frame selection in this branch. **System:** Ubuntu 26.04.1 LTS,
Linux 7.0.0-38-generic x86-64, GCC 15.2.0. The desktop session was Wayland,
but this check did not create or attach a plugin editor.

This is a PI-03/PI-04 qualification observation for two installed binaries,
not product-wide VST3 certification or an admission of arbitrary live edits.

| Class | VST3 class ID | Installed binary SHA-256 |
| --- | --- | --- |
| Surge XT instrument | `abcdef019182faeb566d624153675854` | `b8584398f314819241ca7287117d5559ed838d37ef08faab9847f27ba2993cec` |
| Surge XT Effects | `abcdef019182faeb566d624153465854` | `9b2da5f1b4a81a02ad55c88eba55257eb4a7f1747a1fd0866b8eaf5b7c8d0f45` |

The installed `surge-xt` package reported version 1.3.4. Each class and block
size ran in a fresh probe process with a 30-second timeout. The scanner ran
out of process before in-process host loading. The host prepared a 128-frame
maximum at 48 kHz and passed **actual** 64- or 128-frame blocks to the plugin.
The instrument received a velocity-0.5 note; Effects received constant stereo
input of +0.25/-0.25. The probe delivered the first writable continuous
parameter it found at the midpoint of one block, then saved opaque component
state, rebound a fresh instance, saved a project through `ProjectStore`,
reopened it, and rebound another fresh instance.

| Class | Actual frames | Event ID / offset | First / restored / reopened peak | Result |
| --- | ---: | --- | --- | --- |
| Instrument | 64 | `2465059` / 32 | 0 / 0.00475 / 0.00403 | Host checks passed |
| Instrument | 128 | `2465059` / 64 | 0.01172 / 0.00791 / 0.01693 | Host checks passed |
| Effects | 64 | `720135338` / 32 | 0.21875 / 0.21875 / 0.21875 | Host checks passed |
| Effects | 128 | `720135338` / 64 | 0.21875 / 0.21875 / 0.21875 | Host checks passed |

All four scans, loads, process calls, opaque-state restores and exact plugin
identity/state project reopen comparisons passed. The processed samples were
finite. The 64-frame instrument's first block was silent; later restored and
reopened blocks had signal. That is consistent with a note onset and is not
an audio failure. The host reports one silent auxiliary input and two
discarded secondary outputs for the instrument, and one silent auxiliary
input for Effects. These buses are not routed by this check.

Both controllers reported `not_implemented` for optional controller-state
synchronisation. The X11 editor capability probe returned
`x11_candidate_only`, which does not test embedding or mouse input. The
reported `parameter_event: forwarded` establishes host queue delivery only;
it does not verify the requested value, audible response or precise
sample-offset application. The probe did not measure CPU, callback
allocations, factory-preset changes, latency changes, tails, project playback,
offline/live equivalence, Wayland native GUI, or long-run stability. Overall
plugin compatibility therefore remains **unverified**; the narrower automated
host check is **tested** for these hashes and conditions. Existing
[interactive X11 evidence](vst3-local-compatibility-probe.md) and
[Filter 1 Cutoff live evidence](surge-xt-live-latency-qualification.md) have
their own separate scopes.

## Reproduce locally

Build the opt-in target in the selected worktree build directory and run its
synthetic self-test first:

```sh
mkdir -p build/dev/scratch
TMPDIR="$PWD/build/dev/scratch" cmake --build build/dev --target oscillate_vst3_compatibility_probe
TMPDIR="$PWD/build/dev/scratch" build/dev/tests/plugins/oscillate_vst3_compatibility_probe --self-test
```

For each class, run `--list BUNDLE` to verify the class ID and hash, then
invoke the probe separately at both block sizes. Example instrument run:

```sh
PROBE=build/dev/tests/plugins/oscillate_vst3_compatibility_probe
TMPDIR="$PWD/build/dev/scratch" timeout 30s "$PROBE" \
  '/usr/lib/vst3/Surge XT.vst3' abcdef019182faeb566d624153675854 \
  instrument "$PWD/build/dev/scratch" 64 \
  > build/dev/scratch/surge-instrument-64.json
```

The four raw JSON outputs and scan listings were retained only in this
worktree's ignored `build/dev/scratch/`; they are not repository assets.

## Instrument release-tail screen (18 October 2026)

The opt-in `oscillate_vst3_instrument_tail_probe` ran against the same installed
instrument hash at 48 kHz with short and long notes and both 64- and 128-frame
blocks. Surge declared a 96,000-frame tail and produced finite output that
decayed below the probe threshold after that bound. The screen did **not** pass:
each render made 3,878 C++ allocations during processing, and repeated renders
were not bit-identical. The JSON evidence is in the ignored
`build/quick/scratch/surge-instrument-tail.json`. Exit status was 2. This
binary remains unadmitted for qualified real-time export; the synthetic
fixture's successful tail contract does not change Surge's status.
