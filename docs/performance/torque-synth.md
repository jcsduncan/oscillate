# Torque bass synth: local callback cost

ADR-0032 follow-up, 19 October 2026. Local measurement only.

**Setup**
- Hardware: Intel Core i7-10700K, load average about 1.3.
- Build: GCC 15.2.0, CMake Release (`-O3 -DNDEBUG`), no sanitizer.
- Benchmark: `oscillate_torque_bench` holds one note (Torque is mono) for 10 s at 48 kHz. The first second is excluded.
- Units: percentages are processing time relative to the block's real-time duration on one core.

| Workload | Frames | Mean | p99 block |
| --- | ---: | ---: | ---: |
| **Budget case:** Reese, 7 saws, comb, fuzz, chorus I+II, delay | 64 | 1.5 % | 1.8 % |
| Same | 256 | 1.5 % | 1.8 % |
| Acid Classic (diode ladder, tube) | 64 | 0.6 % | 0.8 % |
| Seq Acid Line (sequencer running) | 64 | 0.7 % | 1.2 % |
| Moog Bass | 64 | 0.5 % | 0.7 % |
| Distorted 808 (the heaviest preset) | 64 | 1.7 % | 3.0 % |

Every case is well within the ADR-0032 budget of 4 % of one core. `process` is allocation-free with the sequencer running and the cutoff automated.

## Notes

The voice computes filter coefficients once per base-rate frame, because the envelope moves the cutoff at audio rate. That includes the diode ladder's tridiagonal factorisation and a `tan`. Mono polyphony makes this affordable. The filter, drive, VCA and distortion run 2× oversampled.
