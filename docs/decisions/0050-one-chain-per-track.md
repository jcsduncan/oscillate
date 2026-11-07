# ADR-0050 — One chain per track

**Status:** Accepted and implemented for live playback, routing and authoring. Export has two VST3 offline renderers with their own limits (`src/application/vst3/effect_export.cpp`, `instrument_export.cpp`). **Date:** 21 October 2026.

## Context

Live playback, routing and export each admitted only a few hand-written effect topologies:
- at most four VST3 instances in a project, three on a track, and two VST3 instrument tracks, which then needed matching chains;
- one native effect beside one VST3 effect, but no other mixture;
- no native and VST3 paths on different tracks at once;
- no VST3 effect on the master;
- a VST3 sidechain only when it was the track's sole effect.

Each shape had its own preparation code in `RoutedPlaybackControl::build_and_publish`, in `vst3_effect_offline` and in the routing registry, and each edit to a VST3 stage was a separate special case:
- a dry insert overlay;
- a held state replacement;
- a synthetic-latency transition.

None of these were limits of the engine. They were limits of the code, and they made every VST3 edit while playing take a silent hold or be refused. ADR-0049 made built-in chain edits seamless; this decision gives every chain the same treatment.

## Decision

### A track is a chain
A track's chain is:
- its VST3 instrument, when it has one;
- then each enabled effect in saved order, built-in or VST3.

The master's chain is its enabled effects. There are no shape limits; a chain may hold up to 64 stages (`ProjectRouteIdRegistry::max_stages_per_path`), which bounds its latency-handover history.

Built-in instruments and the sampler render in the source bank, as before, and are not chain stages. *(Superseded in part by [ADR-0063](0063-crate-chop-sampler.md): the sampler is removed; its successor, Crate, is a built-in instrument.)*

### One builder: `oscillate::track_chain`
- **`prepare_chain(saved, context, prior)`** prepares a chain:
  - Built-in stages come from `native_inserts::prepare_native_stage`.
  - VST3 stages come from a `Vst3Preparer` that the plugin host supplies, so neither the library nor export depends on VST3 code.
  - Given the running chain, it carries over every stage whose saved identity (slot and processor) is unchanged, and prepares only new or changed stages. A kept plugin is never re-instantiated.
- **`ChainRuntime`** runs the stages and keys the one a sidechain targets. It records each stage boundary's recent input, up to 8192 frames.
- **Priming at a live switch.** Every fresh stage is primed from the input it would have heard: its lookahead plus a 2048-frame margin for envelopes and tails, replayed at earlier positions. A replaced or added plugin therefore starts with no gap. A plugin that has already run never takes earlier positions again.
- **Exact time.** Time is kept exactly (ADR-0049) when one stage is added, removed or replaced and the stages that must be replayed accept it. A replacement must also hold no less latency than its predecessor. Otherwise the per-edge crossfade applies.
- **`switch_chain(prior, next)`** decides how the predecessor retires:
  - **Ringing removed stage:** the longest-ringing one drains on zero input beside the successor. This includes a lookahead stage, unless an exact handover already re-emits the samples it held.
  - **Otherwise:** the predecessor passes to the successor by a state handoff.
- **Keyed stages.** A kept stage that a sidechain now keys is prepared again if it was prepared without its detector, because a plugin activates its auxiliary input only at preparation.
- **Unknown tails.** A VST3 stage that cannot bound its tail rings out for a fixed time and fades out over its last `drain_fade_frames`, instead of stopping.
- **`receipts(track, chain)`** gives routing one `BoundStage` per prepared stage. Routing checks that the receipts match the saved chain exactly, sums each node's latency from them, and lets a sidechain key any stage that reads a detector.

Live track chains, the live master and offline native export use this builder; the VST3 offline renderers drive the same live control, so export and playback cannot drift apart.

### Plugins
`TrackInsertProvider::prepare` takes the snapshot it binds to, its tempo, automation and sidechains included. Every preparation is a new instance with its own graph ID, so a changed plugin's new instance runs beside the old one while the old one rings out.

A VST3 stage is prepared for blocks of at most `vst3_host_block_frames` (128). It is wrapped in `engine::ChunkedProcessorRuntime`, which also chunks the detector input, whenever the graph runs larger blocks; the graph no longer caps every block at 128 because a plugin is present.

`plugins::Vst3StageProvider` routes each stage to the instrument or effect host. Quick scans a plugin new to a run on a worker while the old graph plays, then begins the edit; a failed scan fails the edit with its diagnostic. Authoring (`create_vst3_instrument_track`, the insert service, the bridge and the browser) admits any instrument track and any chain.

### Every chain edit switches live
`RoutedPlaybackControl` keeps one active chain per track and one for the master. An edit re-prepares only the chains it changed, switches each with `switch_chain`, and publishes through `prepare_and_publish_live` (ADR-0049) while the old graph keeps playing. This covers:
- adding, removing, reordering, enabling or disabling a VST3 effect;
- loading a VST3 state;
- master VST3 effects;
- edits that span several tracks.

### Removed
- The routing shape limits and `ordered_mixed_stages`.
- In the controller: the mixed, serial and two-instrument special cases, `ActiveTrackInsert` path owners, the dry-insert overlay, and held VST3 state replacement.
- In the engine: `RoutedSyntheticSilentLatencyTransition`.

## Consequences

- **Unrestricted chains.** Any chain plays and edits live (export has its own limits, see the status); a VST3 plugin is never re-instantiated by an edit that does not change it.
- **Plugin loading.** A new VST3 instance is still prepared on the control thread, which delays the edit by the plugin's load time but never the audio. Moving this to a worker is a follow-up.
- **Not yet covered while playing:**
  - **Step 2 of the seamless programme:** VST3 instrument replacement, source and MIDI changes, and adding or removing tracks. These will switch at a block primed in advance, with voice continuity.
  - **VST3 automation lane edits:** these need an automation mailbox in the VST3 runtime.
  - **Convolution changes.**
- **Export.** The two VST3 offline renderers still keep single-path admission gates. They are to collapse into one generic renderer.
