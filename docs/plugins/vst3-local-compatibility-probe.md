# Local VST3 compatibility probe

**Scope:** PI-03/PI-04/PI-05 qualification aid, not a release certification or an
application feature. The probe is an opt-in executable excluded from the
default build. It runs one selected class in its own process. Use a fresh
process for every class, because third-party code can crash or hang inside the
host after its out-of-process scan. Run it only on bundles you intend to test.

Build with the pinned VST3 SDK and existing local dependencies configured:

```sh
mkdir -p build/vst3/scratch
TMPDIR="$PWD/build/vst3/scratch" cmake --build build/vst3 --target oscillate_vst3_compatibility_probe
TMPDIR="$PWD/build/vst3/scratch" build/vst3/tests/plugins/oscillate_vst3_compatibility_probe --self-test
```

Discover the class ID, then run an instrument or effect probe. Keep `WORK_DIR`
inside the selected build tree; the temporary project is removed after the
probe. Save the JSON output outside the project if you want a qualification
record. Bundle path, version and SHA-256 identify exactly what was tested.

```sh
PROBE=build/vst3/tests/plugins/oscillate_vst3_compatibility_probe
"$PROBE" --list "$HOME/.vst3/Example.vst3"
TMPDIR="$PWD/build/vst3/scratch" "$PROBE" "$HOME/.vst3/Example.vst3" CLASS_ID instrument "$PWD/build/vst3/scratch" 64
```

The probe checks the scanner receipt, class identity, in-process load, a
64- or 128-frame 48 kHz process call (128 by default), one normalized continuous parameter
event at half of the selected block when available, opaque component and independent controller-state save/rebind, and a real
project-store save/reopen followed by another verified bind and process call.
The plugin is prepared with a 128-frame maximum in both cases; the selected
block size is the actual frame count passed to each process call.
It checks finite output, not expected sound. The parameter result
`forwarded` means the host delivered a sample-offset event; plugin response
and interpolation accuracy remain unverified. No continuous automatable
parameter leaves that check `unverified`.
The block peaks show signal presence only where nonzero; an instrument may
produce silence in its first block and sound later. Differing peaks across
instances are not an audio equivalence test.

`host_status: tested` means only these automated host checks passed for this
exact binary and class. Overall `status` stays `unverified` pending listening,
real project workflow, performance and native GUI review. `known-problematic`
means the selected class failed a check under the probe conditions; it is not
a diagnosis of every host or plugin version. A missing bundle or wrong class
ID remains `unverified`, not a plugin defect. `native_gui` is always
`unverified`: the X11 capability probe does not create or attach a window,
and Wayland and editor run-loop behavior require an interactive test.

The committed self-test uses the synthetic ConnectedGain and TimedInstrument
bundles for deterministic success, a wrong class, and a missing bundle. It
does not establish any third-party compatibility. Record manual listening,
GUI, project workflow, plugin version, host commit, OS/window system, sample
rate and known limits alongside probe JSON before marking a real plugin
`tested` in the product catalogue.

## Local third-party observation, 18 October 2026

Surge XT 1.3.4 appeared in `/usr/lib/vst3` during this work. Its instrument
binary is `b8584398f314819241ca7287117d5559ed838d37ef08faab9847f27ba2993cec`;
its Effects binary is `9b2da5f1b4a81a02ad55c88eba55257eb4a7f1747a1fd0866b8eaf5b7c8d0f45`.
Both passed the out-of-process scan. The instrument has one active auxiliary
stereo input, one active main stereo output, two inactive secondary stereo
outputs, and one event input. Effects has one active main and one active
auxiliary stereo input plus one active main stereo output. Both support
float32 processing.

The host now bounds audio buses to eight, prepares scratch buffers off the
callback, supplies silence to unconnected auxiliary inputs, and discards
secondary outputs. A synthetic two-input/two-output fixture checks the
silent auxiliary input, unchanged main output, and zero host callback
allocations at 64 and 128 frames. This **does not** expose Surge's extra
outputs or route sidechain audio to its auxiliary input; those remain product
limits. The host reports their counts to project binding, and the Quick
instrument/effect editor shows the silent-input/discarded-output warning.
This does not make multi-output or sidechain routing complete.

The local probe passed host open, a non-silent first process block, one
continuous parameter event delivered at sample 64, opaque component-state
restore, project-store save/reopen, rebind, and first process after reopen
for both classes. The instrument's three tested blocks were non-silent, with
peaks varying between runs; Effects produced 0.219 on all three blocks with
the fixed input. These are signal-presence checks, not audio equivalence or
quality measurements.

Both Surge controllers return `kNotImplemented` to `setComponentState` without
reading the state bytes. The SDK's preset loader accepts this optional result,
so component DSP-state restore now succeeds and the host reports
`controller_state_sync: not_implemented`. A bound result carries a diagnostic
that native editor state needs separate review. That original X11 probe reported
only `x11_candidate_only`; it did not test window attach, Wayland behavior,
interactive editing, listening or a real project workflow. Overall compatibility status
therefore remains **unverified**, while the narrower automated host status is
**tested** for these exact binary hashes.

## Interactive X11 editor observation, 18 October 2026

An isolated Xvfb run with the installed Surge XT instrument opened its native
editor through the Qt VST3 attachment. Before the host run-loop fix the editor
drew but ignored an XTest drag of its pitch control. JUCE registers one
`IEventHandler` for multiple file descriptors; the host rejected the second
descriptor, leaving the editor input loop incomplete. After accepting multiple
descriptors for that handler, the same drag moved the pitch control, Surge
marked the patch modified, and the host captured one parameter edit. A focused
contract test now checks registration,
dispatch and complete unregistration of both descriptors.

The Qt Quick shell hosts the native X11 editor in an in-app overlay. A
synthetic view test checks attach, embedding and detach; the Surge Xvfb probe
confirmed that the embedded editor responds to pointer input. This evidence
does not qualify Wayland, live editing during playback, audio response to the
parameter, or save/reopen of an interactive Surge preset change.
