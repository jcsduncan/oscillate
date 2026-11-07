# Drift granular synth: local callback cost

ADR-0033 follow-up, 19 October 2026. Local measurement only.

**Setup**
- Hardware: Intel Core i7-10700K, load average about 1.5.
- Build: GCC 15.2.0, CMake Release (`-O3 -DNDEBUG`), no sanitizer.
- Benchmark: `oscillate_drift_bench` holds 12 notes for 10 s at 48 kHz. The first second is excluded.
- Units: percentages are processing time relative to the block's real-time duration on one core.

| Workload | Frames | Mean | p99 block |
| --- | ---: | ---: | ---: |
| **Budget case:** 12 notes, 50 grains/s, 200 ms grains, shimmer 0.6 | 64 | 11.2 % | 14.4 % |
| Same | 256 | 11.9 % | 18.1 % |
| 12 notes, 200 grains/s, 200 ms grains (about 40 grains per note) | 64 | 20.1 % | 24.5 % |
| Spectral Smear, 12 notes | 64 | 12.4 % | 15.4 % |
| Stutter Clock (synced), 12 notes | 64 | 9.1 % | 15.4 % |

The budget case meets the ADR-0033 target of 15 % of one core. CPU targets are soft (sound comes first); this one is met without trading any sound. `process` is allocation-free with Freeze and Blur automated.

## Where the time goes

- **The source.** Each voice computes its source (wavetable, 16 partials and air) every frame, and this was most of the cost. It went from 15.3 % to 10.3 % with two changes that cost no sound:
  - The partials run as complex rotations, renormalised every control tick, instead of polynomial sines.
  - A frozen voice stops computing its source once the in-flight guard has been written.
- **Mip levels.** Writing the source through four half-band decimators adds about 1 %. In exchange, pitched-up grains never alias.
- **Grains.** Each grain is one cubic read plus a window per frame, so cost rises roughly with density × size.
