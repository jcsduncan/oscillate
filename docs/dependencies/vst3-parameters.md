# VST3 parameter discovery and input events: first host slice

**Status:** Synthetic controller and processor fixtures pass locally. This is
not a third-party compatibility claim. **Requirements:** FS-04 PI-03/04;
FS-03 automation precision.

The effect host discovers parameters while an instance is constructed off the
audio callback. It asks a component for its separate controller class ID; if
none is declared, it queries the component for a combined `IEditController`.
The controller is initialized when separate, receives a minimal metadata
handler, and is released before the component and the containing module.
The handler rejects editor-origin edits because this slice has no editor or
model-command bridge.

Discovery accepts at most 256 parameters. It copies stable numeric IDs,
UTF-8 title/short title/units, step count, normalized default and automation
flags into host-owned metadata. Duplicate IDs, invalid UTF-16, empty titles,
out-of-range defaults, contradictory automate/read-only flags, and malformed
counts fail construction with a diagnostic. All discovery and controller state
transfers run off the callback. After a suspended component-state restore,
the same bytes are forwarded to the controller's `setComponentState`; failure
faults that runtime rather than resuming with divergent component/controller
state. Controller-specific opaque state and parameter-value snapshots remain
future work.

The callback accepts at most a configured 1–1024 normalized events per block.
Events must be sorted by parameter ID, then by strictly increasing sample
offset within that ID. An offset is in `[0, block_frames)` and a normalized
value is finite and in `[0, 1]`. Validation completes before DSP; rejected
events never invoke the processor. Fixed-capacity, read-only
`IParameterChanges`/`IParamValueQueue` views group a caller-owned event span
without copying, allocation, locks or I/O. The caller must keep that span
alive until `process` returns. Event preparation and sorting belong to a
control or worker side of the graph. The preallocated queue views are valid
only for that process call and an instance has one exclusive render owner.

`parameter_precision(id)` reports `sample_offsets_forwarded` for automatable
parameters, `control_only` for known nonautomatable parameters, and
`unavailable` for unknown IDs or absent controllers. The first result says
only that the host submitted the VST3 sample offsets. A plugin may quantize,
smooth or ignore them; actual precision is unverified. There is no callback
side fallback that silently moves a parameter change to a block boundary.
Control-only editing, plugin-origin edits and full automation integration
require the later model/UI bridge.

The code adds direct MIT-licensed SDK interface headers
`pluginterfaces/vst/ivsteditcontroller.h` and
`pluginterfaces/vst/ivstparameterchanges.h`, with their transitive
`pluginterfaces` headers. It compiles no SDK `.cpp` files and adds no runtime
dependency. Root and `pluginterfaces/LICENSE.txt` notices still apply.

Synthetic tests exercise separate-controller lifecycle, multi-parameter queue
grouping, first/last sample offsets, state restoration order, invalid event
blocks, malformed metadata and no-allocation callback behavior. Plugin CPU,
fault, editor, MIDI, dynamic latency and third-party compatibility remain
separate qualification work. In-process plugin faults are not hard-contained.

On an Intel Core i7-10700K, GCC 15.2 Release build at 48 kHz, one local run of
100,000 synthetic stereo gain blocks after warmup measured 52 ns at 64 frames
and 90 ns at 128 frames without events, versus 364 ns and 650 ns with two
parameter points. Both paths allocated zero times inside `process`. These are
adapter-plus-fixture timings; the fixture deliberately probes queue ABI
methods in every parameter block, so they do not predict real plugin cost or
whole-graph underruns. A repeatable reference-hardware workload remains the
performance gate.
