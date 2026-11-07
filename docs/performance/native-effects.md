# Native effects: CPU

`oscillate_effects_bench` (Release, one core pinned with `taskset`) runs every factory preset over ten seconds of stereo noise at 48 kHz in 128-frame blocks. The table gives the best of three runs, as a share of real time.

| Engine | Range over presets | Where it goes |
|---|---|---|
| Contour | 0.4–1.5% | 2× oversampled filter models; per-sample coefficients. |
| Forge | 4.9–9.8% | 16× resampling on both wet and dry paths; antiderivatives at the 16× rate. Tanh-family curves cost the most. |
| Orbit | 0.2–0.9% | Lagrange delay reads, one per voice. |
| Relay | 1.1–1.5% | Two lines, the diffusion chain (always fed) and three loop characters. |
| Vista | 0.9–1.0% | Two phase-compensated Linkwitz-Riley trees (mid and side), the decorrelator and meters. |
| Pocket | 6.4–6.7% | 24 bands of fourth-order analysis for kick and bass, 24 bells, and the active-set bell solve every 32 frames. |
| Lens | 4.1–8.4% (13.1% with the sidechain overlay) | Three window sizes per hop; long windows update at 87.5% overlap (every 4–8 hops), short ones every 10.7 ms; 400 display points from running bin sums. |
| Loudness | 0.2% | K-weighting biquads, the 4× windowed-sinc true-peak interpolator and exact gating once per 100 ms. |
| Scope | 0.1% (about 0.8% with a sidechain) | The sweep's min/max columns; with a sidechain, a 16384-point cross-correlation every 85 ms. |
| Tune | 2.8% (6.1% Bass, under 0.1% Kick) | An NSDF by FFT and two single-bin DFTs about 23 times a second (Instrument and Bass), or a low-pass and zero crossings per sample (Kick). |
| Trim | under 0.2% | Gains and two allpasses per channel; stages at rest are bypassed. |
| Signal | under 0.2% | One oscillator, or noise through the 30-odd first-order pink corners. Creation fits the pink filter in about 15 ms. |
| Expanse | 5.1–6.5% | 16 lines with two allpasses and two biquads each; Hadamard mixing; shimmer when on. |

Quality comes first (see the native synth roadmap), so nothing is traded for CPU here. The obvious savings, if they're ever needed:
- **Forge:** a lower oversampling factor while the drive is low.
- **Expanse:** computing absorption only when the decay controls move, instead of every tick.
