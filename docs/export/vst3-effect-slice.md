# Bounded offline VST3 audio-effect export

**Status:** Experimental vertical slice, 15 October 2026.

`render_vst3_effect_wav_master` and `render_vst3_effect_wav_stem` accept a
stopped, revision-owned project snapshot. The Qt Quick Export Master action
selects this path when the project has a saved VST3 audio effect. Export runs
on a worker and reports errors without replacing an existing valid output.

The slice accepts at most two audio tracks with bounded, unwarped managed WAV
clips and one or two enabled serial VST3 effect slots on one audio track, or
one saved bypassed VST3 effect slot. The bypassed slot retains its identity,
parameters and opaque state, but no processor is loaded and it contributes no
latency or tail. The bypassed master and stem follow the same dry route and
source timing as a project without that effect. Enabling or bypassing a slot
during active live playback fails graph publication with a saved/active
revision diagnostic; restart prepares the changed graph. For enabled slots,
the exporter verifies each saved bundle path, binary hash, class metadata,
plugin state and exact slot identity against fresh scans. It uses the routed graph for main/send
routing, supported send-gain automation and delay compensation. It adds the
effects' finite reported tails, with a combined ten-second cap, to the aligned output.
The prepared compensation is the sum of the verified serial effects' reported
latencies, capped at 8,192 frames and at 512 KiB each for route and source
history. Larger or over-budget graphs fail before opening an output writer.
Each effect receives the project sample, musical-quarter position, tempo and
sample rate in VST3 `ProcessContext`. Export configures the component and
processor for VST3 offline processing before rendering. The current graph
supports bounded stepped and gradual tempo events for VST3 effects; blocks
crossing a tempo event are split before processing. Gradual BPM is sampled at
each plugin call start from the shared timeline; VST3 receives one BPM per call.
The renderer primes the graph through its reported latency and discards those
leading frames, so the WAV starts at project sample zero and its duration is
the source endpoint plus the supported tail. The synthetic gain fixture now
delays audio by the seven or 96 samples it reports; an independent native
export checks that the first audible project sample stays aligned.
Master and single-source processed stem APIs use the same timing and routing
contract. Qt Quick exposes the master action; stem API integration is pending.

Unsupported instrument, master/bus insert, third effect, mixed native/VST3 chain,
multi-slot chain containing a bypassed insert, non-send automation,
stretched/warped media, encoded source, changed binary and unsupported output
format are reported before opening a writer. The master is written as WAV
float32/PCM24/PCM16, FLAC or MP3 at a selected output rate through the shared
[VST3 output-format stage](vst3-output-formats.md); the plugins still run at
the project rate. A failed runtime render leaves the previously published
output intact. This slice does not imply general VST3 effect compatibility or
arbitrary chain support. A failure in either serial processor aborts the staged
render and preserves the previously published output.

The source-level export fixture covers save/reopen, a bypassed dry-master
comparison with the independent native exporter, bypassed stem samples,
64/128-frame crossings, seven- and 96-frame PDC, master and processed stems, send automation precedence,
repeat render, a positive eight-frame tail, rejected shapes and interrupted
output. The serial fixture verifies two distinct saved states, serial gain,
summed latency, processed-stem summation and failure of the second processor.
`application_vst3_export_parity` holds the effect and instrument exports to
live routed playback: a VST3 effect's master and both stems, its finite tail,
a 96-frame latent plugin at 64- and 128-frame blocks, and a VST3 instrument's
master and stem all null against the live stream with its latency trimmed.
The Quick fixture reopens a saved VST3 audio effect and verifies the UI
worker dispatch and project-aligned WAV duration. GCC, Clang,
ASan/UBSan, format and tidy checks passed on the isolated export branch; the
integrated scanner-enabled Quick suite passed 82/82. These are local fixtures,
not the reference 32-track workload or third-party plugin qualification.

Saved normalized values for known VST3 effect parameters can initialize an
effect without an opaque component-state blob. Binding stages them before
activation and flushes them before the first audible block. The bounded offline
master and stem exporters use the same binding rule during tail/latency probing
and graph preparation. Unknown references reject export before publication.
The connected-gain fixture checks two values against processed output and
rejects an unknown ID. This is static initialization, not automation playback.
