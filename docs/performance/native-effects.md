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
| Lens | 4.1–8.4% (13.1% with the sidechain overlay), and about 0.25% for the tonal meter | Three window sizes per hop; long windows update at 87.5% overlap (every 4–8 hops), short ones every 10.7 ms; 400 display points from running bin sums. The tonal meter (ADR-0120): buffering every frame, then one 16384-point transform (0.34 ms on an i7-10700K) and the band sums every 8192 frames; 0.25% is all of it. |
| Loudness | 0.2% | K-weighting biquads, the 4× windowed-sinc true-peak interpolator and exact gating once per 100 ms. |
| Scope | 0.1% (about 0.8% with a sidechain) | The sweep's min/max columns; with a sidechain, a 16384-point cross-correlation every 85 ms. |
| Tune | 2.8% (6.1% Bass, under 0.1% Kick) | An NSDF by FFT and two single-bin DFTs about 23 times a second (Instrument and Bass), or a low-pass and zero crossings per sample (Kick). |
| Trim | under 0.2% | Gains and two allpasses per channel; stages at rest are bypassed. |
| Signal | under 0.2% | One oscillator, or noise through the 30-odd first-order pink corners. Creation fits the pink filter in about 15 ms. |
| Expanse | 5.1–6.5% | 16 lines with two allpasses and two biquads each; Hadamard mixing; shimmer when on. |
| Cast | 1.2–1.9% at 64 and 128 frames (`oscillate_cast_bench`, true-stereo bundled IRs of 0.4–9.5 s); 99th-percentile block 2.3–5.2% of its period, longest 3.2–12.3%; process CPU (getrusage) equal to the mean | The 64-tap head per route, then each tier's partition products, spread over its next block. |
| Hush | 0.45–0.55% (0.46–0.56% in 64-frame blocks) | Eight key-filter biquads, the detector and the state per channel; a `pow` only while the gain moves. |
| Strike | Measured by `oscillate_strike_bench` on drum hits over noise, not the noise above, with one instance and with sixteen. 0.34–0.38% with one band at 64 and 128 frames; 99th-percentile block 0.5–0.7% of its period. Linear-phase bands with Clip: 4.3–4.4% (Drum Bus Punch, three bands) and 5.3% (Master Impact, four), 99th percentile 10.7–20.5%, longest 18.7–45.7%. Process CPU equal to the mean. Sixteen single-band instances in a chain: 4.7–8.9%, 99th percentile 7.7–12.5% | Per band and channel the quadrature pair (eight allpass sections), the level and two followers, and one `exp` while the gain moves. The linear-phase bank runs one forward and one inverse FFT per band every 512 frames, so its cost lands in one block of each hop; the 4× clip about 0.8%. |
| Keel | Measured by `oscillate_keel_bench` on noise, with one instance and with sixteen. 1.2–1.3% at 64 and 128 frames; 99th-percentile block 1.8–2.6% of its period, longest 3.0–6.4%. With a reference on the sidechain: 1.8%, 99th percentile 3.7–4.4%. Process CPU equal to the mean. Sixteen in a chain: 19.6–20.5%, 99th percentile 31.4–43.0% (29.7–29.8% and 52.9–73.3% with references) | Eleven state-variable filters per channel and their gliding coefficients; one 16384-point transform per 2048 frames, spread a stage at a time over the hop, and the least-squares fit once a hop. |
| Gild | Measured by `oscillate_gild_bench` on a mix (bass, chord partials, kicks, hiss), with one instance and with eight. 0.9–1.0% at rest (four bands, no drive); one driven band 5.0–5.2% at 4× (Warm Mids, Bass Weight) and 6.2–7.8% at 8× (Vocal Presence, Air Lift), 99th-percentile block 7.5–12.3% of its period; Tape Glue (four linear-phase bands at 4×) 17.2–18.2%, 99th percentile 26.6–47.3%. Process CPU equal to the mean. Eight in a chain: 39–61% for one driven band each, 147–149% for Tape Glue | Per driven band and channel Edge's linear-phase up and down resampling (about 2.7% a band at 4×), the curve at the top rate (twice with Even or the tube, for the even part), Level Match and the spectrum's 4096-point transform every 2048 frames. A band at zero drive sleeps. |
| Ceiling | 7.0–7.1% at 64 and 128 frames (`oscillate_ceiling_bench`, noise 12 dB over); 99th-percentile block 8.4–10.2% of its period, longest 11.9–17.7%; process CPU equal to the mean. Sixteen in a chain: 110–114% of one core, 99th percentile 118–150%. With no room to clip (Transients and Clip at 0): 2.5% | The 8× linear-phase clipper (about 4.5%), which sleeps with no room; the 8× fine-peak detector, the 4× guard and the output true-peak meter, about 1.9% together; K-weighted loudness. |

Ceiling and Gild were measured on an Intel Core i7-10700K with GCC 15.2, Release, pinned with `taskset`.

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

## Strike on drums

`oscillate_limiter_audit --shaper` runs each Strike preset at the programme's own level and reports what it changes: integrated loudness, short-term crest factor (16× true peak over RMS), onset contrast in the three bands (positive is more attack against the body) and the 16× true peak. The synthetic drum loop:

| Preset | LUFS change | Crest change | Onset contrast change L/M/H | True peak change | Latency |
|---|---|---|---|---|---|
| Kick Snap | +0.07 | +1.51 | +1.14/+2.05/+0.57 | +4.46 | 1.0 ms |
| Snare Crack | +1.15 | +1.40 | +1.32/+2.00/+0.68 | +5.85 | 1.0 ms |
| Tighten Room | −3.41 | +1.03 | +0.21/+0.99/−0.43 | −0.80 | 0 |
| Drum Bus Punch | +0.09 | +0.65 | +0.52/+1.50/+0.49 | +1.83 | 32.8 ms |
| Master Impact | +0.32 | +0.29 | +0.34/+0.63/+0.29 | +1.09 | 32.8 ms |
| Soften Attack | +0.44 | −0.03 | −1.52/−1.69/−0.44 | −0.45 | 0 |

The same three bands split at minimum phase with every gain at 0 dB change the onset contrast by −1.09/−0.43/0.00 dB: the crossovers' allpass delays the lows. Linear phase changes nothing at zero settings, so the bus and master presets use it.

The eight mastered tracks (mean):

| Preset | LUFS change | Crest change | Onset contrast change L/M/H | True peak change | Latency |
|---|---|---|---|---|---|
| Kick Snap | −0.41 | +1.33 | +0.79/+1.44/+0.32 | +4.07 | 1.0 ms |
| Snare Crack | +0.52 | +1.23 | +0.91/+1.39/+0.40 | +5.26 | 1.0 ms |
| Tighten Room | −3.15 | +1.19 | +0.58/+0.84/−0.38 | −0.29 | 0 |
| Drum Bus Punch | −0.13 | +0.70 | +0.99/+1.71/+0.52 | +2.60 | 32.8 ms |
| Master Impact | +0.27 | +0.26 | +0.54/+0.65/+0.35 | +1.19 | 32.8 ms |
| Soften Attack | +0.59 | +0.13 | −1.36/−1.36/−0.26 | +1.51 | 0 |

Strike was measured on an Intel Core i7-10700K with GCC 15.2, Release, pinned with `taskset`.

## Gild on mixes

`oscillate_limiter_audit --exciter` runs each Gild preset at the programme's own level and reports the integrated loudness change, what the preset adds (its Delta) against the programme, overall and in three bands (under 150 Hz, 150 Hz to 2.5 kHz, over 2.5 kHz), and the 16× true peak change. "None" is under −120 dB. The eight mastered tracks (mean):

| Preset | LUFS change | Added dB | Added L/M/H dB | True peak change | Latency |
|---|---|---|---|---|---|
| Default | 0.00 | none | none/none/none | +2.57 | 1.85 ms |
| Air Lift | 0.00 | −45.8 | −56.1/−52.5/−27.0 | −0.02 | 1.92 ms |
| Warm Mids | −0.01 | −34.5 | −39.8/−28.0/−32.0 | +2.42 | 1.85 ms |
| Bass Weight | −0.01 | −25.1 | −25.2/−26.0/−94.6 | +2.62 | 1.85 ms |
| Tape Glue | 0.00 | −35.2 | −36.1/−33.9/−43.2 | +0.07 | 33.83 ms |
| Vocal Presence | 0.00 | −46.6 | −58.4/−44.3/−30.5 | +1.47 | 1.92 ms |

The synthetic drum loop:

| Preset | LUFS change | Added dB | Added L/M/H dB | True peak change | Latency |
|---|---|---|---|---|---|
| Default | 0.00 | none | none/none/none | +0.40 | 1.85 ms |
| Air Lift | 0.00 | −36.4 | −50.6/−41.5/−19.2 | −0.72 | 1.92 ms |
| Warm Mids | 0.00 | −46.6 | −50.9/−37.5/−42.3 | −0.51 | 1.85 ms |
| Bass Weight | −0.02 | −25.1 | −26.2/−21.8/−94.2 | −0.58 | 1.85 ms |
| Tape Glue | 0.00 | −35.6 | −37.0/−30.5/−47.3 | −0.16 | 33.83 ms |
| Vocal Presence | 0.00 | −49.2 | −59.4/−44.7/−34.1 | −0.10 | 1.92 ms |

- **Level.** Level Match keeps every preset within 0.02 LU.
- **Harmonics land above the band driven.** Air Lift adds most over 2.5 kHz, Bass Weight in the lows and mids, and nothing over 2.5 kHz.
- **True peak.** At minimum phase the split's allpass changes the waveform even at zero drive (Default: +2.6 dB on the tracks). Tape Glue's linear phase moves it by under 0.2 dB.

## Keel toward its targets

`oscillate_limiter_audit --balance` runs each Keel preset at the programme's own level. It reports:
- after 8 s of settling, the third-octave balance's distance from the preset's target, before and after (RMS and largest, from 40 Hz to 16 kHz, both centred there);
- after 8 s of settling, the wander: per band, the standard deviation of the applied gain (output over input) across one-second frames, averaged over the bands;
- over the whole programme, the integrated loudness change and the onset contrast change in the three bands (positive is more attack against the body);
- the programme's own tilt against pink noise.

The eight mastered tracks (mean; their tilt is −2.69 dB/oct):

| Preset | Before RMS / max dB | After RMS / max dB | LUFS change | Wander dB | Onset contrast change L/M/H |
|---|---|---|---|---|---|
| Default | 4.20 / 9.46 | 3.05 / 8.12 | +0.06 | 0.50 | +0.52/+0.30/+0.15 |
| Gentle Polish | 4.20 / 9.46 | 3.49 / 8.71 | +0.05 | 0.24 | +0.33/+0.20/+0.10 |
| Tame Harsh | 4.20 / 9.46 | 3.81 / 9.12 | +0.03 | 0.16 | −0.03/+0.11/+0.10 |
| Warm It Up | 4.30 / 10.54 | 3.25 / 9.25 | +0.05 | 0.40 | +0.35/+0.27/+0.14 |
| Streaming Balance | 4.20 / 9.46 | 3.19 / 8.43 | +0.07 | 0.38 | +0.47/+0.28/+0.14 |
| Low End Control | 4.20 / 9.46 | 3.78 / 8.89 | +0.02 | 0.36 | +0.56/+0.03/−0.01 |

The synthetic drum loop (tilt −2.62 dB/oct):

| Preset | Before RMS / max dB | After RMS / max dB | LUFS change | Wander dB | Onset contrast change L/M/H |
|---|---|---|---|---|---|
| Default | 6.81 / 14.49 | 4.83 / 11.34 | +0.04 | 0.05 | +0.89/−0.21/−0.16 |
| Gentle Polish | 6.81 / 14.49 | 5.63 / 12.68 | +0.01 | 0.02 | +0.46/−0.12/−0.09 |
| Tame Harsh | 6.81 / 14.49 | 6.40 / 13.89 | +0.00 | 0.01 | −0.01/−0.00/−0.15 |
| Warm It Up | 6.94 / 14.65 | 5.34 / 12.16 | +0.02 | 0.03 | +0.48/−0.15/−0.13 |
| Streaming Balance | 6.81 / 14.49 | 5.11 / 11.83 | +0.02 | 0.03 | +0.72/−0.19/−0.14 |
| Low End Control | 6.81 / 14.49 | 6.36 / 13.76 | +0.01 | 0.03 | +0.94/−0.09/−0.02 |

The onset contrast moves with the balance inside each band: a low band brought down at 50 Hz against 120 Hz has more click over its body. The filters' own phase adds little: converged on a 4 dB lift at 80 Hz, Keel's group delay stays under 2 ms in every band from 31.5 Hz to 16 kHz (`native_keel_contract`). The wander on music is the music's balance moving under a 4 s Speed; on the steady loop it is 0.05 dB.

Keel was measured on an Intel Core i7-10700K with GCC 15.2, Release, pinned with `taskset`.

Quality comes first, so nothing is traded for CPU here. The obvious savings, if they're ever needed:
- **Forge:** a lower oversampling factor while the drive is low.
- **Expanse:** computing absorption only when the decay controls move, instead of every tick.
