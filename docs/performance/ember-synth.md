# Ember vintage synth: local callback cost

ADR-0028 follow-up, 18 October 2026. Local measurement only. This is not a release qualification.

**Setup**
- Hardware: Intel Core i7-10700K.
- Build: GCC 15.2.0, CMake Release (`-O3 -DNDEBUG`), no sanitizer, no intrinsics.
- Benchmark: `oscillate_ember_bench` renders 10 s at 48 kHz per row. It times every `NativeSynthRuntime::process` call, holds all notes from the first block, and excludes the first second from the statistics.
- Units: percentages are processing time relative to the block's real-time duration on one core.
- Repeat runs vary by about ±0.4 points on this unpinned desktop.

| Workload | Frames | Mean | p99 block |
| --- | ---: | ---: | ---: |
| 8 voices unison; ensemble, chorus I+II, delay, tape; audio-rate poly-mod to cutoff; Age 100 % | 64 | 5.9 % | 8.1 % |
| same | 128 | 6.1 % | 8.3 % |
| 8 notes poly, all effects, poly-mod | 64 | 6.2 % | 8.3 % |
| 8 notes, Ember Default | 64 | 4.6 % | 6.4 % |
| 1 note, Ember Default | 64 | 1.1 % | 2.2 % |
| 1 note, effects off | 64 | 0.9 % | 1.5 % |

The worst-case mean meets the ADR-0028 budget of 8 % of one core; its p99 is at the budget.

**Optimization history for the worst case at 64 frames.** Each step reran the oracle suites.
- First correct version: 10.3 %.
- Division-free cubic saturation for OTA stages, plus idle ensemble, chorus and delay skipping their reads (lines keep filling): about 9.5 %.
- Saturation limited to the OTA input and its first two stages: 5.9 %.
  - In isolation, a 2× OTA frame went from 93 ns to about 45 ns. Per-stage saturation lengthened the serial dependency chain; it was not a branching problem, since a min/max clamp measured the same.

Prism is unaffected:
- Its chorus skip returns the exact input it already returned when off.
- Its goldens pass unchanged.

**Real-time.** Allocation-free in `process` at full load, with an automation lane.

## Full four-stage OTA saturation

All four OTA stages saturate with the smooth rational curve, as the hardware does. The table compares a cheaper variant that saturated only the input and first two stages with a cubic curve; sound comes first, and the cubic's harder knee also aliased more.

The four-stage saturation compresses about 2 dB, so Ember's post-filter voice gain rose from 0.6 to 0.75 to keep preset loudness. Goldens were rebaselined.

| Workload | Frames | Mean (before → after) |
| --- | ---: | ---: |
| 8 voices unison, all effects, Age 100 % | 64 | 5.9 % → 7.1 % |
| 8 notes Ember Default | 64 | → 5.7 % |
