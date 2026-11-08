# ADR-0005 — Delay-compensation transitions

**Status:** Accepted; implemented for the bounded routed graph. Delay histories and transitions are in `src/engine/delay_compensation.cpp` and `routing_latency.cpp`. The numerical PDC budget remains a G7 qualification item. Superseded in part by [ADR-0101](0101-vst3-live-edits.md), for VST3 latency-changing transitions.
**Date:** 14 October 2026
**Requirements:** PRD §§7, 15, 17; FS-02 ER-03/ER-10.

## Context

Longest-path PDC defines steady-state alignment, but a plugin latency change or graph switch can reset delay history, truncate tails or briefly misalign parallel paths. A numerical compensation limit is still needed from reference-workload evidence.

## Decision

Each graph plan contains a latency ledger per path and preallocated compensation lines. A stable route/path ID permits compatible delay history to carry across revisions. Active paths keep a bounded, preallocated input-history ring sized to the approved compensation limit. The control side takes a consistent, nonblocking history snapshot and primes a changed line off the callback before activation. If a usable snapshot is unavailable, the old graph remains active while preparation retries, or the affected path uses the documented bounded fade/mute fallback. Publication requires an alignment-ready transition manifest; the callback performs only bounded pointer/state selection at a block boundary. Retired delay history remains alive while any tail-drain or active path references it.

When a plugin reports a latency change, the builder prepares a new plan. Until ready, the affected path remains on a safe previous configuration where possible, or is visibly bypassed/muted. If priming/history transfer is impossible, a bounded fade/mute fallback is recorded and reported. A path exceeding the measured compensation budget is rejected or disabled with the path and limit shown; known-misaligned audio is not silently mixed.

The default maximum compensation time and memory budget are determined by a benchmark using the reference plugin set at 48 kHz and 64/128 samples. The engine contract records the chosen values, latency-report timing assumptions and fallback duration before PDC merges.

The bounded offline VST3 effect-export slice uses an interim 8,192-frame cap
for the sum of verified serial plugin latencies. It prepares only the reported
amount, subject also to its 512 KiB route-history and source-history budgets.
This cap is an experimental admission bound, not the launch PDC limit. A
larger reported latency fails before opening an output writer.

The live Quick route currently uses the same 8,192-frame admission cap at
48 kHz. An [isolated PDC history sweep](../performance/pdc-budget-development-sweep.md)
supports retaining it as a provisional development default. It does not
qualify a product-wide limit or replace the physical mixed-plugin G7 run.

### VST3 latency-changing transitions (superseded by ADR-0101)

The existing routed mixer deliberately rejects a replacement processor whose
reported latency differs from its predecessor; such an edit is saved but not
made audible, and the prior graph keeps rendering. Its same-owner synthetic
latency probe does not authorise a VST3 replacement or a dynamic
plugin-latency claim. Before that guard changes, a changed-owner transition
must satisfy this contract:

1. **Exclusive owners.** The audible instance, its delay state and its latency
   stay fixed; a distinct candidate is built and validated off the callback
   from the exact saved revision. A `kLatencyChanged` restart or a failed
   latency re-query rejects the candidate.
2. **Exact history.** At a callback-acknowledged hold, the candidate is primed
   off the callback from a bounded, generation-tagged **pre-processor input**
   history of the affected node. Insufficient or stale history fails the
   publication and leaves the old graph and PDC intact.
3. **Output phase and tails.** The first admission requires a proven
   silence-safe boundary (quiet input and output covering the larger latency
   and the predecessor's whole finite tail). Audible transitions need an
   explicit fade or time-alignment policy and a drain plan for the predecessor.
4. **Atomic publication.** The new route/PDC plan is prepared against one held
   block context and selected at a block boundary without allocation; the
   predecessor retires on the control side after the switch.
5. **Failure.** Any failed check leaves the old path and PDC audible; an
   untrustworthy old instance is muted with an explicit reason.

As built, only the engine primitive exists: a synthetic fixed-delay owner can
switch at a wholly quiet boundary (7↔12 frames, 64- and 128-frame callbacks,
zero callback allocations), the application passes it through only for
runtimes implementing `SyntheticSilenceSafeLatencyRuntime`, and graph
preparation re-queries a candidate VST3's latency. No production VST3 runtime
implements the marker, so unequal-latency VST3 edits remain saved-only.

## Alternatives considered

- Reset delay history on every graph switch: simple, but causes discontinuities and temporary misalignment across parallel paths.
- Change delay lengths directly in the callback: avoids preparation latency but risks allocation or unbounded copying at the deadline.
- Continue mixing known-misaligned paths until recalculation completes: preserves sound but violates PDC correctness without warning.

## Consequences and evidence

Graph changes can wait for priming, so model and playback revisions may temporarily differ. Impulse tests measure parallel-path offsets immediately before, at and after transitions. Tests cover growth/shrink, bypass, removal, send/sidechain paths, over-budget latency, rapid successive changes, retained tails and safe reclamation.
