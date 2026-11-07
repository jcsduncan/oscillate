# VST3 saved parameter values: restore gate

**Status:** the host's two-stage mechanism and control-side project binding are
tested with an in-process SDK fixture. General third-party compatibility and
live manual editor changes remain open.

## Current behavior and regression

`Vst3PluginInstance.component_state` is the opaque processor state. The existing
binding restores it through `IComponent::setState` and then synchronizes the
controller through `IEditController::setComponentState`, before activation.
Saved `parameters` retain parameter IDs and normalized values for references
and recovery. With opaque state present, the host does not layer those values
over the restored processor state.

If component state is absent and saved parameter values exist, project binding
stages verified automatable values and flushes them before returning a ready
effect. Unknown IDs or a failed stage/flush return `parameter_values_unapplied`
with no effect to publish. The input record remains unchanged, and unknown IDs
are reported in `unresolved_parameter_ids`. A valid component state remains
authoritative; saved parameter references are not layered over it. Whether a
particular component state restores every parameter remains a plugin
compatibility test, not a host guarantee. The connected-bundle regression
checks the Quick editor's stage/flush/save/reopen sequence, first audible
samples, restored latency, conflicting references and unchanged durable data.

## Why a direct setter is insufficient

The pinned VST3 SDK is `v3.8.1_build_84`. Its `IEditController::setParamNormalized`
is a **UI-thread** operation that updates the controller view; it does not by
itself deliver a parameter to the processor. The processor receives parameter
changes through `ProcessData.inputParameterChanges` on the **processing
thread**. The SDK permits a zero-sample, zero-buffer `IAudioProcessor::process`
call to flush parameters before ordinary audio. The current synchronous
project-binding API establishes neither thread owner. Calling both halves in
that function would violate the documented VST3 thread contract or silently
restore only one side.

Sources: [SDK interface thread annotations](https://github.com/steinbergmedia/vst3sdk/blob/v3.8.1_build_84/pluginterfaces/vst/ivsteditcontroller.h),
[parameter delivery](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical%2BDocumentation/Parameters%2BAutomation/Index.html),
[zero-buffer flush](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical%2BDocumentation/API%2BDocumentation/Index.html),
[persistence order](https://steinbergmedia.github.io/vst3_dev_portal/pages/FAQ/Persistence.html).

## Tested host-level stage

`Vst3EffectHost::stage_static_parameter_values` takes a nonempty, sorted,
unique, bounded batch before activation. It rejects invalid, unknown and
non-automatable parameters before mutation and faults that candidate instance,
then calls the controller setter on its caller's UI thread. A failed setter
also faults the instance. The host holds
the batch in preallocated-on-control storage and refuses audible `process()`
while its processor flush is pending.

After ownership transfers to the processing thread, the caller activates the
host and calls `flush_static_parameter_values`. It submits the batch via
`IParameterChanges` in a zero-sample, zero-input/output-buffer `process` call.
The host's queues are allocated during construction. A successful flush opens
the audible `process()` gate; rejection faults the instance. The fixture has
distinct controller and processor objects and checks a 64- and 128-frame first
audible block, both failure stages, and callback allocation count. The caller
must keep all stage and teardown calls off the audio callback. Stage methods
are not thread dispatchers and do not make arbitrary plugins thread safe.

`bind_vst3_project_effect` performs both stages during off-callback preparation
and returns `ready` only after a successful processor flush. The host fixture
exercises SDK interfaces directly, and a separate source-only dynamic bundle
verifies scanner, loader and project binding. The host connects separate
controller and processor `IConnectionPoint` interfaces when both are present.
The synchronous preparation path is not a dispatcher for arbitrary plugin
thread models; compatibility beyond the fixture is unproven.

## Integration contract and remaining checks

1. On the control side, validate IDs, finite normalized values, duplicate IDs,
   parameter flags and the plugin's declared parameter set. Preserve unknown
   IDs and their values in the project; report them distinctly from rejected
   values. Decide whether read-only and bypass/program-change parameters are
   restorable from the plugin's capabilities, rather than assuming all IDs are.
2. Restore component state first. Only if component state is absent, prepare
   a bounded static-value batch. The preparation owner calls controller
   setters on its owned thread and reports each failure. The host must not
   publish a ready effect if a required value fails or cannot be delivered.
3. A processing-thread initialization step delivers that same batch through
   `inputParameterChanges` using a zero-sample process call before the first
   audible block. It owns preallocated queues and buffers. It never allocates,
   locks or waits in the audio callback. If the plugin rejects the flush, keep
   the old graph/placeholder active and report the failed binding.
4. Graph compatibility must include the effective static parameter set (or a
   verified state revision). The current graph-node compatibility key omits
   saved parameter values, so reuse would otherwise skip a changed value.
   Graph publication may happen only after both controller and processor
   initialization succeed; failures follow the model-versus-active graph
   status contract.
5. Add a deterministic fixture with a separate controller and processor.
   Assert matching values before the first audible block, zero callback
   allocations, state-over-value priority, unknown-ID retention, controller
   refusal, flush refusal, teardown, and unchanged saved data. Exercise 64-
   and 128-frame blocks under GCC, Clang, ASan and TSan.

This gate does not alter the project schema or claim general plugin
compatibility. The existing project binding remains unsupported until the
UI-thread and processing-thread owners can implement the full handoff.
