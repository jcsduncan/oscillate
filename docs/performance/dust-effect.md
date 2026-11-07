# Dust: callback cost

ADR-0069 follow-up. Measured 29 October 2026 on an Intel Core i7-10700K, GCC 15.2, the `quick-opt` Release build at 48 kHz, niced, with the DAW open (so slightly pessimistic). The rain presets cost most: every droplet is its own voice.

## How to run it

The bench (`bench/native_dsp/dust_bench.cpp`) is built only with `-DOSCILLATE_BUILD_BENCHES=ON`, and it is not a CTest gate. Build and run it in an uninstrumented Release build, one core pinned, with nothing else building:

```sh
cmake --preset release -DOSCILLATE_BUILD_BENCHES=ON
scripts/build.sh build/release --target oscillate_dust_bench
build/release/bench/native_dsp/oscillate_dust_bench
```

## What it measures

`oscillate_dust_bench` plays each factory preset for ten seconds at 48 kHz:
- **Input:** quiet stereo noise (−26 dBFS).
- **Sidechain:** a 60 Hz kick every half second, so the duck works.
- **Transport:** 120 BPM, so the radio's gate runs.

It runs the whole thing in 64- and 128-frame blocks, excluding the first second. It reports the mean and the 99th-percentile block as a share of the block's real-time duration on one core, best of three.

Expect the cost to follow:
- **The layers that are on:** a layer that is off costs nothing once its fade ends.
- **Rain:** its drop voices (up to 32) and the Expanse room behind Distance.
- **Wind:** ten filters a channel, retuned every 32 frames.

## Results

**Setup**
- Hardware: Intel Core i7-10700K.
- Build: GCC 15.2, `quick-opt` Release, DAW open.
- Units: % of one core, per block's real-time duration.

| Preset | 64 mean | 64 p99 | 128 mean | 128 p99 |
| --- | ---: | ---: | ---: | ---: |
| Dusty 33 | 1.49% | 1.97% | 1.48% | 1.66% |
| Worn 78 | 1.63% | 1.93% | 1.64% | 2.67% |
| Fresh 45 | 1.36% | 2.07% | 1.35% | 2.05% |
| Cassette Deck | 1.28% | 1.43% | 1.26% | 2.01% |
| Late Night Radio | 0.96% | 2.07% | 0.95% | 1.20% |
| Static Pulse | 0.56% | 0.64% | 0.55% | 0.66% |
| Rain on the Window | 6.69% | 9.88% | 6.73% | 8.84% |
| Storm Front | 7.46% | 8.23% | 7.76% | 11.98% |
| Wind Through the Yard | 0.77% | 0.95% | 0.77% | 0.89% |
| Dub Chamber | 1.86% | 2.04% | 1.87% | 2.03% |
| Basement Hum | 0.81% | 1.05% | 0.80% | 0.95% |
| Server Room | 1.02% | 1.30% | 1.02% | 1.21% |
| Warehouse | 1.08% | 1.40% | 1.07% | 1.31% |
| Lo-fi Bed | 1.53% | 1.83% | 1.53% | 1.74% |
| Field Day | 7.04% | 8.06% | 6.99% | 7.16% |
| Init | 0.36% | 0.37% | 0.35% | 0.44% |

`process` is allocation-free (`native_dust_contract.the_callback_never_allocates`, and the platform contract for every preset).
