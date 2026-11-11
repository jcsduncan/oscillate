# ADR-0127 — The master overlaps the tracks: each callback in parts, as a wavefront

**Status:** Accepted and implemented. **Date:** 11 November 2026.
**Builds on:** ADR-0048 (audio helpers), ADR-0050 (one chain per track), ADR-0085 (nested voice batches), ADR-0110 (heavy projects across the helpers), ADR-0125 (the master chain's serial cost).
**Supersedes in part:** ADR-0048's graph schedule (the sources' batch before the levels).
**Contract:** `RoutedMixer::mix_block`'s `RoutedNodeFill`, `ProcessorRuntime::renders_any_split`, `RoutedSubblocks`, `RoutedMixerLimits::subblocks` and `fixed_subblock_frames`, `SubblockPolicy` and its constants (`include/oscillate/engine/routed_mixer.hpp`); `ProcessorRuntime::stage_count` and `process_stage`, implemented by `track_chain::ChainRuntime`; `RoutedSourceFill::prepare_selected` and `fill_node`, `RoutedPrepareStatus` (`routed_publisher.hpp`); `RoutedAudioSource::prepare_source_bank` and `fill_node`; `AudioWorkerPool::help_nested`; the reference player's `--subblocks`; `qualify_reference_projects.py --subblocks --gate-exposed-percent --gate-late-blocks`. No schema change.

## Context

The tracks render in parallel on the helpers, but the master chain only starts once every track has finished the whole block, and runs on the callback while the helpers wait. ADR-0125 cut the chain from 5.4 to 2.56 ms per 1024-frame block and rejected rendering block n's master beside block n + 1's tracks, because that adds a block of output latency. On a heavy project the tracks fill most of the period, and the master's time still adds to every block.

The engine's processors render identically under any block split: every device has a split-invariance test, and the mixer's gains, ramps, delay lines and meters run frame by frame.

## Decision

1. **A block is cut into parts and scheduled as pieces.** A piece is one node's part of the block. A node's part k waits for part k of every node it reads (sends and sidechains included) and for its own part k − 1. So the master mixes part k while the tracks render part k + 1, with no added latency: the block still ends when its last part leaves the master.
2. **The master's stages pipeline.** A processor made of stages (`stage_count`, `process_stage`; the track chain is one) lets the mixer run each stage as a piece of its own: stage s of part k waits for stage s − 1 of part k and its own part k − 1. Different helpers run the master's devices on different parts at once. Only the master does this: every other node already runs beside the rest of its level. A master with a tail still draining (a live switch's replaced chain) keeps whole processing for that block, since its drain is summed after the processor.
3. **A ready queue, not barriers.** One pool batch serves the block. Each thread takes the next ready piece, the levels after the first ahead of the tracks; a finished piece releases the pieces waiting on it. A thread with nothing ready helps any nested voice batch (ADR-0085), spins 128 rounds, then sleeps on a futex, and each queued piece wakes one sleeper. Every part's piece writes only its node's frames of that part, so pieces of different parts never touch the same frames.
4. **Sources render inside the schedule.** On a block that switches nothing, the source decides whether the block renders (`prepare_source_bank`: the transport generation, a seek's hold, an accepted loop wrap) and then each node renders its own source's part just before mixing it (`fill_node`), instead of every source rendering before the graph starts. A switching block, and a staged candidate's exact fill, still fill whole before mixing.
5. **Whole blocks run level by level.** When a block is not split, its nodes run in dependency levels, each node filling its source first. A level of one node, normally the master, stays on the callback thread with its state in that core's cache; with the queue a helper took it, and the light reference's p99 rose 15%.
6. **The split is chosen by measurement.** `SubblockPolicy` keeps a running mean (over 16 blocks) of the levels after the first as a share of the block's render time: the longest node of each level, and the master's pieces summed. Below 5%, blocks stay whole. From 10%, the policy times the two ways against each other: 16 blocks of splitting after 16 blocks of warm-up, then 16 blocks of the other way every 1024 blocks. It splits only while splitting renders at least 10% faster per frame, so a project whose split costs more than it overlaps keeps whole blocks. A split block takes enough parts that the deeper levels' last part is about 2.5% of the block: between 2 and 8, each at least 64 frames. The policy carries over a live switch. `RoutedSubblocks::fixed` (a set part length) and `off` serve tests and benchmarks.
7. **A plugin keeps whole blocks.** `ProcessorRuntime::renders_any_split` says whether a processor renders the same under any split. The engine's processors do; a VST3 plugin host does not (its manual edits are qualified per process size, and a plugin need not be split-invariant). A node whose chain or output stage holds one, or which is still draining one it replaced, processes each block whole in its last part, once every input has finished, and what reads it waits for that; the rest of the graph still runs in parts. A master holding one does not pipeline its stages.
8. **A source that fails part way** leaves its block silent and played: the graph has moved past it, so the transport moves on rather than replaying a block the mixer would refuse.
9. **Export is unchanged.** Offline renders keep the block pipeline of `mix_step` (ADR-0048's export pipeline) or whole blocks; split invariance makes them equal to live playback.

### What is unchanged

- **Live edits** take effect at block boundaries as before: graph switches, overlays and live gains are all applied before the block's schedule starts.
- **Seeks and loops:** a part's context carries its own position; a planned loop wrap flags only the block's first part.
- **Meters** observe each part in order; a meter window still holds at least 1024 frames and may now close at a part's end.
- **No allocation on the callback:** every queue, counter and plan is sized when the mixer is prepared. `parts_never_allocate` counts every thread's allocations through split blocks.

## Measured

i7-10700K (8 cores, 16 threads), GCC 15.2, Release, seven helpers, paced fake callback, 48 kHz, 20 s a run; before is main at 2d0ba59. The heavy master is ADR-0125's (Temper, Ceiling, Gild, Lens, Keel, Shape, Loudness). The master's exposed time is how long, in each block, the master runs after every other node and source has finished: the serial tail that adds to the callback.

Callback p99 and whole-process CPU (% of one core), without the trace. The heavy reference holds a VST3 effect and a VST3 instrument, which render whole blocks (decision 7); its rows were measured back to back with main.

| Reference, master, frames | p99 before | p99 after | CPU before | CPU after |
| --- | ---: | ---: | ---: | ---: |
| light, heavy, 256 | 971 µs (18.2%) | 821 µs (15.4%) | 25.7 | 35.9 |
| light, heavy, 512 | 2405 µs (22.5%) | 1428 µs (13.4%) | 23.3 | 34.7 |
| light, heavy, 1024 | 3210 µs (15.0%) | 2301 µs (10.8%) | 21.4 | 30.6 |
| heavy, heavy, 256 | 2651 µs (49.7%) | 2249 µs (42.2%) | 180.0 | 179.8 |
| heavy, heavy, 512 | 5021 µs (47.1%) | 4167 µs (39.1%) | 180.1 | 198.1 |
| heavy, heavy, 1024 | 9715 µs (45.5%) | 6881 µs (32.3%) | 167.0 | 186.2 |
| light, default, 128 | 411 µs (15.4%) | 406 µs (15.2%) | 24.0 | 23.2 |
| light, default, 1024 | 2114 µs (9.9%) | 1679 µs (7.9%) | 16.3 | 21.9 |
| heavy, default, 128 | 1241 µs (46.5%) | 1211 µs (45.4%) | 184.9 | 197.6 |
| heavy, default, 1024 | 8518 µs (39.9%) | 6926 µs (32.5%) | 158.5 | 187.2 |

No block passed its period in any run, before or after. The master's time a block and its exposed time, with the trace:

| Reference, frames | Master before | Exposed before | Master after | Exposed after | Mean parts |
| --- | ---: | ---: | ---: | ---: | ---: |
| light, 256 | 694 µs | 694 µs (13.0%) | 816 µs | 444 µs (8.3%) | 3.96 |
| light, 512 | 1357 µs | 1357 µs (12.7%) | 1711 µs | 782 µs (7.3%) | 7.93 |
| light, 1024 | 2672 µs | 2672 µs (12.5%) | 3460 µs | 1609 µs (7.5%) | 7.89 |
| heavy, 256 | 706 µs | 706 µs (13.2%) | 827 µs | 430 µs (8.1%) | 3.97 |
| heavy, 512 | 1391 µs | 1391 µs (13.0%) | 1609 µs | 413 µs (3.9%) | 7.88 |
| heavy, 1024 | 2720 µs | 2720 µs (12.8%) | 3530 µs | 524 µs (2.5%) | 8.00 |

- **The light reference** leaves the master almost alone in each block, so its exposed time falls only by what its stages overlap with one another; the heavy reference hides most of its master behind the tracks.
- **The master costs more in parts:** 15–30% more in total, from each device's per-call work on shorter blocks. It no longer adds that time to the block.
- **The small project** (light reference, default master, 128 frames) costs the same CPU and p99.
- **Steps on the way:** level barriers per part (one batch per step) gained a third as much and cost more CPU in wake-ups; pieces taken in number order, each waiting for its inputs, held helpers idle and made the p99 worse than before. A whole block with the master on a helper, through the queue, was 15% slower than with the master on the callback.

## Consequences

- **More threads are busy for less time.** A split block wakes helpers for pieces that used to run one after another, so whole-process CPU rises where the policy splits (the light reference with the heavy master: from 21–26% to 30–36% of a core). Projects the policy leaves whole pay nothing; it splits only for a measured gain of at least 10%.
- **The trace has a span per part.** `graph.node` spans repeat for each part and each master stage, and `graph.wavefront` frames a split block with its part count. The qualification script sums the master's spans per block and reports its exposed time. The master gate (light reference, heavy master, 1024 frames) fails when the exposed time passes 8% of the period: on the four-core hosted runners it measured 5.6–6.7% split and about 13% whole. There the heavy reference fills the cores, and the policy keeps its blocks mostly whole (1.05–1.1 parts).
- **Device telemetry** is produced per part, as under any smaller device block.
