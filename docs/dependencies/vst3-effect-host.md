# VST3 effect-host core: first contract slice

**Status:** Synthetic factory exercised locally. This is not a production
compatibility result. **Requirements:** FS-04 PI-03, PI-04, PI-08.

The host accepts one scanned Vst3Class identity and a caller-owned
IPluginFactory through a private adapter seam. SDK types remain inside
src/plugins and its fixture tests. The public Vst3EffectHost API exposes
activation, exclusive block processing, suspension, latency and bounded
component-state save/restore. There is no bundle loader in this slice; the
scanner does not instantiate plug-ins. The integration owner can link a
verified scanner result to a loader and this factory seam later.

The adapter accepts exactly one main stereo audio input and output bus with
32-bit float samples, a configured sample rate, and a maximum block of 1–8192
frames. Setup, activation, processing and state transfers return explicit
errors. A process failure marks that runtime faulted until replacement. The
host process path creates only stack descriptors and does not allocate; the
synthetic fixture's allocation oracle covers the combined call. A real
third-party plug-in can itself allocate, block, throw, or crash in process.
In-process hosting gives no crash containment guarantee.

Component state is capped at 16 MiB by configuration and is read or written
only while processing is suspended. A bounded in-memory IBStream supports
read, write, seek and tell. State belongs to the component only; controller
state, parameter metadata/events, process context, MIDI, sidechains, bypass,
tail reporting, dynamic latency notification, editor run loop, and additional
bus arrangements are separate work. Plug-ins requiring a host application
context may reject this first adapter. The graph must own a host instance
exclusively and keep it alive across compatible topology changes; this slice
does not publish or reclaim graph instances.

The code compiles **no SDK C++ sources**. Direct SDK headers are
pluginterfaces/base/ipluginbase.h, pluginterfaces/base/ibstream.h,
pluginterfaces/vst/ivstcomponent.h,
pluginterfaces/vst/ivstaudioprocessor.h, and
pluginterfaces/vst/vstspeaker.h. Transitive headers are
pluginterfaces/base/falignpop.h, falignpush.h, fplatform.h, fstrdefs.h,
ftypes.h, funknown.h, smartpointer.h, and pluginterfaces/vst/vsttypes.h,
all from the pinned pluginterfaces submodule. The root and
pluginterfaces/LICENSE.txt MIT notices apply. The synthetic factory uses the
same interface headers; it does not use public.sdk, base, or VSTGUI code.
The SDK include path is marked SYSTEM for the host target because its packing
pragmas otherwise trigger Clang warnings-as-errors.

The effect-host target is included in the explicit `vst3` preset and uses the
existing `OSCILLATE_VST3_SDK_ROOT` cache path. No project schema, editor or UI
link is introduced.

### Local evidence

Synthetic factory tests cover creation/initialization cleanup, stereo
arrangement and activation, sample-size rejection, 64-frame gain output,
latency, suspended state save/restore, a bounded sparse-seek stream, and
exceptions and failures at setup, process and state boundaries. GCC, Clang,
ASan/UBSan and Release fixture tests pass; scoped clang-tidy and clang-format
pass. The host source has 278/292 executable lines covered (95%). This is
fixture evidence only; physical reference plug-ins and full graph tests remain
required.

On an Intel Core i7-10700K, GCC 15.2 Release build, 48 kHz, the trivial gain
fixture measured median 23 ns per 64-frame block and 32 ns per 128-frame block
over three local runs of 100,000 blocks after warmup. Host plus fixture
allocations inside process were zero. Clock scaling caused variation
(18–31 ns and 27–48 ns respectively). These timings measure the adapter and
one trivial synthetic effect, not a real VST3 plug-in, whole graph or underrun
qualification.
