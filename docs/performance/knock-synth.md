# Knock percussion synth: local callback cost

ADR-0068 follow-up. Measured 29 October 2026 on an Intel Core i7-10700K (3.8 GHz), GCC 15.2, the `quick-opt` Release build, 48 kHz.

**How to run**

```sh
cmake -DOSCILLATE_BUILD_BENCHES=ON build/release
scripts/build.sh build/release --target oscillate_knock_bench
build/release/bench/native_dsp/oscillate_knock_bench
```

**Workload.** `oscillate_knock_bench` hits all eight pads on every sixteenth at 124 BPM for 10 s, so both voices of every pad ring. The kit loads a sixteenth before the first hits, as a kit chosen while playing does. It prints:
- the mean cost and the p99 block cost, as a percentage of real time on one core, after the first second;
- "first hits": the block where all eight pads first land, after the kernel has calibrated them over its first control ticks (ADR-0068 §7).

`process` stays allocation-free (`native_knock_contract.full_load_does_not_allocate`).

With the kits' default widths (ADR-0068 §14), measured after Width; before it, Organic was 7.3% (64) and 7.4% (128) mean.

| Workload | Frames | Mean | p99 block | First hits |
| --- | ---: | ---: | ---: | ---: |
| Minimal | 64 | 3.2% | 15.0% | 9.6% |
| Dub Techno | 64 | 8.4% | 21.8% | 11.9% |
| Organic | 64 | 10.8% | 25.1% | 12.5% |
| Glass Garden | 64 | 5.0% | 17.5% | 8.9% |
| Minimal | 128 | 3.2% | 12.9% | 11.5% |
| Dub Techno | 128 | 8.2% | 19.7% | 14.6% |
| Organic | 128 | 10.7% | 23.5% | 16.3% |
| Glass Garden | 128 | 5.0% | 15.4% | 13.7% |

**Turning a knob while it plays** (`Organic, turning Pitch / Decay`: the parameter automated across its range every two seconds):

| Workload | Frames | Mean | p99 block | Max |
| --- | ---: | ---: | ---: | ---: |
| Turning Pitch | 64 | 10.6% | 29.5% | 55.9% |
| Turning Decay | 64 | 10.2% | 24.5% | 35.8% |
| Turning Pitch | 128 | 10.5% | 28.5% | 34.4% |
| Turning Decay | 128 | 10.3% | 23.0% | 34.0% |

Before the fix in ADR-0068 §7, turning Pitch cost 104% of a core on average and turning Decay peaked at 277% of a block.

**Width** (`Organic, all wide`: every pad at Spread 1, Scatter at Depth 0.8 and Haas 10 ms). Each ringing voice adds one stereo pass over its modes and a Haas line read per frame; Spreading every pad fully costs about the same as the kits' defaults, since the stereo pass is per mode whatever the spread:

| Workload | Frames | Mean | p99 block | First hits |
| --- | ---: | ---: | ---: | ---: |
| Organic, all wide | 64 | 10.9% | 25.1% | 12.5% |
| Organic, all wide | 128 | 10.9% | 23.9% | 16.5% |

Swung hits cost nothing extra: a pending hit only splits the render at its frame. The bench passes no transport, so no hit swings in it.

**Worst case.** A kit change in the very block where all eight pads hit calibrates them all in that block: about 150% of a 64-frame block (2 ms) and 75–95% of a 128-frame one. That 2 ms is about 40% of a 256-frame block.
