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

i7-10700K (8 cores, 16 threads), GCC 15.2, Release, seven helpers, paced fake callback, 48 kHz, 20 s a run, on a shared desktop. Before is main at cacbd2c (ADR-0126's realtime FFT), measured back to back with this change. The heavy master is ADR-0125's (Temper, Ceiling, Gild, Lens, Keel, Shape, Loudness); the heavy reference also holds a VST3 effect and a VST3 instrument, which render whole blocks (decision 7).

Callback p99 (share of the period) and whole-process CPU (% of one core), without the trace:

| Reference, master, frames | p99 before | p99 after | CPU before | CPU after |
| --- | ---: | ---: | ---: | ---: |
| light, heavy, 256 | 989 µs (18.5%) | 592 µs (11.1%) | 22.3 | 31.5 |
| light, heavy, 512 | 1731 µs (16.2%) | 1003 µs (9.4%) | 19.4 | 29.7 |
| light, heavy, 1024 | 3216 µs (15.1%) | 1580 µs (7.4%) | 18.0 | 24.8 |
| heavy, heavy, 256 | 2658 µs (49.8%) | 1947 µs (36.5%) | 186.6 | 185.2 |
| heavy, heavy, 512 | 4611 µs (43.2%) | 3658 µs (34.3%) | 178.3 | 186.5 |
| heavy, heavy, 1024 | 8941 µs (41.9%) | 6715 µs (31.5%) | 165.8 | 176.7 |
| light, default, 128 | 529 µs (19.8%) | 522 µs (19.6%) | 24.5 | 23.3 |
| light, default, 1024 | 3046 µs (14.3%) | 1908 µs (8.9%) | 18.3 | 21.8 |
| heavy, default, 128 | 1543 µs (57.9%) | 1239 µs (46.5%) | 214.9 | 199.1 |
| heavy, default, 1024 | 10077 µs (47.2%) | 7199 µs (33.7%) | 190.6 | 189.9 |

No block passed its period in any run, before or after. The master's time a block and its exposed time (how long it runs after every other node and source of the block has finished: the serial tail that adds to the callback), with the trace. Whole blocks (`--subblocks off`) are the schedule before:

| Reference, frames | Master whole | Exposed whole | Master in parts | Exposed in parts | Mean parts |
| --- | ---: | ---: | ---: | ---: | ---: |
| light, 256 | 509 µs | 509 µs (9.5%) | 588 µs | 337 µs (6.3%) | 3.21 |
| light, 512 | 976 µs | 976 µs (9.2%) | 1253 µs | 455 µs (4.3%) | 7.93 |
| light, 1024 | 1955 µs | 1955 µs (9.2%) | 2479 µs | 947 µs (4.4%) | 8.00 |
| heavy, 256 | — | — | 549 µs | 331 µs (6.2%) | 2.65 |
| heavy, 512 | — | — | 1247 µs | 194 µs (1.8%) | 8.00 |
| heavy, 1024 | — | — | 2286 µs | 292 µs (1.4%) | 8.00 |

- **The light reference** leaves the master almost alone in each block, so its exposed time falls by what its stages overlap with one another; the heavy reference hides most of its master behind the tracks.
- **The master costs more in parts:** about a quarter more in total here, and nearly twice as much on the four-core hosted runners, from each device's per-call work on shorter blocks and its state moving between cores. Four parts instead of eight cost as much and hide less (at 1024 frames: 2403 µs, 1244 µs exposed, p99 2070 µs).
- **The small project** (light reference, default master, 128 frames) costs the same CPU and p99.
- **Steps on the way:** level barriers per part (one batch per step) gained a third as much and cost more CPU in wake-ups; pieces taken in number order, each waiting for its inputs, held helpers idle and made the p99 worse than before; a whole block with the master on a helper, through the queue, was 15% slower than with the master on the callback.

## Consequences

- **More threads are busy for less time.** A split block wakes helpers for pieces that used to run one after another, so whole-process CPU rises where the policy splits (the light reference with the heavy master: from 18–22% to 25–32% of a core). Projects the policy leaves whole pay nothing; it splits only for a measured gain of at least 10%.
- **The trace has a span per part.** `graph.node` spans repeat for each part and each master stage, and `graph.wavefront` frames a split block with its part count. The qualification script sums the master's spans per block and reports its exposed time.
- **The master gate measures two things.** The master's cost and the callback's p99 are gated in whole blocks (`--subblocks off`), as ADR-0126 set them. Played in parts, the light reference's master runs alone for 3.6–3.8% of the period at 512 and 1024 frames on the four-core hosted runners, against 6.9–7.7% whole; that exposed time is gated at 5%, with the same p99 limit. On those runners the heavy reference fills the cores and the policy keeps its blocks whole (1.0–1.05 parts).
- **Device telemetry** is produced per part, as under any smaller device block.
