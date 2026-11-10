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
| Cast | 1.2–1.9% at 64 and 128 frames (`oscillate_cast_bench`, true-stereo bundled IRs of 0.4–9.5 s); 99th-percentile block 2.3–5.2% of its period, longest 3.2–12.3%; process CPU (getrusage) equal to the mean | The 64-tap head per route, then each tier's partition products, spread over its next block. |
| Hush | 0.45–0.55% (0.46–0.56% in 64-frame blocks) | Eight key-filter biquads, the detector and the state per channel; a `pow` only while the gain moves. |
| Ceiling | 7.0–7.1% at 64 and 128 frames (`oscillate_ceiling_bench`, noise 12 dB over); 99th-percentile block 8.4–10.2% of its period, longest 11.9–17.7%; process CPU equal to the mean. Sixteen in a chain: 110–114% of one core, 99th percentile 118–150%. With no room to clip (Transients and Clip at 0): 2.5% | The 8× linear-phase clipper (about 4.5%), which sleeps with no room; the 8× fine-peak detector, the 4× guard and the output true-peak meter, about 1.9% together; K-weighted loudness. |

Ceiling was measured on an Intel Core i7-10700K with GCC 15.2, Release, pinned with `taskset`.

## Ceiling at equal loudness

`oscillate_limiter_audit` (`tools/limiter_audit`; ADR-0117 gives the method) drives each preset to equal integrated loudness under a −1 dBTP ceiling. `--classic` measures the classic sample-peak limiter Ceiling replaced, with its former presets, trimmed after rendering until its true peak sits at the ceiling. In the tables:
- **TP over** is that limiter's own true-peak overshoot.
- **Onset loss** is in three bands: under 150 Hz, 150 Hz to 2.5 kHz, and over 2.5 kHz.
- **Pump** is the RMS of the top band's gain modulation.
- **Tones** peak 3, 6 and 9 dB over the ceiling at the three targets.

Eight mastered tracks (30 s each), mean; "max" marks a limiter that could not reach the target and the loudness it reached:

| Target | Limiter, preset | LUFS | TP over | Crest loss | Onset loss L/M/H | Pump |
|---|---|---|---|---|---|---|
| −14 | Classic, Default | −14.00 | 0.00 | −0.01 | 0.00/0.00/0.00 | 0.00 |
| −14 | Ceiling, Default | −14.00 | −0.01 | 0.00 | 0.00/0.00/0.00 | 0.01 |
| −9 | Classic, Loud | −9.01 | +1.44 | 1.15 | 0.03/1.10/−0.01 | 1.77 |
| −9 | Classic, Default | −9.33 max | +1.44 | 0.90 | −0.32/0.78/−0.08 | 2.25 |
| −9 | Ceiling, Default | −9.10 | −0.01 | 0.66 | −0.78/−0.02/−0.43 | 1.73 |
| −9 | Ceiling, Punchy | −9.00 | −0.01 | 0.67 | −1.01/−0.29/−0.31 | 1.14 |
| −6 | Classic, Loud | −7.96 max | +1.48 | 2.24 | 0.61/2.09/0.06 | 3.15 |
| −6 | Ceiling, Punchy | −7.50 max | −0.01 | 1.78 | −0.80/0.28/−0.52 | 2.60 |

The synthetic drum loop. Trimmed to a compliant true peak, the classic limiter reaches −9.82 LUFS at most with its Default, so −10 is added for an equal comparison:

| Target | Limiter, preset | LUFS | TP over | Crest loss | Onset loss L/M/H | Pump |
|---|---|---|---|---|---|---|
| −14 | Classic, Default | −13.98 | +0.82 | 0.07 | 0.05/0.09/−0.05 | 0.38 |
| −14 | Ceiling, Default | −13.97 | −0.01 | 0.09 | 0.01/0.02/0.01 | 0.07 |
| −10 | Classic, Default | −9.98 | +0.82 | 1.16 | 0.67/1.23/−0.21 | 3.70 |
| −10 | Ceiling, Default | −9.97 | −0.01 | 0.88 | 0.08/0.47/−0.45 | 2.42 |
| −9 | Classic, Loud | −9.05 | +0.97 | 2.04 | 1.38/2.06/0.28 | 3.27 |
| −9 | Ceiling, Punchy | −9.01 | −0.01 | 1.35 | −0.13/0.37/−0.41 | 2.40 |
| −6 | Classic, Loud | −8.37 max | +0.97 | 2.75 | 1.87/2.66/0.80 | 4.09 |
| −6 | Ceiling, Punchy | −8.26 max | −0.01 | 2.10 | 0.22/0.93/−0.17 | 3.33 |

Tones 6 dB over the ceiling, dB:

| Limiter, preset | THD+N 50 Hz | THD+N 1 kHz | SMPTE IMD | CCIF IMD |
|---|---|---|---|---|
| Classic, Default | −59.7 | −148.6 | −64.8 | −166.3 |
| Classic, Loud | −36.3 | −148.6 | −36.6 | −166.3 |
| Ceiling, Default | −150.0 | −151.4 | −110.8 | −159.4 |
| Ceiling, Loud Club (1 dB of Clip on every peak) | −42.2 | −42.2 | −44.1 | −56.1 |

- **True peak.** At 16×, the classic limiter's true peak reads up to 1.1 dB (loop) and 1.5 dB (tracks) over the ceiling; Ceiling's reads up to 0.24 and 0.11 dB over.
- **Latency.** At the default lookahead, the classic limiter's was 5.0 ms; Ceiling's is 7.4 ms.
- **CCIF.** Both limiters are at the numerical floor.
- **Crest at −14 LUFS.** The 0.02 dB difference on the loop is the gain stage's margin under the ceiling.

Quality comes first, so nothing is traded for CPU here. The obvious savings, if they're ever needed:
- **Forge:** a lower oversampling factor while the drive is low.
- **Expanse:** computing absorption only when the decay controls move, instead of every tick.
