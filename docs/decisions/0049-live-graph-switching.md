# ADR-0049 — Live graph switching and latency handover

**Status:** Accepted and implemented; ADR-0050 extends it to every chain, VST3 included. **Date:** 21 October 2026.

## Context

An edit made while playing used to switch graphs through a callback hold:
- the callback froze the transport;
- the control side rebuilt the graph against the quiesced state;
- playback resumed.

Each hold emitted five to eight silent device blocks. Edits that changed a path's latency were refused until the next Play, because the new graph had no way to continue the old graph's time. Ableton Live, Logic and Bitwig switch such edits while playing without a gap. Playback must be seamless here too: an edit never inserts silence, and it never changes the timing of anything it did not touch.

## Decision

### Hold-free switching
`RoutedGraphPublisher::prepare_and_publish_live` publishes a plan the callback adopts at the next block, with no hold:
- **Preparation.** The control side prepares the successor with `prepare_routed_mixer_live`, reading only the running mixer's immutable configuration. Every ring, history and drain list is allocated there, sized for the worst case.
- **Adoption.** At the switching block, the callback first fills the candidate's sources. It then calls `RoutedMixer::adopt_live_state(prior)`, which copies each surviving node's and edge's running state into the preallocated successor: delay rings, source and processor input histories, gain ramps, the transport position and the drain lists. It allocates nothing and cannot fail after it begins; `continues(prior)` checks the precondition first.
- **Commit.** The old plan retires through the usual epoch.
- **Withdrawal.** A candidate that can never be adopted (its `max_frames` below every device block, or a source bank that never fills) is withdrawn with `withdraw_pending`. The callback answers at the start of its next block, before touching the candidate, by moving it to the retired slot; `collect_retired` then frees it. `has_pending` stays true until the callback answers, so nothing new is published meanwhile.

`RoutedPlaybackControl::begin_current` publishes live whenever the edit keeps the same tracks, sources, VST3 inserts, convolution slots and VST3 automation lanes. Anything else takes the held path until it gains its own seamless transition.

### Built-in chains switch stage by stage
`switch_native_chain` builds the successor chain from the freshly prepared one, carrying over every running stage whose saved slot is unchanged, so a filter, envelope or reverb keeps its state through an edit to its neighbours. Removed stages retire as follows:
- **Ringing removed stage:** the longest-ringing one drains on zero input beside the successor. Drains are a per-node list, up to `max_tail_drains_per_node` of them, so successive edits overlap instead of being refused.
- **Lookahead removed stage:** it hands its held samples on through the latency handover below.
- **Otherwise:** the old chain passes to the successor by a state handoff.

Any number of a track's slots may change in one edit. An edit spanning several chains, or a chain with a VST3 effect, still defers.

### Exact latency handover
When one node's latency changes by Δ, the downstream compensation changes by −Δ on that node's outgoing edges only, so the rest of the graph keeps its timing. The handover is exact (the output equals never having switched, sample for sample) when:
1. the master output latency is unchanged;
2. the node's successor continues its predecessor, through `continues_latency_of`, or the removed processor releases what it held, through `releasable_latency`;
3. every outgoing edge's compensation changes by exactly −Δ.

At adoption:
- **Added latency (Δ > 0).** The new processor is primed by `continue_latency_of`. A built-in chain replays each stage boundary's recorded input, up to `boundary_history_frames`, through the added stage, so its lookahead holds the samples it emits first. The node's outgoing delays then forget their newest Δ frames, which the node will emit again.
- **Removed latency (Δ < 0).** The removed stage's held input continues through the stages after it, through `continue_latency_of`, or returns dry, through `release_latency`. Those frames pass the node's level and are appended to the outgoing delays (`DelayCompensationRuntime::shift`) and to the node's history, so they play next.

The engine test `live_latency_changes_keep_time` checks that switches of 0→8 and 8→4 frames render exactly 3·x(t−16) throughout.

### When time cannot be kept
When the output latency itself changes, or a handover is not exact, nothing shifts. Each edge whose compensation changed crossfades from its old reading to its new one over `DelayCompensationRuntime::crossfade_frames` (256). Unchanged edges keep reading exactly. A continuing processor is still primed, so a new lookahead never starts empty. A steady signal therefore holds its level through the switch: there is no global fade, dip or silent block, and the test checks this to 1e-5.

## Consequences

- **Gap-free edits.** Adding, removing, reordering or retuning built-in effects, including lookahead stages, is gap-free while playing. Latency changes no longer wait for the next Play.
- **Memory.** Each built-in chain keeps 8192 frames of history per stage boundary, and processor input histories are sized to `max_processor_input_history_frames`.
- **The held path remains only for:** source edits outside ADR-0024's no-hold cases. ADR-0050 made VST3 and multi-chain edits live, and convolution changes are refused at once without a hold. Each remaining case is to become a live transition. The held path must not grow new uses.
- **Test knob.** `RoutedPlaybackControl::set_live_switching(false)` exists only so held-path tests can force a hold for edits that would otherwise switch live.
