# VST3 instrument block transport contract

## Manual continuous parameter event boundary

The routed instrument source owns the same single-entry, acknowledged
manual-parameter slot as a routed VST3 effect. The callback activates its
graph revision only at the accepted publication boundary and deactivates it
when the graph is retired or the host fails. Submission must match the current
graph revision, runtime instance ID and compatibility key. A topology-only
revision reusing the runtime invalidates a pending event for the prior graph.
The event reaches the host at sample offset zero of the first segment in the
next audio block; MIDI events retain their own fixed capacity and offsets.
The receipt names the saved edit revision and the first project sample of the
block. An automated parameter wins over manual admission.

## Automation event capacity

Instrument and effect runtimes size the host from one shared budget,
`vst3_automation_event_budget(lanes, max_block_frames)`: every automated lane
may change on every frame, plus one slot that always reserves manual-event
headroom. The provider counts the saved lanes before loading the plug-in and
passes the budget to the binding, so host queue preallocation, runtime buffer
reservation and admission agree. Loaded hosts never get less than
`default_vst3_parameter_events` (257), so static-value staging is unchanged.

`max_vst3_parameter_events` (4,096 points per process call) is the hard
ceiling; a project needing more fails preparation with `resource_exhausted`
(instrument) or `automation_unavailable` (effect), and the prior graph stays
active. This admits up to 31 fully changing lanes at 128 frames and 63 at 64
frames. The callback re-checks the reserved capacity before every point and
faults the route rather than reallocating, dropping a point or playing stale
values; the host independently rejects an oversized call with
`parameter_capacity_exceeded` before the plug-in runs. Repeated automation of
one parameter is rejected at preparation. At the ceiling a host reserves about
128 KiB of parameter queues and 64 KiB of editor-capture space, and each
runtime reserves 64 KiB of point storage, all at preparation.

The synthetic `LaneInstrument` fixture (the `TimedInstrument` source built
with 65 inert continuous parameters) logs every delivered point. At 64 and 128
frames it verifies 8 lanes and the maximum lane count, each with one point per
frame at its exact sample offset and ramp value, a manual event at sample
zero beside full automation with an applied receipt, explicit rejection one
lane past the ceiling, and zero callback allocations. This is a host contract,
not third-party plug-in compatibility evidence.

The generic provider defaults to `latency_unverified`: a caller must provide
independent evidence that this instrument cannot change latency in response
to the admitted parameter before enabling the slot. The slot is a host-side
primitive, not a ProjectSession command. Quick manual controls remain closed
until the saved edit and active-revision acknowledgement form one transaction.

`Vst3EffectHost::process_instrument` accepts one transport snapshot for every
audio block. The caller obtains `project_sample`, `quarter_position`, and
instantaneous `tempo_bpm` from the shared `timeline` module at the block start;
the plug-in host does no timing conversion. The host forwards them through
VST3 `ProcessContext` with valid tempo and musical-position flags and sets
`kPlaying` only when requested. Cycle, recording, time signature, system time,
and continuous time are not represented by this contract and are not marked
valid. The caller must split a block when it needs a different context within
that block.

The caller increments `generation` on every seek or other timeline
discontinuity. At the next process block, the host sends note-off events at
sample offset zero for all notes it has accepted and then sends the new block's
notes. A stopped block also flushes tracked notes and rejects new notes. The
release is also queued for the first block after suspension and reactivation,
even if the caller has not changed the generation. The caller must still
submit a stopped or discontinuity block before retiring a live host when it
needs its plug-in to receive note-offs.
This is a best-effort release of VST3 notes, not a reset of arbitrary plug-in DSP
state or reverb tails. A plug-in that ignores matching note-offs can still
misbehave and belongs in compatibility diagnostics. The ledger holds at most
the configured note-event capacity. If a flush plus new events would exceed
that capacity, the host rejects the block before calling the plug-in; the
caller can submit a release-only block before new notes. Event and ledger
buffers are reserved at host creation; the callback does not allocate.

The dynamic `TimedInstrument` fixture validates exact sample position,
quarter position, tempo, play state, a note sustained across blocks, release
on stop, and a seek release followed by an audible note in the same block at
64 and 128 frames. It also rejects nonfinite tempo, sample overflow, new notes
on a stopped block, and event-capacity overflow. This is a host contract test,
not evidence of compatibility with third-party instruments or a full MIDI
track workflow.

## Saved-project routed instrument slice

`Vst3InstrumentTrackProvider` binds one saved VST3 instrument on one instrument
track to the existing routed graph's verified-processor seam. Preparation uses
an out-of-process scan receipt and rechecks the binary, class, saved component
state, project sample rate and compiled MIDI. The advisory saved bundle path is
not loaded. The project keeps the exact plugin ID, state and MIDI through
save/reopen. A missing or changed receipt, unsupported parameter values,
another active instrument source fails before graph publication. Tempo events
are converted through the shared tempo map at preparation, and a callback block
crossing an event is split so each VST3 process call receives the correct
block-start tempo, quarter position, and locally rebased MIDI and automation
offsets. For gradual curves, the callback queries the shared tempo evaluator
at each process-call start. VST3 supplies one BPM per process call, so a plugin
may treat the interior of a block as constant tempo; this is a format-level
approximation, not sample-accurate tempo modulation inside that call. Live and
offline paths use the same rule. Events rounded to the same sample use the last
event's BPM. Only the 48 kHz, 64/128-frame synthetic
`TimedInstrument` fixture has been qualified. The fixture requires a 128-frame
host setup even when the callback renders 64 frames.

The live binding contracts carry a default-false `instrument_source` bit in
`PreparedTrackInsert` and `BoundTrackInsert`. Routed control sets the route token
only after a complete provider result, and both control and route planner reject
a token whose kind differs from the saved track. Existing audio effect providers
remain effect-only. This does not change the persisted project schema.

The provider uses the shared MIDI event compiler and tempo map. The callback
maps stable note IDs into bounded VST3 note IDs, forwards sample offsets and
transport generation, and uses preallocated buffers. A compatible held routing
edit keeps the same host and its held notes. A live MIDI or plugin-state edit
remains saved in the model but fails graph application until a newly prepared
session; the UI must show the model/active revision difference. After stopping
and joining the callback, the owner calls `stop_after_join()` to send note-offs
before retiring the host. A plugin process failure mutes the route and reports
a separate processor fault.

A planned, primed loop wrap advances the transport generation, so the host
releases notes held from the prior traversal at offset zero. The graph retains
the plugin instance and its processing history while the new traversal's MIDI
events enter at their normal offsets. The 64/128-frame routed fixture checks
the old note's finite release tail before the next note onset and zero callback
allocations. This is a synthetic host contract; it does not qualify arbitrary
third-party sustain or loop behaviour.

The bounded offline exporter creates a separate host in VST3 offline mode,
including its saved-state tail probe. Live playback keeps real-time mode.
Neither path may substitute a built-in instrument or a silent source for saved
plugin intent. No third-party instrument compatibility is claimed.

On an Intel Core i7-10700K, GCC 15.2 `Release`, a 10,000-block uninstrumented
routed callback loop at 48 kHz measured 1,250 ns/block at 64 frames and
2,204 ns/block at 128 frames, with zero observed allocations. The workload has
one saved synthetic instrument track and its fixed MIDI clip; after the note
ends, later blocks still run the VST3 processor with no note events. These
figures are a local baseline, not a 32-track or device-underrun qualification.

The 18 October 2026 Release fixture on the same i7-10700K and GCC 15.2.0
measured 1,364 ns/block at 64 frames and 2,463 ns/block at 128 frames after
stepped-tempo support, with zero observed allocations. The older numbers also
precede other host and automation changes, so they are historical context,
not an isolated tempo-splitting regression measurement. A direct constant-map
branch measured 1,331/2,393 ns/block in one run; it was not retained because
the improvement was small relative to measurement variability.

On an Intel Core i7-10700K, GCC 15.2 `Release`, the fixture's steady stopped
transport loop at 48 kHz took 124 ns/block for 64 frames and 177 ns/block for
128 frames over 10,000 calls, with zero observed host allocations. This is a
single source-only plug-in and an unloaded machine snapshot, not a production
graph latency or underrun qualification. Sanitizer and coverage results are
used only for correctness, not these timings.
