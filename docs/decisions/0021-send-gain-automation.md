# ADR-0021 — Stable send-gain automation binding

**Status:** Implemented; focused live/offline qualification complete
**Date:** 15 October 2026  
**Requirements:** PRD §14, FS-06 AM-01/03/07, ADR-0019.

The existing persisted send has a stable `SendId` and physical linear gain in
`[0,4]`. Schema 1.11 appends `SendGainTarget{SendId}` to the tagged automation
target set, encoded as `{"kind":"send_gain","send_id":"…"}`. A lane must be owned
by the send's source track. Its saved descriptor uses physical representation,
semantic key `mixer.send_gain`, range `[0,4]`, and continuous values. The saved
descriptor default remains historical metadata; the current persisted send gain
is the unautomated fallback. Old manifests through 1.10 remain readable, and
1.10 cannot claim the new target kind.

Track lanes and audio/MIDI clip envelopes use the shared prepared evaluator.
Audio clip envelopes are accepted only for the backed send-gain target at this
stage. Their half-open clip interval and clip-local coordinates follow ADR-0019;
the active clip replaces the track curve and the track curve resumes at release.
Removing a referenced send is rejected until its lanes are removed in the same
explicit project operation. No silent retargeting or lane deletion occurs.

The routed mixer accepts optional per-sample gain spans aligned to its stable
edge-ID plan order. It rejects a wrong ID, size, nonfinite or out-of-range value
before moving its cursor or delay history. An empty span uses the saved static
gain. A published live plan must own its matching prepared automation and
preallocated gain buffers; offline rendering must use the same evaluator and
edge mapping. This contract adds no allocation, file access or lock to the
callback. Full multi-track headroom remains subject to qualification.

## Consumer contract and qualification

The live routed publication plan owns its gain evaluator and scratch blocks for
exactly one project revision. It evaluates the full shared tempo tick block
before filling source audio, then supplies physical per-sample gains in stable
compiled edge order. A failed gain block is silent and does not advance source
or mixer history. At a graph hold boundary, the pending revision's evaluator
is used only if its graph can be published; the previous revision resumes on
failure. Send automation alone may change while a routed source remains active.
Non-send saved automation remains an explicit unsupported live edit.

Offline master and processed stems use the same binding and evaluator on the
absolute project-sample grid. Each export has separate preallocated state, so
solo stem rendering does not alter timing or precedence. The adapter allows at
most 8192 frames per block and 16 MiB of double/float gain scratch; excess
bindings fail during preparation. The gain callback does not allocate, block,
or inspect mutable project state. Multi-send CPU cost and the 32-track reference
workload still require qualification before a broad performance claim.

Local Release baseline on an Intel i7-10700K, GCC 15.2, 48 kHz, 32 automated
send lanes (gain preparation only): 10.8 µs per 64-frame block and 21.7 µs
per 128-frame block over 20,000 iterations. The benchmark excludes mixer,
source decode, plugin processing and device overhead, so it is a component
baseline rather than the 32-track release qualification.
