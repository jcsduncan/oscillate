# ADR-0064 — Temper: a resonance suppressor with dynamic EQ bands

**Status:** Accepted; engine, presets and panel built. **Date:** 29 October 2026.
**Builds on:** ADR-0040 (native effect platform), ADR-0047 (Shape, Press, Ceiling), ADR-0058 (sidechain keys for effects).
**Panel design:** [ui/temper-panel-mockup.html](../ui/temper-panel-mockup.html).

## Context

Requirement: a dynamic resonance suppressor, with soothe3 (oeksound, May 2026) as the quality bar. soothe3 has these controls:
- Depth, plus Detail, which merges soothe2's sharpness and selectivity;
- Soft mode (with an adaptive threshold) and Hard mode;
- Tilt, which leans Detail, Attack and Release toward the lows or highs;
- Max Cut;
- low-latency, minimum-phase and linear-phase processing;
- a drawn sensitivity curve, and Delta listening.

Temper also adds four things soothe3 lacks:
- dynamic EQ bands on the same display;
- sidechain unmasking;
- a transient/sustain split;
- a live reduction heatmap.

Shape (ADR-0047) stays the static EQ.

## Decision

1. **A new native effect, Temper (id `temper`)**, on the existing platform. Its state is the parameter table alone, with no document, so it needs no schema change. The table holds:
   - nineteen global controls;
   - a six-node sensitivity curve (frequency and ±12 dB each);
   - eight dynamic bands, each with on, kind (bell, low shelf or high shelf), frequency, Q, range (±24 dB), threshold and ratio. All bands are off by default.
2. **Analysis, each hop:** a Hann-windowed STFT of the processed channels measures 240 bands, each a 24th of an octave from 20 Hz. Two real channels are packed into one complex transform.
   - **Suppressor:** each band is compared with its neighbourhood, whose half-width runs from 1.5 octaves down to a quarter octave as Detail rises. Tilt leans Detail across the spectrum.
     - *Soft* compares against the neighbourhood's power mean, so only what truly stands out is cut (an adaptive threshold), with a soft knee.
     - *Hard* compares against the mean level, with a 1 dB threshold and more weight.
   - **Weighting:** the excess is scaled by Depth (10 removes it all), by the sensitivity curve (+6 dB doubles it) and by Split. Split weights a band's cut by whether that band is in an attack, meaning it is above its own 100 ms average, with Separate setting how far above.
   - **Shaping:** the cut is smoothed across frequency (narrower as Detail rises), capped at Max Cut, and blended between the channels by Link.
   - **Unmask** adds up to 12 dB of cut in each band where the key is present and not drowned out by the input. Focus sets how strictly the key must dominate before it counts. A silent key carves nothing.
   - **Ballistics:** every band follows Attack and Release, leaned by Tilt.
   - **Dynamic bands:** each band measures the energy under its own shape and moves toward `range` by `(level − threshold)(1 − 1/ratio)`, never past `range`, with the same attack and release. A positive range boosts.
3. **Filtering.** The gain curve (with Mix blending gains, so a half mix never combs) becomes one FIR per hop. The Phase setting picks the path:

   | Phase | Filter | Latency at 48 kHz | Notes |
   |---|---|---|---|
   | Natural | minimum phase (folded cepstrum), 2048 taps, overlap-add | 768 frames (16 ms) | The analysis looks a quarter window ahead. |
   | Linear | linear phase (windowed), 2048 taps | 1280 frames (26.7 ms) | Suited to parallel and mid/side work. |
   | Low Latency | minimum phase, 256 taps, applied sample by sample, crossfaded across each hop | none | |

   Sizes double with each doubling of the rate. Phase changes the latency, so it is not automatable, and a change rebuilds the route (ADR-0040).
4. **Channels:** Stereo, Mid, Side, Left or Right. The unprocessed component passes untouched, delayed by the latency. **Delta** plays the input less the output, so Delta plus the output is the delayed input exactly. Trim glides.
5. **Telemetry for the panel**, per display band (1/12 octave): the input, output and key spectra, and the gain applied now, which the panel scrolls into the heatmap. Also: peaks, the deepest cut, each dynamic band's gain and detected level, the share of the spectrum in an attack, and the latency.
6. **Sixteen presets:** vocals (harshness, de-ess, mud), drums (cymbals, ring control, snare ring), bus and master (both linear phase), unmask for kick and for vocal, synth fizz, live low latency, room resonance and side harshness.

## Consequences

- Temper is CPU-heavy by design (sound first). Natural and Linear run about six 4096-point transforms per 256-frame hop at 48 kHz. Measured (effects bench, one core, 48 kHz): about 9.5% in Natural, 6% in Linear and 5% in Low Latency.
- Preset thresholds and sensitivity curves are starting points, not yet fitted to measured references (vocal, cymbal and drum recordings).
- **Tested** by the platform contracts (block splits, silence, tails, seeks, no allocation) and by Temper's own oracles (`native_temper_contract`):
  - at rest it passes the input exactly, delayed;
  - latencies are exact;
  - a 3 kHz ring over noise is cut by more than 6 dB while the noise an octave away moves less than 0.5 dB;
  - Max Cut holds;
  - Depth, sensitivity and Hard steer the cut;
  - Split spares onsets;
  - Unmask carves only under a playing key;
  - dynamic bands take their range above threshold and nothing below;
  - stereo modes leave the rest bit for bit;
  - Delta plus the output is the input;
  - telemetry places the dip at the ring.
