# VST3 fixed-tail qualification seam

The VST3 routed-insert provider does not infer a fixed export drain from
`IAudioProcessor::getTailSamples()`. A successful scan proves which binary was
loaded, but it does not prove that a plugin actually becomes silent after its
declared tail or keeps that bound after parameter and state changes.

`Vst3FixedTailQualification` is a control-side, session-local receipt for a
separately tested **effect**. It binds the exact scanned binary SHA-256 and byte
size, class/vendor/version, saved component and independent controller bytes,
and parameter references,
sample rate, declared tail, and evidence at both 64- and 128-frame blocks. The
provider compares the receipt with the loaded runtime's tail declaration and
sets `PreparedTrackInsert::fixed_tail_qualified` only for an exact match. It
rejects saved automation, manually qualified live parameter control, changed
settings, changed binary or metadata, an untested block size, and a changed
declaration. Project files never contain or implicitly regenerate this receipt.

The deterministic LoadedGain fixture covers the seam. At both callback
sizes it checks the output ends after the fixture's seven-frame latency plus
zero declared tail, remains silent on the next block, and repeats after a
fresh provider is created from the same saved state. It also checks state,
binary, tail and test-size invalidation. This is synthetic evidence only: no
third-party binary is qualified by the repository. In particular, Surge XT
remains ineligible for the JACK export path while its callback-allocation gate
is red.

Before issuing a production receipt, a compatibility runner must test the
specific binary and supported state class with impulses, sustained input,
silence drains, relevant parameter extremes and state restore/reopen, at both
block sizes and the target sample rate. It must check actual silence or a
defined residual threshold after the declared drain, finite output, latency
and tail stability, callback allocations, faults and repeatability. A saved
state or parameter change requires fresh evidence. A production caller must
also prevent an attached editor or other external control path from mutating
the prepared runtime during capture, or invalidate the receipt immediately;
this provider cannot observe an arbitrary plugin's private mutations. The
current fixture is not a substitute for that runner or for physical JACK
qualification.

## Opt-in fixed-tail screen

`oscillate_vst3_fixed_tail_probe` runs one exact scanned effect binary at 48 kHz
and both 64- and 128-frame blocks. For each block size it repeats an impulse
and 4,096 frames of stereo sustained input after a fresh state restore, then
feeds silence through the declared tail, latency and a 2,048-frame guard. It
records finite output, peak beyond the declared drain (limit `1e-6` linear),
exact repeatability, reported latency and tail, restart flags, and C++ `new`
calls inside `process`. `--parameter ID` additionally probes values 0, 0.5
and 1 at the start of the sustained run. A tail beyond two seconds or an
unavailable/infinite declaration stops the probe. The JSON carries the full
saved component and optional controller bytes as hex for manual matching to the
session-local receipt (null means absent; an empty string means present-empty);
it always sets `production_admitted: false`.

Build and run from the canonical configured build tree:

```sh
mkdir -p build/quick/scratch
TMPDIR="$PWD/build/quick/scratch" cmake --build build/quick --target oscillate_vst3_fixed_tail_probe
TMPDIR="$PWD/build/quick/scratch" build/quick/tests/plugins/oscillate_vst3_fixed_tail_probe --self-test
TMPDIR="$PWD/build/quick/scratch" build/quick/tests/plugins/oscillate_vst3_fixed_tail_probe \
  --bundle /path/to/Effect.vst3 --class 0123456789abcdef0123456789abcdef \
  --output build/quick/scratch/effect-tail-evidence.json
```

Pass `--state-file` with raw VST3 component-state bytes and, when present,
`--controller-state-file` with the independent controller bytes to test a
specific saved state. Each file is limited to 4 MiB. The tool reports an
unsuccessful screen with exit code 2 and JSON;
invalid inputs or unavailable hosts return 1. The self-test includes a
deliberately dishonest zero-tail synthetic effect that emits a late echo and
must fail the screen.

For a repeat run against a previously reviewed input, pass
`--expect-binary-sha256` with the prior JSON's `binary_sha256`, and
`--expect-component-state-file` and, if present,
`--expect-controller-state-file` with the exact reviewed raw state bytes.
The probe refuses a mismatched binary or state before processing and exits 1.
The expectation files are independent of the state files supplied for this
run; passing the same mutable file for both does not detect an intervening
change. The self-test checks binary and component-state mismatch refusals.

This screen tests only the supplied state and optional one parameter; it does
not enumerate all plugin parameters, presets, GUI edits, sample rates,
transport modes or real project automation. The C++ allocation counter does
not observe direct `malloc`, custom allocators or other threads. Repeatability
is exact for two fresh instances of the same binary and block size, not a
promise across operating systems or plugin builds. Human review, wider state
coverage, callback instrumentation, editor-mutation control and physical JACK
qualification remain required before any production receipt is issued.

## Live removal after insertion

The engine, routed application and Quick handoff already support removing one
active dry VST3 effect at a callback boundary. This is deliberately closed for
an ordinary third-party insert. The only current post-insert receipt supplier
is the synthetic Quick contract test; the normal project bridge never calls
`RoutedPlayback::offer_post_insert_qualification`. The opt-in JSON screen above
is not a receipt supplier and `production_admitted: false` must never be
reinterpreted as permission. A zero `getTailSamples()` declaration, successful
scan, zero measured latency, or whole-binary manual-control qualification is
insufficient on its own.

A production supplier must run independently of the live instance after the
insert has created its final saved plugin ID and state. It must bind the next
project revision, track, insert slot, plugin ID, exact scanned binary/class,
component and controller state, parameter references, project rate, and the
actual host process sizes. It must prove fixed zero latency at both 64 and 128
frames with **no** admitted manual parameter IDs, and separately prove zero
residual tail after impulse and sustained input, fresh restore, silence drain
and repeated runs at both sizes. It must reject faults, restart/latency/tail
changes, nonfinite output and stale or changed saved state. Any editor,
automation or other control that can mutate the active instance invalidates
that tail proof. Qualification and instance preparation run off the audio
callback; the existing provider then verifies the receipt against the loaded
runtime, and the routed control verifies the exact active owner at removal.

The current `application_routed_playback` and `quick_routed_vst3_chains_contract`
tests exercise qualified removal and missing-tail refusal at both 64 and 128
frames. The Quick test also refuses mismatched saved instance, state,
controller state, revision and manual-control evidence. They prove the
handoff and fail-closed behaviour with a synthetic effect, not third-party
qualification. Until a reviewed supplier meets the contract above, normal
VST3 removal remains saved but inactive during playback; Stop and Play applies
it through the ordinary graph rebuild.

### Production receipt gate and next slice

There is currently **no production post-insert receipt producer**. The opt-in
fixed-tail probe emits diagnostic JSON with `production_admitted: false`; its
result cannot be passed through `offer_post_insert_qualification` as a proof.
That probe exercises one supplied state and, optionally, one parameter. It
does not prove the full state class, editor mutations, callback allocation
behaviour outside C++ `new`, or continued silence after every supported edit.
The prepared runtime's zero-tail declaration is also not a proof.

The next implementation slice is an opt-in, non-admitting evidence runner in
the plugins test tools. Given an exact saved effect and verified scan, it
should create fresh instances at the project rate, restore the exact component
and controller states, and run impulse, sustained signal, silence drain and
repeat-render cases at both 64 and 128 frames. It should record the binary
hash and size, class/vendor/version, state bytes, parameter references, actual
host process sizes, declared and measured latency/tail, finite output,
residual peak, restart requests, faults and callback allocations. A deliberate
late-echo fixture, changed state and mismatched binary must fail. The output
remains diagnostic until a reviewed compatibility policy defines the supported
state class, residual threshold, parameter coverage and admission criteria.

A future production supplier may issue a session-local receipt only after that
independent evidence is accepted. It must bind the final saved plugin ID,
track, slot and next project revision before insertion, recheck the verified
scan and prepared instance on publication, and wait for the callback's active
revision acknowledgement before removal is offered. It must invalidate the
receipt on a saved state or parameter edit, automation or manual-event
admission, editor or other control access to the active instance, plugin
restart or latency/tail change, processing fault, binary/scan change, sample
rate or host process-size change, and any graph revision that loses the exact
active owner. A new receipt requires a new independent run; neither project
reopen nor a previous session restores this permission. The 64/128 removal
tests must then cover each invalidation, old-graph continuity on refusal,
saved-versus-audible revision status, callback allocation, and save/reopen.
