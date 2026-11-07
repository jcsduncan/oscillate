# Prism native synth: local callback cost and quality evidence

ADR-0027 follow-up, 18 October 2026. Local measurement only. This is not the 32-track release qualification or a physical-device xrun result.

**Setup**
- Hardware: Intel Core i7-10700K, x86-64.
- Build: GCC 15.2.0, CMake Release (`-O3 -DNDEBUG`), no sanitizer. No intrinsics or runtime dispatch. Stereo filter and oversampler pairs are written as element-wise C++ that GCC maps onto SSE2.
- Benchmark: `oscillate_prism_bench` renders 10 s at 48 kHz per row. It times every `NativeSynthRuntime::process` call, starts all notes in the first block and holds them, and excludes the first second (attacks and warm-up) from the statistics.
- Units: percentages are processing time relative to the block's real-time duration on one core.

## Callback cost

| Workload | Frames | Mean | p99 block | Max block |
| --- | ---: | ---: | ---: | ---: |
| 16 notes, 8+8 unison, ladder, filter drive 12 dB, post drive, chorus I+II | 64 | 14.9 % | 23.2 % | 29.1 % |
| same | 128 | 14.1 % | 14.9 % | 16.0 % |
| 16 notes, 8+8 unison, LP24 | 64 | 11.9 % | 14.9 % | 21.5 % |
| 16 notes, 1+1 unison, ladder | 64 | 8.2 % | 10.3 % | 15.6 % |
| 16 notes, Prism Default | 64 | 5.9 % | 6.3 % | 8.0 % |
| 16 notes, Prism Default | 128 | 5.9 % | 6.4 % | 8.0 % |
| 1 note, Prism Default | 64 | 0.57 % | 0.62 % | 1.7 % |

The worst-case mean meets the ADR-0027 budget of 15 % of one core. Its 64-frame p99 does not yet (23.2 %), so treat the worst case as heavy for a single track at small buffers. Typical patches cost well under 1 % per note.

**Optimization history for the worst case at 64 frames.** Each step kept the same output contract and reran the oracle suites.
- First correct version: 26.9 %.
- Half-band reduced from 12 to 8 coefficients: 23.9 %. Images are still below −100 dB up to 20 kHz at 44.1 kHz.
- Silent sub, noise and ring sources skipped per control interval: 23.9 %.
- Voice rendered stage by stage per control interval: 20.0 %.
- Oscillator split into an inline fast path and an out-of-line event path: 16.7 %.
- Stereo filter and oversampler lanes processed together: 14.9 %.

## Real-time and determinism

- The focused allocation probe reports zero allocations in `process` at full polyphony, 8+8 unison and with an automation lane.
- Control values are evaluated on a 32-frame grid aligned to absolute project frames. Renders with 64-frame, 128-frame and irregular block splits (37/100/513/64/1/255) are bit-identical for four presets.
- Live panel edits use a four-entry SPSC mailbox with revision acknowledgement. Parameters marked next-note never reset a sounding note; the runtime reports them as deferred until the old notes end.

## Audio quality, measured by the contract tests

| Property | Result |
| --- | --- |
| Saw at C8, 44.1 kHz, inharmonic floor from 20 Hz to 18 kHz | −99 dBFS (naive saw: above −40) |
| Pulse at 30 % width, A7 / triangle at A7 | −108 / −126 dBFS |
| Hard sync (saw, sine and triangle slaves, C6 master) | −99 / −81 / −96 dBFS |
| Pitch at 44.1, 48, 88.2 and 96 kHz, keys 33, 69 and 96 | within 0.5 cent |
| SVF against bilinear analog prototypes (LP, HP, BP 12, LP24) | within 0.05 dB |
| Half-band round trip up to 18 kHz | flat within 0.01 dB |
| Envelope segments, any curve | land on the exact frame |

**Oscillator latency.** The BLEP/BLAMP kernel is a Kaiser-windowed sinc with 24 taps, cut off at 0.45 of the sample rate. Oscillator output is therefore linear phase and 11 frames late. That is 0.25 ms at 44.1 kHz, the same in playback and export.
