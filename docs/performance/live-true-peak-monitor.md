# Quick master true-peak monitor

**18 October 2026.** The final-output callback copies each block into a fixed
64-slot SPSC queue. A worker runs the 4× native true-peak estimator. The UI
shows the left/right maximum since the current transport generation began.
Seek, stop and restart invalidate old readings. If a block cannot be queued,
or the callback observes a same-generation position gap, the UI shows the
reading as unavailable until the next generation. The callback does not run
the interpolation kernel, allocate, wait or access Qt.

The focused fake-device contract covers final-output samples, seek/stop,
oversized callback input, queue saturation, callback allocation and stale
reading rejection. The focused ThreadSanitizer run passes. The estimator's
signal oracle and standards limits are in [the true-peak core note](true-peak-core.md).

## Local callback copy cost

An uninstrumented GCC 15.2.0 `-O3 -DNDEBUG` scratch probe on the Intel
i7-10700K paced 2,000 stereo blocks at the nominal 48 kHz period for each
size. It timed `LiveTruePeakMonitor::observe` with `steady_clock` while its
worker ran. Both runs had zero dropped blocks and no incomplete reading.
The scratch source and binary are under `build/quick-release/scratch` and are
not part of CTest.

| Frames | p50 µs | p99 µs | Maximum µs | Dropped blocks |
| ---: | ---: | ---: | ---: | ---: |
| 64 | 0.268 | 0.785 | 1.508 | 0 |
| 128 | 0.443 | 1.261 | 2.190 | 0 |

This times only the callback copy/tap, not the upstream audio graph, worker
CPU, Qt repaint or device backend. The host was not pinned or isolated from
background load. The existing unpaced wrapper microbenchmark saturates this
worker queue and therefore cannot represent normal true-peak copy cost.
Actual backend underruns, controlled 32-track headroom, near-Nyquist filter
conformance and reference-meter comparison remain release gates.
