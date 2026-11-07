# PDC history budget development sweep, 18 October 2026

This is an isolated delay-line measurement, not G7 release qualification. It
supports retaining the **existing 8,192-frame live admission limit at 48 kHz**
as a provisional development default. At 48 kHz that is 170.7 ms of maximum
parallel-path compensation. The application currently supplies that frame
limit and a 64 MiB aggregate route-history byte budget, with source history
disabled (`apps/daw_quick/src/routed_playback.cpp`).

## Method

`oscillate_delay_compensation_bench` prepares 32 stereo route histories and
times 10,000 blocks after 2,000 warm-up blocks at each 64- and 128-frame
size. Half the routes delay the full budget and half delay half. Every block
fills, processes and checksums all 32 routes. Build: GCC 15.2, Release, Intel
Core i7-10700K, 48 kHz nominal periods. No CPU affinity, controlled
background load, physical output, actual plugin callbacks or xrun counter.
All figures below are one-run wall-clock microseconds per 32-route block.

| Frame budget | 32-route history | 64-frame mean / p99 / worst | 128-frame mean / p99 / worst |
| ---: | ---: | ---: | ---: |
| 64 | 16,640 B | 14.61 / 18.62 / 149.28 | 28.54 / 36.13 / 78.94 |
| 512 | 131,328 B | 14.29 / 18.25 / 48.68 | 28.51 / 36.20 / 95.25 |
| 2,048 | 524,544 B | 14.57 / 29.10 / 54.36 | 28.41 / 43.83 / 148.54 |
| 8,192 | 2,097,408 B | 14.22 / 16.36 / 57.38 | 28.30 / 33.03 / 87.68 |

The 64-frame period is 1,333.3 µs and the 128-frame period is 2,666.7 µs.
The data show no meaningful steady-state CPU growth as the ring grows to
8,192 frames. They do **not** establish whole-graph deadline headroom, since
plugins, instruments, source reads, automation, UI and device work are absent.
The worst values are host scheduling observations, not an xrun measure.

If future topology edits opt in to post-processor source histories for all 33
nodes in a 32-source-plus-master graph, those rings would add 2,162,952 B;
32 route lines plus 33 source histories would occupy 4,260,360 B (4.06 MiB)
before buffers, sidechains, processor state and graph overlap. An active and
prepared successor may briefly hold both sets. The current live graph does
not opt in to that extra source-history memory.

The focused `delay_compensation_contract` exercises exact stereo impulse
alignment immediately after a 4,096-to-8,192-frame block-boundary switch,
ring wrap, and explicit rejection above the supplied limit. It also covers
small growth/shrink, failed preparation retaining the old line, and zero
callback allocations. `routed_mixer_contract` checks the graph-level
growth/shrink and failed hold. A transition requesting a delay larger than
its own frame budget reports `resource_exhausted` immediately, not
`unavailable`, so it is not mistaken for a retryable priming failure.

## Recommendation and remaining gate

Keep the current 8,192-frame development cap at 48 kHz. Do not increase it
based on this isolated benchmark. At other sample rates, treat the present
frame cap as an implementation limit, not a constant millisecond promise.
Before G7 sign-off, measure the reference mixed plugin set on the physical
device at 48 kHz, 64 and 128 frames; count device xruns; repeat with
controlled background load; test latency reports and rapid route/sidechain
changes; and measure transition preparation time and overlapping graph
memory. Confirm whether the 64 MiB aggregate cap and source-history policy
admit the required launch project. The product limit and fallback duration
remain open until that evidence is reviewed.
