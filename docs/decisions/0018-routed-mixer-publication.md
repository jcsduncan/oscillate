# ADR-0018 — Bounded routed mixer handoff

**Status:** Accepted; implemented: `RoutedMixerPublisher` (`src/engine/routed_publisher.cpp`) hands prepared routed mixers to `application::RoutedPlaybackControl`.
**Date:** 15 October 2026.  
**Requirements:** ER-01/03/06/08/09/10 and PI-08.  
**Affected interfaces:** `RoutedMixerPublisher`; no project schema change.

## Context

`RoutedMixer` owns prepared route buffers and delay histories. Preparing a
successor from an active mixer requires a consistent block-boundary history;
reading that history while its callback writes it is a data race. The existing
`GraphPublisher` publishes a serial node list and cannot carry per-track source
spans or routed compensation state.

## Decision

`RoutedMixerPublisher` uses one control producer and one callback consumer. The
control side requests a hold; the callback acknowledges at its next block
boundary, emits bounded silence and stops touching the old mixer. The control
side can then prepare a successor from the quiescent mixer and its exact
`BlockContext`. A complete plan is atomically offered. At a later block with
the **same project position and transport generation**, the callback swaps
the pointer and renders the new mixer. It moves the old pointer to a retirement
slot; the control side alone destroys it. Failure during preparation leaves
the old plan active; the control side cancels the hold and playback resumes at
the same project position.

The transport owner **must freeze the project cursor for every silent hold
block**. Device time can continue; those blocks represent an explicit audible
gap. If the cursor changes, the callback silences and returns `invalid_block`.
The routed application exposes the hold and failed revision/reason through its
revision-status contract. Some live edits still require Stop and Play, so this
contract does not claim seamless editing for every source or processor change.

### Exact zero-latency dry insert exception

An isolated first VST3 effect on a previously dry audio route may use a
prepared overlay Plan instead of rebuilding the mixer. Admission requires a
verified fixed zero-latency effect, unchanged source, routing, gains, tempo,
automation and master path, no sidechain input, and a settled active graph.
The new Plan shares the active `RoutedMixer` owner, including its edge delays,
source histories and surviving processor state. It names one formerly empty
processor slot and owns the prepared effect. At the next accepted callback
boundary, the callback selects the Plan and processes the effect before the
route's post-insert gain. It never processes the shared mixer concurrently,
copies mutable history or emits a held silence block for this change.
Surviving processors receive no deactivation or reactivation at the overlay
switch. The new effect alone receives activation, and it is deactivated on a
later graph switch or after callback join at stop. A later held graph rebuild
must explicitly retain the same effect owner; otherwise preparation fails
and the overlay stays audible. Replacement needs a separate tail and state
transition contract.

The engine now has a narrow no-hold removal companion for that exact active
dry insert. The control caller must independently prove zero remaining tail
and identify the active processor owner and node; the mixer verifies the same
empty, zero-latency slot underneath it. A pending or retired plan, hold, stale
revision or mismatched owner rejects the request. The callback fills the
unchanged source bank against the old revision, deactivates only the removed
processor, and renders the first dry block through the same mixer histories.
The removed owner stays in the retired Plan until control-side collection,
after callback activation. Application admission requires the exact live first
insert slot and runtime owner, an unchanged saved plugin instance, fixed
zero latency and independent fixed zero tail for that instance, and no later
manual or graph revision. The application keeps the saved removal pending
until the callback accepts the dry revision, then retires the old insert
bookkeeping. Missing evidence leaves the removal saved but not audible, with
the prior effect running until Stop and Play. A plugin tail declaration or
binary scan alone cannot satisfy the gate. Source-fill failure retains
the prior graph under the normal invalid-block contract.

Quick can now accept a one-shot post-insertion receipt on its control
sequencer, after the new instance ID and complete saved state exist and before
the saved revision is submitted. It binds the exact next revision, track,
slot, instance, binary, class, component and controller state, parameter
references and sample rate. Independent empty-manual-ID latency records for
both 64 and 128 frames may coexist with an exact zero-tail record; the
provider still checks the actual scanned binary and prepared runtime. A
different revision or saved state invalidates the offered evidence, and a
manual edit clears it. The receipt is consumed by the insert worker; the
callback never reads it. The synthetic LaneEffect test proves a qualified
first insert can then be removed live without a held block. No production
qualifier yet issues this receipt for a third-party binary, so unqualified
removal remains saved but inactive with the prior effect audible.

The dry graph keeps its 8192-frame backend callback cap because requested
PipeWire/JACK quantum is not proof of every actual callback size. The VST3
effect is prepared to its existing 128-frame host cap on a worker, then a
generic, allocation-free processor adapter splits a larger device block into
serial chunks with exact absolute sample positions and processing generation.
An event at a chunk boundary remains at the same project sample. A failed
scan, host setup, qualification or Plan offer leaves the saved edit visibly
inactive and the old graph playing. This exception makes no claim for
nonzero-latency inserts, sidechains, new routes, multiple effects or real
third-party binary qualification.

This first publication contract keeps the source span count and topological
order fixed across a switch. A plan that changes order is rejected before
publication because the callback cannot infer which unlabeled source span
belongs to which new node. A later named-source input contract can remove
that restriction. The routed mixer itself rejects a changed processor owner
on an existing route until a replacement/tail transition is designed. It
shares compatible processor owners and primes compatible PDC history while
held. A new compensated path without usable history and an over-budget path
both fail while the old graph remains active.

## Evidence and next integration seam

Deterministic fixtures cover stateful tail continuity, impulse alignment on a
latency increase at the switch, over-budget and changed-owner rejection,
source-order and cursor rejection, cancel/resume, retirement, callback
allocation freedom and concurrent control/callback handoff. GCC, Clang,
ASan/UBSan and TSan runs are recorded with the implementation result.

The application now maps saved routes to mixer nodes, freezes its transport
cursor during a hold and reports pending/active/failed graph status. The
remaining source-replacement contract is tracked in ADR-0024. Dynamic plugin
replacement and broader chain combinations remain separate work.

Local uninstrumented baseline (Intel Core i7-10700K, GCC 15.2.0 Release,
48 kHz, 32 stereo source routes, one 64-frame-latency path, 20,000 measured
blocks): two local runs placed both direct and published paths near 15.6–16.1
µs per 64-frame block and 30.4–30.8 µs per 128-frame block. Both paths
observed zero callback allocations. The measured publisher difference is
smaller than run-to-run noise. These are one-machine microbenchmark
measurements, not the 32-track plugin qualification or device-xrun result.

The 17 October cursor-preflight change checks a mixer's expected generation
and sample position before filling prefetched sources. A focused out-of-order
block fixture now proves zero source-frame consumption on rejection. On the
same i7-10700K with GCC 15.2 Release, one 20,000-block, 32-source run measured
15.40 µs direct and 15.56 µs published at 64 frames, and 30.06 µs direct and
30.05 µs published at 128 frames, with zero callback allocations. This single
run only bounds the observed steady-state cost on this machine; it does not
establish a speedup or a device underrun result.

For a last-stage native reverb decay change, the saved chain keeps shared
ownership of its unchanged upstream runtimes across the held graph switch.
The predecessor wrapper remains pinned, while an explicit selected drain
processor runs only the old reverb on zero input. The successor wrapper
processes the retained upstream stages and a freshly prepared reverb. The
application must prove the selected processor belongs to the predecessor,
that no later stage or sidechain path is present, and that the tail bound is
finite. The mixer rejects a selected drain processor that is itself an active
route owner. This narrow substage handoff does not imply arbitrary plugin
chain state transfer.

The same selected-stage rule permits a final master reverb decay edit after
unchanged zero-latency native EQ/compressor stages. The model edit must be
isolated to the master effect settings. A held switch shares the earlier
master runtimes with the successor wrapper and pins the old wrapper while
only the old reverb drains. Limiter latency, later stages and VST3 remain
outside this transition.
