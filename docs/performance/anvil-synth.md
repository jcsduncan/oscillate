# Anvil kick synth: local callback cost

ADR-0060 follow-up, 25 October 2026. Local measurement only.

**Setup**
- Hardware: Intel Core i7-10700K.
- Build: GCC 15.2.0, `-O3 -DNDEBUG`, no sanitizer (the engine compiled against the Release libraries).
- Benchmark: `oscillate_anvil_bench` plays four-to-the-floor at 124 BPM for 10 s at 48 kHz, the first second excluded, so every block runs the 4x body, the rumble and the oversampled master.
- Units: percentages are processing time relative to the block's real-time duration on one core.

| Workload | Frames | Mean | p99 block |
| --- | ---: | ---: | ---: |
| Warehouse Rumble | 128 | 2.0 % | 2.8 % |
| Warehouse Rumble | 64 | 2.0 % | 3.2 % |
| Hard Techno Screech (Soft Square, all drives) | 128 | 2.8 % | 4.5 % |
| Tech House Thump | 128 | 1.8 % | 2.9 % |

`process` is allocation-free.

**Preview.** `anvil::render_preview` of one beat (23,226 frames at 48 kHz) takes 8.4–9.6 ms; Hard Techno Screech's 700 ms hit (33,600 frames) takes 18.8 ms. That is the kernel's own cost (about 0.4 µs a frame): the 4x drive and 2x ceiling on both channels are about half of it, and are kept for sound. Anvil's panel renders the preview on a worker thread and shows only the newest edit's render.

**What was measured and fixed on the way:** decaying states reaching denormals (a 50 ms hit's beat cost 24 ms before flushing), integer modulo in the rumble's delay lines, per-sample partial tables and `pow` calls, and two `sin`/`cos` per 4x sample (replaced by one exact pair a frame and a rotation).
