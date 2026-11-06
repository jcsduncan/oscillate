# VST3 compatibility probe: finite output through restore

**Date:** 18 October 2026. **Scope:** local PI-03/PI-04 host qualification, without a
physical audio device or native editor. This is a probe correction, not a
change to the production VST3 host or a wider plugin admission.

The compatibility probe previously checked both output channels for finite
samples on its first block and during optional stress runs. Its ordinary
parameter-event, component-state restore and project-reopen blocks checked
only the VST3 process result. A plugin returning `kResultOk` while emitting
NaN or infinity at any of those boundaries could therefore earn the probe's
`host_status: tested`. The probe now fails the specific stage with
`status: known-problematic` on any nonfinite sample in either channel. The
`parameter_event_output_finite` field is emitted when an event is forwarded
and that block passes. The synthetic self-test includes finite, NaN,
infinity and unequal-channel-length cases; the existing fixture test crosses
both 64- and 128-frame process, restore and reopen paths.

| Installed class | Binary SHA-256 | 64 frames | 128 frames |
| --- | --- | --- | --- |
| Surge XT Synth 1.3.4 | `b8584398f314819241ca7287117d5559ed838d37ef08faab9847f27ba2993cec` | First, parameter, restored and reopened blocks finite | First, parameter, restored and reopened blocks finite |
| Surge XT Effects 1.3.4 | `9b2da5f1b4a81a02ad55c88eba55257eb4a7f1747a1fd0866b8eaf5b7c8d0f45` | First, parameter, restored and reopened blocks finite | First, parameter, restored and reopened blocks finite |

All four runs retained `host_status: tested`, `status: unverified`, exact
project identity/state reopen, and `controller_state_sync: not_implemented`.
The event IDs were `2465059` for the instrument and `720135338` for Effects;
the parameter event was submitted at half the actual block length. The event
check proves finite output and queue delivery, not audible response or exact
sample-offset behavior. The probe still does not assess native editor input,
whole-graph routing, callback allocations, deadline misses or PDC after a
dynamic latency change. The separate routed Surge probe's callback allocation
gate remains red; this result does not override it.

To reproduce from a configured VST3 build, keep scratch under its build tree:

```sh
mkdir -p build/quick/scratch
TMPDIR="$PWD/build/quick/scratch" cmake --build build/quick \
  --target oscillate_vst3_compatibility_probe
TMPDIR="$PWD/build/quick/scratch" \
  build/quick/tests/plugins/oscillate_vst3_compatibility_probe --self-test
TMPDIR="$PWD/build/quick/scratch" \
  build/quick/tests/plugins/oscillate_vst3_compatibility_probe --self-test-stress
```

Then use the same binary with each pinned bundle/class, `instrument` or
`effect`, the scratch directory, and `64` or `128`, as documented in the
[original compatibility check](surge-xt-compatibility-2026-10-18.md). The
four installed-binary runs and both self-tests passed locally. No public
API, project schema, real-time code path, dependency or licensing decision
changed. The probe's JSON gained one optional evidence field; its format
number and existing fields are unchanged.
