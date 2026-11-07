# ADR-0017 proposal — Live VST3 track composition

**Status:** Superseded by [ADR-0050](0050-one-chain-per-track.md): a track's chain holds any mix of built-in and VST3 stages.
**Requirements:** PI-04, PI-08, ER-01, ER-02, ER-03, ER-08, ER-09, ER-10.  
**Decision owners:** project schema, engine graph/publication, application playback, plugin host.

## Conflict found

The saved project model has `Track::vst3_plugins` and no master VST3 chain.
`GraphControl` currently appends all processor requests after the summed clip
and sampler sources. Appending a saved track plugin there would apply it to
every track, alter its chain position and make the active revision claim false.

`RoutedMixer` can run a processor on an individual route and account for its
initial latency, but it is not currently connected to `GraphPublisher`, the
live playback output or project track routes. Its transition preparation
requires the prior mixer to be quiescent at an exact sample boundary and
rejects a changed processor owner on an existing route. There is no safe
mechanism to publish a changed routed mixer while live playback continues.

The existing `oscillate::vst3_graph_node` already supplies the narrow plugin
adapter: verified scan/binary binding, restored opaque state, a preallocated
runtime request, initial latency, an atomic process-failure status and a stable
instance/replacement contract. A second application wrapper that only forwards
those fields would add no behaviour or useful coverage before route ownership
and publication are defined.

## Proposed integration contract

1. Define durable route and insert ownership. Each saved track plugin occupies
   a specific insert position after that track's source/instrument and before
   its outgoing route edges. Add a separately persisted master chain only if
   the product needs master VST3 inserts; give it commands, undo, serialization
   and migration tests. Do not infer a master plugin from a track placeholder.
2. The application composition owner maps every full 128-bit project plugin ID
   to a unique nonzero engine node ID without truncation or collision with
   source, sampler or native processor IDs. It keeps that mapping stable across
   unrelated revisions and reorders. The route graph stores track ID, insert
   index, plugin ID, scan/binding status and current latency separately from
   the saved placeholder.
3. On the control/worker side, require a fresh successful scan receipt for a
   new or replaced instance. Call `prepare_vst3_graph_node`; use its node
   request only if ready. A compatible topology edit reuses the same runtime
   owner and status object. Changed opaque state/binary/configuration prepares
   a replacement and discloses the existing one-block continuity fallback.
   Saved parameter-reference values remain diagnostic until controller restore
   is implemented.
4. Publish one prepared routed plan at a callback block boundary. The callback
   must never wait for a mixer to quiesce, load a module, allocate a delay
   line, serialize state, or destroy a retired runtime. The control side needs
   a quiescence/handoff protocol for the prior routed mixer, processor reuse,
   compensation history and retirement. A failed build or unprimeable delay
   leaves the old active graph intact. The UI shows model revision, active
   revision, failed plugin/route and reason; export cannot silently use the old
   graph for the new project revision.
5. Treat an initial or changed plugin latency as a route/PDC input. Do not
   publish a known-misaligned parallel path. A dynamic latency report needs a
   measured compensation budget and a hold/bypass/mute transition with a
   visible status before activation. A process error mutes only the affected
   route or defined fallback path and preserves the saved plugin identity and
   state. In-process fatal faults cannot be guaranteed recoverable.

## Acceptance evidence before claiming live VST3 inserts

- Two distinct tracks, one with the synthetic gain effect: only its audio is
  processed. Effect order and saved IDs survive save/reopen and track reorder.
- A topology-only edit preserves the fixture's sample history and tail. A
  state/binary change reports the bounded replacement transition and does not
  process one instance concurrently from two graph generations.
- Missing, quarantined, changed-binary and state-rejected fixtures preserve
  the model revision and placeholder, report pending/failed application, and
  keep prior valid playback. Retry or undo has deterministic behaviour.
- Impulse tests cover the plugin's initial and changed latency on two parallel
  routes before, at and after publication. Over-budget and unavailable-history
  paths fail without a misaligned mix.
- Callback allocation/lock probes, ASan/UBSan and TSan graph-swap stress pass;
  a 48 kHz, 64/128-frame uninstrumented benchmark reports CPU and allocations.
  Plugin compatibility remains limited to tested versions, and synthetic
  fixtures do not establish third-party compatibility.

**Next decision:** extend route ownership and transitions for multiple inserts,
runtime replacement, master inserts and dynamic latency only after the narrow
contract below has production evidence.

## Bounded implementation decision (15 October 2026)

ADR-0018's held routed-mixer publication now supplies the required quiescent
handoff for the initial **single saved VST3 effect insert on one audio track per project**.
The insert remains in `Track::vst3_plugins[0]`, after that track's audio source
and before its main/send edges. Routing compilation fails closed unless the
application supplies a matching, verified bound receipt and its initial
latency. The first effect occupies high-half processor ID `2^63`; route nodes
and edges stay in the low half. The per-session provider reserves a second
distinct ID for another saved effect; live playback initially composed
only one insert.

The optional VST3 adapter consumes the verified graph-node factory once on the
control side. It retains the resulting shared runtime and atomic failure probe
through compatible route edits. The publisher's held handoff prevents two graph
generations from processing that instance at once. Initial latency enters the
same route PDC planner as parallel paths. Missing or rejected scans leave the
saved placeholder intact and the new model revision failed/pending separately
from the prior active graph. A plugin process error mutes only its track and is
reported through a separate `processor_fault()` diagnostic; it does not rewrite
the active graph revision.

Changing/removing the insert, its opaque state, binary identity, placement or
format while playback runs is rejected as a failed saved revision; restart is
required to create a replacement. Same-track multi-insert chains, bus inserts,
simultaneous instrument/effect composition, dynamic latency, tail-preserving
replacement, parameter events, UI wiring and
offline VST3 export remain open. This narrow decision does not satisfy those
broader PI-02/03/08 acceptance gates or establish third-party compatibility.

## Two-effect routing prerequisite (15 October 2026)

The route compiler has a separate bounded two-receipt entry point for saved
audio effects on different tracks. It assigns each verified fixed initial
latency to its own track node, validates all bindings before committing route
IDs, and runs the existing parallel-path PDC planner. The legacy one-insert
entry point remains single-insert. At this prerequisite stage the audible live
graph also remained single-insert. The VST3 provider assigns unique high-half
graph-instance IDs per full saved plugin ID within one project session. The
subsequent live composition slice owns and reports both runtime/failure statuses.
Focused routing tests use an independent 7/11-frame parallel-path oracle and
cover receipt mismatch, missing/orphan/duplicate bindings, instrument mixing,
over-budget PDC, route-ID stability and the unchanged single-insert entry
point. GCC, Clang, ASan/UBSan and TSan pass; the route compiler has 121/124
covered executable lines (97%). No audio callback or plugin instance is
created by this prerequisite.

## Provider identity prerequisite (15 October 2026)

`Vst3TrackInsertProvider` allocates at most two high-half graph-instance IDs
within one open-project session, keyed by the full saved plugin ID. Distinct
IDs receive `2^63` and `2^63 + 1` in successful preparation order; a repeat
preparation of the same saved ID retains its assignment even if saved settings
change. A failed scan, bind or runtime preparation does not consume an ID. A
third distinct effect request fails with a two-ID resource diagnostic; the
first two assignments remain intact. An allocator collision or exhausted
numeric range fails explicitly. The provider exposes a control-side identity
lookup, while the callback only uses prepared runtimes. Duplicate saved plugin
IDs across tracks remain a route-compiler conflict, not a request for two
instances under one identity. By itself this identity prerequisite did not
connect two effects to the audible live graph, add per-track fault status, or
qualify third-party VST3 plugins.
Focused GCC/Clang Release and Debug ASan/UBSan tests pass with the synthetic
gain effect; provider coverage is 42/48 executable lines (87%). The uncovered
lines are defensive impossible-state, consumed-factory and allocation-failure
branches. Callback processing of both prepared runtimes performs zero allocations.

## Bounded two-effect live composition (15 October 2026)

Headless `RoutedPlaybackControl` now binds two already-saved, verified effects
on separate audio tracks, or retains the legacy single instrument. It owns a
maximum of two `(track ID, full saved plugin, runtime, fault probe)` records.
Initial publication commits those records only after the complete graph is
prepared and accepted. A failed second preparation leaves no active graph or
control-owned processor. Compatible held edits reuse the exact runtime on
each track; the callback hold prevents concurrent processing by old and new
graphs. Route PDC uses each effect's fixed initial latency.

Adding, removing, moving or changing a saved effect while playing requires a
restart. The saved model edit remains visible as failed-to-apply while the
previous graph and its processor owners remain active. The existing
`processor_fault()` summary reports the first active fault; a track-specific
query identifies the affected effect without changing graph revision status.
The two-effect fixtures check 7/11-frame impulse alignment at 64/128-frame
blocks, unchanged processor histories after a held route edit, rejected
changes and removal, independent process failure, and zero observed C++
callback allocations. Focused tests cover 64/68 changed executable control
lines (94%); the whole routed control file reaches 313/356 (88%) with the
related routed-source and instrument suites. A separate scanned synthetic VST3
fixture proves that two saved instances with distinct state process separate
live audio tracks. This does
not qualify third-party plugins, UI or device wiring, dynamic latency or
offline VST3 export.

## Bounded serial path on one audio track (15 October 2026)

The routed live compiler now also accepts two enabled, verified VST3 effects
on the same audio track. It processes them serially in saved slot order through
one off-callback composite runtime. Each stage is bound by its exact saved
`InsertSlotId`, plugin identity and full state; its declared latency contributes
to the path's checked total before route PDC is prepared. A compatible held
routing edit reuses the same composite and stage owners. A live reorder,
state change, bypass, missing binding or unsupported bus/master chain fails
before publication, leaving the previous audible graph active and the failed
model revision visible. A fault in either stage is reported for that track.

Synthetic scanner-backed save/reopen fixtures exercise distinct plugin state,
order, 64/128-frame processing and the failed-transition contract. Focused
GCC, Clang, ASan/UBSan and TSan routing/application tests pass. A local Release
fixture on an i7-10700K at 48 kHz measured median 1.31 µs per 64-frame block
and 2.72 µs per 128-frame block for two serial synthetic gain effects with a
wet/dry route, versus 1.27 µs and 2.47 µs for one effect. Each figure is the
median of three 20,000-block runs after 1,000 warmup blocks; the fixture
observed zero C++ callback allocations. These are local baselines, not the
32-track performance qualification or third-party plugin compatibility.
