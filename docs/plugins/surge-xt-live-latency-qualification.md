# Surge XT live manual edit latency qualification

**Date:** 18 October 2026. **Scope:** PI-09 evidence for Surge XT 1.3.4 only.
This is a local development probe, not a live-edit admission decision.

## Test setup and method

- Host source base: `c4c09fb`; Ubuntu x86-64, Linux `7.0.0-38-generic`, GCC 15.2.0.
- Instrument binary SHA-256: `b8584398f314819241ca7287117d5559ed838d37ef08faab9847f27ba2993cec`.
- Effects binary SHA-256: `9b2da5f1b4a81a02ad55c88eba55257eb4a7f1747a1fd0866b8eaf5b7c8d0f45`.
- The probe scanned and hash-verified each bundle, loaded the selected class at 48 kHz
  with a 128-frame maximum block, delivered selected normalized parameter events
  at offset 64 of a 128-frame block, suspended and saved component state, then
  loaded a fresh instance and restored that state. It recorded the host's
  initial latency and the fresh instance's declared latency after state restore.
  Synth runs supplied one note; effect runs supplied constant stereo input.
- Every case ran in a separate process with a 25-second timeout. The synthetic
  ConnectedGain `--self-test` passed, checking an independently known 7-sample
  latency before and after restoration and the fail-closed qualification field.

Build the opt-in target and rerun, for example:

```sh
mkdir -p build/quick/scratch
TMPDIR="$PWD/build/quick/scratch" cmake --build build/quick --target oscillate_vst3_live_latency_probe
PROBE=build/quick/tests/plugins/oscillate_vst3_live_latency_probe
TMPDIR="$PWD/build/quick/scratch" "$PROBE" --self-test
"$PROBE" --list '/usr/lib/vst3/Surge XT.vst3' abcdef019182faeb566d624153675854 instrument filter
timeout 25s "$PROBE" --probe '/usr/lib/vst3/Surge XT.vst3' abcdef019182faeb566d624153675854 instrument 1945359057:0.95
```

`--list` returns IDs and metadata filtered by title. `--probe` takes zero or
more `ID:NORMALIZED_VALUE` edits. A multi-edit probe approximates a changed
parameter scene; it is **not** a Surge factory preset change. The probe is
excluded from the default build and CTest because it loads third-party code
in-process.

## Observations

| Class and controls | Normalized values | Initial latency | Fresh restored-state latency |
| --- | --- | ---: | ---: |
| Instrument baseline | unchanged | 0 | 0 |
| A Filter 1 Cutoff | 0.05, 0.95 in separate runs | 0 | 0 |
| A Filter 1 Resonance | 0.95 | 0 | 0 |
| Polyphony Limit | 0.9 | 0 | 0 |
| FX A1 FX Type | 0.5, 1.0 in separate runs | 0 | 0 |
| Instrument scene: Resonance, FX Type, Cutoff | 0.95, 1.0, 0.95 | 0 | 0 |
| Effects baseline | unchanged | 0 | 0 |
| Effects FX Parameter 0 | 0.05, 0.95 in separate runs | 0 | 0 |
| Effects FX Type | 0.5, 1.0 in separate runs | 0 | 0 |
| Effects Bypass | 1.0 | 0 | 0 |
| Effects scene: Parameter 0, FX Type | 0.95, 1.0 | 0 | 0 |

Every process call and fresh-state restoration succeeded. Some saved state
sizes and output peaks changed when FX Type or other controls changed. Those
facts indicate different states and audible signals; they do not prove the
plugin applied every parameter at the requested sample or that it retained
constant latency during active processing. The probe prints
`cached_latency_after_event_samples`, but that value is the host's pre-event
cache, **not** a live query of the plugin processor.

Surge's bundled factory patches are `.fxp` files. The current VST3 host has no
factory-preset import/switch seam, and this test did not use the native editor
to select one. Actual factory preset changes therefore remain unmeasured.

## Live-edit admission gate

**Binary-wide admission remains closed.** The Quick development build now
admits only Filter 1 Cutoff on the exact installed Surge XT Synth 1.3.4 binary,
at 48 kHz and 64 or 128 requested frames. It does not admit the Effects
binary or any other Surge parameter. The scoped decision follows the active
sweep below plus a real native-editor gesture whose saved revision received an
audio-callback applied receipt without stopping playback. It is not a release
qualification for all patches or device configurations.
The production host caches `getLatencySamples()` at creation and after an
explicit suspended state restore; it cannot establish a latency change caused
by a live parameter event. The component handler now records restart flags
atomically but still returns `kResultFalse`: it has **not** fulfilled a
requested restart. The probe reports `fixed_latency_qualified: false` on every
result.

### Restart-request observation on host base `1300b10`

The opt-in probe drained restart flags before the edit, after the active
128-frame process block, and after restoring component state into a fresh
instance. The synthetic handler fixture verifies bitwise aggregation and a
single drain without acknowledging the request. With Surge XT 1.3.4, the
instrument baseline and Filter 1 Cutoff 0.95, and Effects baseline and FX
Type 1.0, each reported `0` before the edit and after the process block.
Each fresh state restore reported `4` (`kParamValuesChanged`); this is a
request to refresh controller parameter values, not evidence that latency
changed. Initial, cached post-block, and fresh restored-state latencies were
all `0` in these four cases. Each case was isolated with a 25-second timeout.
The instrument and Effects binary hashes above still match the scanned
bundles. No native editor gesture or factory preset change was exercised in
this headless probe. Absence of a request in these samples does not prove
fixed latency for all controls, presets, or block transitions.

Before admitting a real Surge control to the live manual-event path, capture
and report its controller restart flags, sample its processor's declared
latency before and after the actual active edit, test relevant parameter
extremes and factory preset switches, and verify graph/PDC handling when
latency changes or a restart is requested. Qualify at the binary/version and
parameter level; these sampled zero-latency state reloads do not justify a
blanket allowlist. Interactive X11/Wayland editor behavior and perceptual
review remain separate gates.

## Active-instance Filter 1 Cutoff sweep

The opt-in `--surge-cutoff-sweep` command is pinned to instrument class
`abcdef019182faeb566d624153675854`, continuous parameter `1945359057`
(A Filter 1 Cutoff), and binary SHA-256
`b8584398f314819241ca7287117d5559ed838d37ef08faab9847f27ba2993cec`.
It refuses another binary. It creates one active instance with a 128-frame
maximum block and processes actual 64- or 128-frame blocks at 48 kHz. A single
note starts in block 0. The values `0, .05, .5, .95, 1, .95, .5, .05` repeat
eight times, holding each value for eight blocks and delivering the event
halfway through the first block of each hold. That is 512 blocks and 64
parameter events per run. It drains restart flags after every block, then
suspends, saves state, opens a new instance, restores state, and processes a
new note. No plugin call overlaps the render callback in this probe.

Run locally with a timeout and keep output inside the build tree:

```sh
mkdir -p build/quick/scratch
TMPDIR="$PWD/build/quick/scratch" cmake --build build/quick --target oscillate_vst3_live_latency_probe
PROBE=build/quick/tests/plugins/oscillate_vst3_live_latency_probe
TMPDIR="$PWD/build/quick/scratch" "$PROBE" --self-test-sweep
for FRAMES in 64 128; do
  TMPDIR="$PWD/build/quick/scratch" timeout 30s "$PROBE" --surge-cutoff-sweep \
    '/usr/lib/vst3/Surge XT.vst3' "$FRAMES" \
    > "build/quick/scratch/surge-cutoff-${FRAMES}.json"
done
```

On host base `f6de406` with the installed 1.3.4 bundle, both runs completed:

| Actual block | Blocks | Finite nonzero samples | Active restart flags | Suspend flags | Restore flags | Initial / suspended cached / fresh restored latency | Restored first-block peak |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 512 | 65,412 | 0 | 0 | 4 | 0 / 0 / 0 | 0.01368 |
| 128 | 512 | 131,052 | 0 | 0 | 4 | 0 / 0 / 0 | 0.01740 |

The JSON records per-value peak and absolute-sum output. Every requested
value generated finite output; this is a signal-presence check, not proof of
the audible cutoff response or of exact sample-offset application. The
`suspended_cached_latency_samples` field remains the host's cached value; the
fresh instance queries its processor after state restoration. No active
latency-change request was observed in these 1,024 sampled blocks. Restore
flag `4` means `kParamValuesChanged`, as in the earlier survey. The synthetic
TimedInstrument `--self-test-sweep` passed at both block sizes, checking the
sequence counts, nonzero output, state reopening, and closed admission field.

The official Surge source tag
[`release_xt_1.3.4`](https://github.com/surge-synthesizer/surge/tree/f7b97c682ade0b87da85ca5968b63d5c7c98e68d)
supports a narrow interpretation. Its
[`SurgeSynthProcessor.cpp`](https://github.com/surge-synthesizer/surge/blob/f7b97c682ade0b87da85ca5968b63d5c7c98e68d/src/surge-xt/SurgeSynthProcessor.cpp)
and corresponding header contain no `setLatencySamples` call or latency
override; this suggests the instrument retains JUCE's default declared
latency, but the installed binary and unsampled controls still need runtime
qualification. In contrast,
[`SurgeFXProcessor.cpp`](https://github.com/surge-synthesizer/surge/blob/f7b97c682ade0b87da85ca5968b63d5c7c98e68d/src/surge-fx/SurgeFXProcessor.cpp#L272-L283)
explicitly changes its declared latency to `BLOCK_SIZE` and notifies the host
when it leaves its non-latent block mode. The Effects binary therefore must
not be admitted on a binary-wide fixed-latency assumption.

This cutoff evidence is **parameter-specific** and does not qualify the
instrument binary for arbitrary live parameter edits. The probe did not
exercise factory patches, alternate synth patches, long-run stress,
sample-offset audio correctness, or latency-changing graph/PDC behavior. Its
`fixed_latency_qualified` result stays `false` because that field describes
the probe's former binary-wide gate. The Quick per-parameter policy additionally
checks exact binary SHA-256, class ID, parameter ID, project rate and requested
block size before exposing a live event slot. A real in-app X11 editor drag on
Filter 1 Cutoff saved project revision 44 and received an applied audio receipt
at sample 99,840 while transport continued. On restart, revision 44 reopened
with the saved normalized cutoff reference and the native slider at its edited
position. Broader plugin controls and release compatibility remain open.
