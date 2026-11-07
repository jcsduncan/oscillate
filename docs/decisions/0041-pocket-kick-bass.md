# ADR-0041: Pocket, a kick and bass masking resolver

- **Status:** Accepted; phase 1 built.
- **Date:** 2026-10-19.
- **Builds on:** ADR-0040, the native effect platform.

## Context

Kick and bass fight over the same 40–150 Hz. Engineers resolve that conflict in six ways:
- **Timing:** duck the bass under the kick, as Kickstart, LFOTool and VolumeShaper do.
- **Frequency:** duck only the overlapping band, as Trackspacer does and as dynamic EQ does when keyed from the kick.
- **Phase:** rotate the bass's low end so it adds to the kick instead of cancelling, as Sound Radix Pi and Auto-Align do.
- **Tuning:** tune the kick to the key.
- **Ownership:** decide which instrument owns the sub, and keep the sub mono.
- **Perception:** add back harmonics while the sub is ducked, so the bass note survives.

No product does all of these. None of them can read the timeline ahead.

## Decision

Pocket is a native effect (ADR-0040). It goes on the bass, with the kick as its sidechain. It's built on a general masking engine, which other pairings can reuse later: vocal against bed, dialogue against music.

1. **Analysis.** One log-spaced bank of 24 bands, 30 Hz to 8 kHz, analyses both signals. Each band has a band-pass and an envelope follower.
2. **Collision.** For each band and each moment: how far the bass sits above the kick plus a margin, counted only while the kick is present in that band. This is "Unmask" mode. "Duck" mode reduces in proportion to the kick's level instead.
3. **Resolvers**, any of which can be swapped in:
   - gain: the per-band ducking, phase 1;
   - phase: low-band allpass rotation and polarity, phase 2;
   - pitch: kick-pitch detection against the bass notes, phase 3;
   - harmonic fill: phase 3;
   - sub mono: phase 1.
4. **Scheduling.**
   - Reactive, from the sidechain, with optional lookahead. Lookahead is a fixed latency, so changing it rebuilds the route (ADR-0040).
   - Predictive, from timeline events known ahead of time. That is phase 4, and it needs a platform extension.

**Ducking is a cascade of dynamic bell filters,** one per band, with zero-delay-feedback state-variable filters, so it stays stable under modulation.
- **Bypass at rest is bit-exact.** With no reduction the cascade is exactly the identity: no latency and no phase change.
- **Why not STFT.** A short-time Fourier transform would cost 85 ms of latency for the resolution the sub needs.

**Telemetry** feeds the masking map: per-band kick level, bass level and reduction, plus a before/after clarity measure (the overlap energy).

## Phases

1. Duck (Unmask and Duck modes, focus range, depth, margin, attack and release or follow-the-kick, lookahead), sub mono, the masking-map telemetry, and the clarity meter. Each comes with oracle tests.
2. Align: measure the kick and bass interaction in the sub; set a phase rotation and polarity that maximise summed energy where they overlap.
3. Tune (kick pitch and bass-note clash display) and Fill (harmonic restoration while ducked).
4. Timeline lookahead: ducking that knows the hits before they arrive, from MIDI clips or analysed audio, with no added latency.

The designed panel is centred on the masking map and built as for the other effects. Routing the sidechain needs the stage 3 integration work in ADR-0040.

## As built: phase 1

**Analysis.**
- 24 bands spaced 1.275× apart, each a two-stage band-pass with Q 2.78, together a fourth-order third-octave.
- The quadrature envelope is frequency-correct: low² + band² / r². The squared frequency ratio r² is the ratio of the two outputs' smoothed powers, so it holds for any tone in the band, not just at its centre.
- The power is averaged over one period of the band centre.
- Measured delay is 0.89 × (4Q/ω₀ + half a period): 37 ms at 49 Hz, 6 ms at 340 Hz.

**Laws.**
- Unmask cuts min(depth, bass + margin − kick), only where a cut can reveal the kick. Past the depth it tapers to zero over 12 dB.
- Duck follows the kick's level in each band over the top 24 dB of the kick's loudest band.
- Pump applies one cut across the whole focus.
- All three use attack and release, or Follow Kick (15 ms).

**Cuts.**
- 24 Simper bells with Q 3. Every 32 frames the bells that are cutting are solved as one active system and refined against the cascade's exact response, so each targeted centre lands on its target.
- A broad cut ripples under 0.6 dB. A lone cut reaches a third of its depth at the next centre.

**Lookahead.** 0–40 ms, a fixed latency. Each band's cut is held back by the lookahead minus that band's analysis delay, so it lands on its audio.

**Sub mono.** The side alone; the mid, which sums with the kick, is never filtered.

**Tests.** `tests/native_dsp/pocket_tests.cpp` covers:
- **Bypass:** bit-exact.
- **Laws:** Unmask, Duck and Pump on steady tones, within 0.6 dB.
- **Timing:** without lookahead the cut trails the law by the band's analysis delay (36 ms at 62 Hz); with 40 ms lookahead, by 4 ms. Its shape matches the law within 0.35 dB RMS.
- **Steady cuts:** distortion at −89 dB, against the input's −91 dB floor.
- **Latency:** exact.
- **Mono sub:** the mid is untouched.
- **Toggles:** click-free.
- **Telemetry:** reads true.

**Real kicks.** 11 Pulse kits, each tuned −12, −5, 0, +5 and +12 semitones, over a sub, a saw and a moving bassline: 165 pairs at the presets' 20 ms lookahead. The reference is non-causal Hilbert envelopes from ideal bands tiled at Pocket's centres. Results:
- **Clarity:** the median pair gets 0.68 of the law. Nine in ten get at least 0.3, and every large conflict at least 0.15.
- **Bass before the kick:** exact until 20 ms before it, then a pre-dip under 2 dB in the 10 ms before that.
- **Bass above the focus:** within 0.1 dB.
- **The kick's click above 1 kHz:** within 0.05 dB.

**Known limit: phase.** The cuts are minimum phase, so the bass left under a kick turns slightly. On 4 of 330 hits that thins the kick and bass sum below 250 Hz by more than 0.3 dB; the worst is 0.65 dB. Align (phase 2) is the cure.

**Listening.** One-off render tools (since removed) produced off and on comparisons from the native drums and synths.
