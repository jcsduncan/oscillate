# Verified VST3 effect graph node

**Scope:** PI-04/PI-08 and ER-01/ER-08, optional VST3 build. The graph node
supports the bounded single project-track insert path described below; it is
not an instrument host or general multi-insert chain.

The control side requires a fresh successful scan receipt. It validates the
saved class/vendor/version/binary hash, loads the scanned binary, restores the
bounded component state and activates the stereo 32-bit effect before producing
an engine node request. A missing, quarantined, mismatched or failed plugin
produces no request and preserves the saved placeholder. The composition owner
must report the binding diagnostic and leave the prior render graph active, or
make an explicit user-visible bypass decision; it must not mark a failed edit
audible. The saved advisory path is never loaded.

The composition owner assigns a unique nonzero 64-bit engine node ID for each
project plugin instance. It retains a `PreparedVst3GraphNode` receipt while its
runtime is live and reuses that request for a compatible topology-only revision.
The graph shares the exclusive runtime; the VST3 instance, its history and tail
therefore survive publication. A change to the saved class, binary, opaque
state, sample rate or maximum block size changes the compatibility key and
requires a fresh binding. The engine marks replacement as continuity-limited
and applies its current one-block output fade. This fade does not preserve a
reverb tail, and must be disclosed by the UI. A prepared factory is single-use;
reuse only works while the prior compatible graph owns the runtime.

The callback converts interleaved stereo engine frames into four preallocated
planar buffers, calls the host, and copies valid output back. A host process
error mutes the affected node, latches an atomic failure status, and leaves the
project's saved state intact. The control side polls that status and reports the
failure. It must never suspend, destroy, retry or serialize a plugin on the
callback. In-process fatal faults remain outside guaranteed containment.
The prepared receipt reports the host's initial latency to graph composition;
dynamic latency changes and parallel-path compensation require the routing/PDC
integration contract.

**Current integration limits:** The optional routed-live adapter now binds one
saved VST3 effect on one audio track and preserves it across compatible held
route edits, with initial latency fed to route PDC. The serial graph remains a
separate processing path. Multiple inserts, runtime replacement, dynamic
latency, automation events, UI attachment and offline VST3 export remain
separate composition work. Saved parameter
reference values are validated and reported by binding but are not yet applied
to the controller. The synthetic gain fixture proves verified binding, stateful
sample history across a graph publication, explicit replacement fade, missing
scan fallback, process-failure muting and callback allocation freedom. It is
not evidence of broad third-party compatibility or the 32-track release gate.

Local baseline (15 October 2026): Intel Core i7-10700K, GCC 15.2.0, Release
build, 48 kHz, synthetic stereo gain fixture, one source and one effect node,
50,000 measured blocks after 1,000 warm-up blocks. The complete serial graph
callback averaged 140 ns for 64 frames and 269 ns for 128 frames, with zero
observed allocations. This is a microbenchmark, not a plugin compatibility or
device-underrun result. Run it with `OSCILLATE_BENCHMARK_PLUGIN_GRAPH=1` on the
`oscillate_vst3_graph_node_tests` Release executable.
