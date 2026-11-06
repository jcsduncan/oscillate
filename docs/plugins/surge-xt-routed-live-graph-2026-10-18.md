# Surge XT whole routed-graph probe, 18 October 2026

**Gate: red.** The installed Surge XT 1.3.4 instrument allocates C++ heap memory during
processing on the render thread. Do not count this as PI-09 or G7 real-time
qualification. The probe is an opt-in development executable, excluded from
the default build and CTest. It does not open an audio device.

## Scope and method

The probe pins the installed Surge XT Synth VST3 class
`abcdef019182faeb566d624153675854` and binary SHA-256
`b8584398f314819241ca7287117d5559ed838d37ef08faab9847f27ba2993cec`.
It refuses a changed binary. On an Intel i7-10700K, Ubuntu 26.04.1,
Linux 7.0.0-38-generic, GCC 15.2.0 Release build, it ran actual 64- and
128-frame blocks at 48 kHz. Each run processed 512 consecutive blocks without
device pacing (0.683 or 1.365 seconds of represented audio). The desktop load
was uncontrolled.

The project has one Surge instrument track, 16 consecutive sixteenth notes at C4
in a MIDI clip, a group main route, a send to a return bus, and master EQ. The
probe saves and reopens this project before playback. During playback it saves
a Filter 1 Cutoff (`1945359057`) value of 0.05 through the real instrument
manual-edit transaction, checks that the model revision remains pending until
the callback applies the event, then checks the applied receipt and audible
revision. It saves and reopens again and checks the value and exact plugin
identity. Output is finite and nonzero in at least 511 of 512 blocks;
the processor reports no fault. Signal presence is not a perceptual or
sample-offset correctness oracle for the cutoff's sound.

The callback allocation counter wraps the whole routed `render_block` call.
An isolated, fresh Surge host in the same executable processes the same note
onsets and cutoff event with the same block size. It produces the **exact same
allocation count at the exact same block indices**. This localizes these C++
`operator new` calls to the Surge/VST3 host processing path rather than the
routed mixer or graph publication. A fresh **TimedInstrument** synthetic VST3
processed the identical 16-note schedule through the same generic host and
recorded **zero** callback C++ allocations in 512 blocks at each size. The
allocations therefore depend on Surge's processing behavior, or on a generic
host branch that only Surge triggers in this tested pair. The counter does not
observe `malloc`,
other allocators, plugin locks or system calls; the observed count is a lower
bound on prohibited work. The counter alone cannot distinguish a call made by the
plugin binary from one made by the generic host at the plugin boundary.

### Allocation caller follow-up

On the same installed binary, a Release build from the main checkout was run
under GDB with a breakpoint on `operator new` conditional on the probe's
thread-local callback-counting flag. The first counted allocation originated
inside `Surge XT.so`: `juce::MidiKeyboardState::noteOnInternal` grew a JUCE
listener iterator vector while `SurgeSynthProcessor::applyMidi` handled a note
in `processBlock`. The third counted allocation also originated inside that
binary: `SurgeMacroToJuceParamAdapter::getName` constructed a `juce::String`
through `JuceVST3EditController::audioProcessorChanged` and
`SurgeSynthProcessor::processBlockPostFunction`. Both stacks then passed
through the ordinary `JuceVST3Component::process` call into our generic
`Vst3EffectHost::process_instrument`. These sampled C++ allocations are plugin
code paths, not a special case in the DAW host. GDB changes the timing of this
diagnostic run, so the separate uninstrumented timing table remains the timing
evidence. The trace does not classify every counted allocation or other
allocation APIs, and the callback-allocation gate remains red.

| Frames | Isolated host and whole graph allocations | Allocating blocks | Largest block | Whole-graph max callback | Over nominal period | Audio/fault/save |
| ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 64 | 3,883 each | 7/512 | 3,876 allocations at block 11 | 381.775 µs | 0/512 | Passed |
| 128 | 3,888 each | 12/512 | 3,876 allocations at block 11 | 393.660 µs | 0/512 | Passed |

Both sizes allocated twice at block 0 and 3,876 times at block 11. Subsequent
single allocations coincided with every new note onset: blocks 93, 187, 281,
375, 468 at 64 frames; blocks 46, 93, 140, 187, 234, 281, 328, 375, 421,
468 at 128. These continued after the initial warmup. The nominal periods
are 1,333.33 and 2,666.67 µs. The zero over-period counts only describe this
short, fast-forward, local run; they provide no backend xrun evidence.

### Steady playback versus the admitted cutoff edit

A follow-up opt-in `steady` mode omits the manual parameter event while keeping
the saved project, 16-note schedule, route, block count and instrument binary
the same. At both block sizes, isolated-host and whole-graph allocation counts
and **every allocating block index** were identical between steady playback and
the cutoff-edit run. The edit itself introduced no additional counted C++
allocation; ordinary playback still fails the callback gate. This is evidence
against a host graph-publication allocation at the edit boundary, not a claim
that all plugin allocations have been classified.

| Frames | Mode | Isolated host | Whole graph | Allocating blocks | Largest block | Nonzero audio blocks | Faults / nonfinite / over-period |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | --- |
| 64 | steady | 3,883 | 3,883 | 7/512 | 3,876 | 512/512 | 0 / 0 / 0 |
| 64 | cutoff edit | 3,883 | 3,883 | 7/512 | 3,876 | 512/512 | 0 / 0 / 0 |
| 128 | steady | 3,888 | 3,888 | 12/512 | 3,876 | 512/512 | 0 / 0 / 0 |
| 128 | cutoff edit | 3,888 | 3,888 | 12/512 | 3,876 | 512/512 | 0 / 0 / 0 |

The local probe used the installed Surge XT 1.3.4 instrument binary with SHA-256
`b8584398f314819241ca7287117d5559ed838d37ef08faab9847f27ba2993cec`.
The exact Release probe executable with debug information used for the final
four-run table had SHA-256
`4a6ef0fabdd1870bab1ad862cc49cf4d12aa169daf3172e0470d9b6eb4802e42`
(GCC 15.2.0, `-O3 -DNDEBUG -g`, Intel i7-10700K, Linux
7.0.0-38-generic). An earlier `-O3 -DNDEBUG` build of the same probe source
also produced the same counts and block indices. Desktop load was uncontrolled.
The debug information was added for caller inspection; no timing conclusion
depends on the GDB run.

GDB stopped on the **first counted allocation** of the 64-frame steady isolated
host run. Its caller stack went from the probe's `operator new` into
`Surge XT.so`'s JUCE listener-vector growth,
`juce::MidiKeyboardState::noteOnInternal`,
`SurgeSynthProcessor::applyMidi`, and `SurgeSynthProcessor::processBlock`, then
through `JuceVST3Component::process` to the DAW's generic
`Vst3EffectHost::process_instrument`. This directly attributes that sampled
allocation to the Surge binary. The earlier cutoff-run GDB samples above also
pointed into that binary. These stacks are samples, not a classification of all
3,883/3,888 calls or of `malloc`, locks and system calls. A fresh synthetic
TimedInstrument processed the same note schedule through the same host with
zero counted C++ allocations at 64 and 128 frames. No generic host defect was
proven, so the strict gate and current production policy stay unchanged.

The same opt-in path also passed a **test-only**, explicitly qualified manual
event for A Filter 1 Resonance (`8095466`, continuous, automatable), at 0.95.
At both block sizes its edit received an applied callback receipt, advanced
the audible revision, and survived save/reopen. The isolated host reported
zero active restart flags, cached latency 0 before and after the event, and
latency 0 after restoring the saved state into a fresh instance. Its callback
allocation pattern remained identical to the cutoff case. This proves the
current manual-event seam can carry this control in a bounded headless run.
It does **not** prove the active plugin's true latency did not change, that
the requested resonance was audible, or that native-editor input works. The
production policy remains cutoff-only pending those checks and the allocation
issue; this test does not broaden admission.

The probe deliberately exits with status 1 after printing the evidence because
the project's audio callback contract prohibits allocation. PI-09 remains
parameter-scoped; the other Surge controls, patches, FX binary, editor input
and physical device load are separate qualifications. The earlier
[host stress run](surge-xt-long-run-host-2026-10-18.md) did not measure
allocations, and its green timing result cannot override this red contract.

## Reproduce

Configure the standalone qualification CMake project with the pinned local
dependencies, then build only `oscillate_surge_live_graph`. In this checkout,
the local Release build is `build/surge-graph`; keep `TMPDIR` in its `scratch`.

```sh
mkdir -p build/surge-graph/scratch
TMPDIR="$PWD/build/surge-graph/scratch" \
  cmake --build build/surge-graph --target oscillate_surge_live_graph
for FRAMES in 64 128; do
  TMPDIR="$PWD/build/surge-graph/scratch" timeout 45s \
    build/surge-graph/oscillate_surge_live_graph \
    '/usr/lib/vst3/Surge XT.vst3' "$PWD/build/surge-graph/scratch" \
    "$FRAMES" 512
done
# Compare ordinary playback under the same fixture without a manual event.
for FRAMES in 64 128; do
  TMPDIR="$PWD/build/surge-graph/scratch" timeout 45s \
    build/surge-graph/oscillate_surge_live_graph \
    '/usr/lib/vst3/Surge XT.vst3' "$PWD/build/surge-graph/scratch" \
    "$FRAMES" 512 steady
done
# Optional resonance survey; this also exits 1 on callback allocations.
TMPDIR="$PWD/build/surge-graph/scratch" timeout 45s \
  build/surge-graph/oscillate_surge_live_graph \
  '/usr/lib/vst3/Surge XT.vst3' "$PWD/build/surge-graph/scratch" \
  64 512 resonance
```

For one sampled steady-playback caller stack, configure the same local Release
build with `-DCMAKE_CXX_FLAGS_RELEASE='-O3 -DNDEBUG -g'`, rebuild only
`oscillate_surge_live_graph`, then run:

```sh
TMPDIR="$PWD/build/surge-graph/scratch" timeout 60s gdb -q -batch \
  -ex 'set debuginfod enabled off' -ex 'set pagination off' \
  -ex 'break operator new(unsigned long) if count_allocations' \
  -ex run -ex 'bt 9' --args build/surge-graph/oscillate_surge_live_graph \
  '/usr/lib/vst3/Surge XT.vst3' "$PWD/build/surge-graph/scratch" \
  64 64 steady
```

This stops at one allocation; it does not produce timing evidence or classify
the remaining calls.

Each Surge command currently exits 1 on the allocation gate. Before any
production admission, classify the remaining allocation callers and retest a
bounded policy with the exact binary and project workload.
Do not change the strict callback rule or mark arbitrary Surge edits safe to
make this probe green. For the synthetic control, build the optional fixture and
run the same host-only measurement:

```sh
TMPDIR="$PWD/build/surge-graph/scratch" cmake --build build/surge-graph \
  --target oscillate_qualification_instrument_fixture
for FRAMES in 64 128; do
  TMPDIR="$PWD/build/surge-graph/scratch" timeout 45s \
    build/surge-graph/oscillate_surge_live_graph --host-only \
    "$PWD/build/surge-graph/fixtures/TimedInstrument.vst3" \
    464748494a4b4c4d4e4f505152535455 "$FRAMES" 512
done
```
