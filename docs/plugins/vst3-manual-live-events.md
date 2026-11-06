# VST3 manual event runtime primitive (PI-09)

Each verified effect or instrument graph node owns one
single-producer/single-consumer manual-event slot.
Admission is on the control side. It requires the exact engine instance ID and
compatibility key, a nonzero project revision, a finite normalized continuous
parameter, no active automation lane for that parameter, an already rendered
runtime, and a fixed-latency contract. The default contract is **unverified**,
so unqualified VST3 controls reject manual live changes. VST3 metadata and an
initial `getLatencySamples()` result do not prove that later parameter values
keep latency fixed. Passing `fixed_latency_verified=true` requires independent
evidence for every admitted parameter. ConnectedGain and TimedInstrument are
synthetic contract fixtures; the exact installed Surge XT Synth 1.3.4 binary
has scoped evidence for Filter 1 Cutoff at 48 kHz and 64/128-frame buffers.
An actual latency-changing control must retain the default closed gate.
GraphPublisher and RoutedMixerPublisher activate the slot only for their
callback-accepted graph revision and deactivate it at a switch. A held or
retired runtime cannot accept a new event. A control preflight checks the
same identity and admission conditions as submission, then submission repeats
them after the project commit. A switch in between is reported as saved but
not live; it cannot silently apply to the replacement.

The slot holds one event. A second submission returns `busy` until the
control owner collects the receipt. The audio callback claims the event once
at the beginning of a graph block, inserts its offset-zero point into the
first preallocated VST3 segment event buffer, and records `applied` only if
the full graph block succeeds. The receipt carries the exact project revision
and graph block start sample. It
reports host input-queue delivery, not the plugin's internal smoothing or
audible response. A failed process or retired runtime reports failure; stale
instance/key submissions cannot reach a replacement. Saved automation wins:
manual admission for an automated parameter is rejected. The verified bundle
host has 257 event places, leaving the existing 256-event automation budget
plus one manual place.

The verified instrument-source provider has the same bounded manual-event
slot, graph lifecycle, receipt and automation precedence. Its latency gate is
also closed by default. `Vst3ManualInstrumentEdit` supplies the matching
headless project transaction. It requires the exact saved instrument source,
an independent suspended clone capture, no automation lane, and a qualified
active slot. The shared effect/instrument transaction logic saves one revision
and one undo step before submitting; it reports saved-versus-live revisions and
receipt outcomes identically for both paths.

`Vst3ManualEffectEdit` is a headless transaction seam for one effect gesture.
Its caller must capture opaque component state and parameter references from
an independent suspended clone and supply the slot belonging to that exact
saved effect. It rejects stale editor snapshots, missing or disabled effects,
automation lanes, mismatched captured values, unavailable parameters, and
unverified latency before committing. A successful admission saves one project
revision and one undo step, then submits an offset-zero event to the exact
active graph identity. Status distinguishes model revision, durable saved
revision, pending host delivery, applied sample, and saved-but-failed live
delivery. A save error that left the in-memory model advanced is separately
reported as `model_not_saved`. A graph switch between preflight and submission
can leave the saved revision not live, with a refusal reason.

Routed control looks up exact saved effect and instrument slots in the
callback-accepted topology. The topology revision identifies the retained
runtime and slot; a manual event names a newer saved project revision.

`RoutedPlaybackControl` adopts one manual revision without rebuilding
topology. The transaction drives an adoption owner in three steps:

1. Before the save, `preflight` requires audible state to equal the latest
   saved revision, no pending graph, native or manual update, and the
   expected identity to name the plugin's slot in the active topology.
2. After the save and before submission, `begin` adopts the snapshot into
   the playback mirror. The control accepts it only if it differs from the
   audible snapshot in that plugin's component state and parameter
   references alone. Status is pending and other edits wait.
3. `settle` reports every terminal receipt once. An applied receipt advances
   the audible revision and the active saved plugin; runtime, slot, DSP
   history and latency are unchanged. A failed receipt, refused submission or
   stopped run keeps the prior audible revision and marks the saved revision
   failed. Saved and audible parameters now differ, so the plugin has no
   manual binding until a fresh preparation.

Undo, redo and native editor apply submit no event. They take the ordinary
graph path, which refuses changed VST3 state during playback, so they remain
saved but not live. Clip-source capture accepts the published snapshot when
it matches either the active topology or an acknowledged audible revision.

Quick's `RoutedPlayback` owns each gesture's transaction and is its adoption
owner, so the transaction cannot outlive the run. The generic editor
preflights before changing the clone. If live admission lacks qualification,
it saves the value and reports that it is not yet audible. A pending live
event refuses a competing gesture until its receipt. After an admitted
commit, the UI shows pending, then live with the receipt sample, or saved but
not live with the reason.
`Vst3TrackInsertProvider` now has the instrument provider's opt-in
`fixed_latency_verified` flag; it covers every prepared effect. Quick passes
scoped fixed-latency evidence by binary digest and, for the qualified Surge XT
instrument, parameter ID. All other real controls retain
`latency_unverified`. Effect and instrument editors can open while playback
runs. A verified continuous generic parameter or completed native gesture
saves one project revision during playback even when live admission fails; the
UI says that the saved value is not yet audible. A pending live event still
waits for its receipt, and a plugin restart request requires native Apply.
Only an admitted, callback-acknowledged event is labelled live. The installed
Surge scope and evidence are in [Surge XT qualification](surge-xt-live-latency-qualification.md).

The slot has one control producer; submission, receipt collection and
runtime reclamation are serialized on the control side. Callback work
allocates and locks nothing.
