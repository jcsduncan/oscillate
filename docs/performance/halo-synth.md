# Halo FM synth: local callback cost

ADR-0031 follow-up, 18 October 2026. Local measurement only.

**Setup**
- Hardware: Intel Core i7-10700K, shared with other builds (load average 3–10 during runs).
- Build: GCC 15.2.0, CMake Release (`-O3 -DNDEBUG`), no sanitizer.
- Benchmark: `oscillate_halo_bench` holds 16 notes for 10 s at 48 kHz. The first second is excluded.
- Units: percentages are processing time relative to the block's real-time duration on one core.

| Workload | Frames | Mean | p99 block |
| --- | ---: | ---: | ---: |
| **Budget case:** 16 notes × 2 unison, Strings (all six operators, ensemble, chorus II) | 64 | 8.4 % | 12.4 % |
| 16 × 2, same | 256 | 8.1 % | 8.6 % |
| 16 × 2, effects off | 64 | 7.7 % | 10.7 % |
| 16 × 1, effects off | 64 | 6.0 % | 6.6 % |
| 16 × 2 with the ladder filter | 64 | 12.2 % | 16.5 % |
| 16 × 4 unison | 64 | 12.7 % | 17.8 % |
| E-Piano, 16 notes | 64 | 6.6 % | 8.8 % |

The budget case meets the ADR-0031 budget of 10 % of one core. After ADR-0034 added a crossfade for waveform switches, a re-run measured 8.8 % (a check on the control rate and one predictable branch per operator). The optional filter runs per voice at the oversampled rate and costs about 4 % at full polyphony. `process` is allocation-free.

## What made it fit

The first working kernel measured 29.9 %. The cost was mostly latency: each sample is a serial chain of six table reads, each feeding the next operator's phase.

| Change | 16 × 2 mean |
| --- | ---: |
| First kernel: one lane at a time, double-precision phase | 29.9 % |
| Unison lanes evaluated interleaved; 32-bit fixed-point phase and flat tables | 17.9 % |
| Non-feedback operators read no previous output, so both oversampled frames interleave | 14.3 % |
| Kernel templated on lane count (accumulators in registers) | 13.1 % |
| Modulation pre-scaled to fixed units at control rate, off the chain | 12.2 % |
| Kernel specialised per algorithm (compile-time routing; only real connections summed) | 8.4 % |

Double-precision tables (32 KB for the sine) were tried and were slower than float tables.

The 128 specialised kernels (32 algorithms × 1–4 lanes) make `halo.cpp.o` about 1.0 MB of code, roughly 7.5 KB per kernel. Only the current routing's kernel runs, so the instruction-cache footprint per voice is one kernel.

## Quality checks that ran alongside

- **Aliasing:** Bright EP at C6, 44.1 kHz, held at its attack-peak indices, has an inharmonic floor of −96 dBFS. The target was below −70 dBFS.
- **Sidebands:** through the whole voice, they match J_n(β) within 0.2 dB.
- **Pitch:** within 0.5 cent at 44.1, 48 and 96 kHz.
- **Golden fingerprints:** within tolerance after every optimisation step, in Debug and Release.
