# ADR-0117 — Ceiling: a true-peak mastering limiter, and noise-shaped dither on export

**Status:** Accepted and implemented. **Date:** 10 November 2026.
**Supersedes in part:** [ADR-0047](0047-classic-processors-as-engines.md), where Ceiling runs the classic limiter bit for bit.
**Builds on:** ADR-0040 (native effect platform), ADR-0108 (Edge's curve, oversampler and true-peak guard), ADR-0062 (denormals flushed).
**Contract:** `include/oscillate/native_dsp/effects/ceiling.hpp`, the table `src/native_dsp/effects/ceiling_table.hpp`, the DSP `src/native_dsp/effects/ceiling_dsp.hpp`; dither in `include/oscillate/export/dither.hpp`. No schema change.

## Context

Ceiling ran the classic limiter: a sample-peak detector over the lookahead window, a gain that dropped at once and released on one pole, and a hard clamp. It had three faults a mastering limiter cannot have:
- **Its true peak passed the ceiling.** It read sample peaks, so intersample peaks went through: up to 1.1 dB over at 16x on a drum loop, 1.5 dB on mastered tracks.
- **It distorted steady low notes.** Its gain followed every half cycle shorter than the lookahead: −60 dB THD+N on a 50 Hz tone 6 dB over the ceiling, and −65 dB SMPTE IMD.
- **It cut transients.** Every peak was turned down by the whole of its excess, so the attack of a kick lost more than its body.

Export rounded integer formats with flat TPDF dither and no choice of shape.

## Decision

1. **Evolve Ceiling in place.** Ceiling is still one device, the master's limiter, with the same id. The gain stage, clipper and guard below replace the classic limiter as its DSP. The classic `LimiterRuntime` stays in `native_dsp`: the graph's tests and benches use it as a plain latent processor, and the audit tool uses it as the baseline. The parameter table changes (greenfield, no migration):
   - Gain (0–24 dB), Ceiling (−30–0 dBTP) and Lookahead (0.5–10 ms, the latency, not automatable);
   - Release (5–1000 ms) and Character (0–100%);
   - Transients, Clip (0–6 dB) and Shape;
   - Link and Transient Link;
   - Target (−30 to −4 LUFS, for the panel).
2. **A true-peak gain stage.** Each channel's peak is read at 8x (`FinePeak`: seven phases of a 33-tap Kaiser-windowed sinc). It covers each interval between two frames, as Edge's guard does (ADR-0108). The gain each interval needs, in log gain, is held for the lookahead plus 20 ms, then averaged by two boxcars whose spans add up to the lookahead. The fall is then an S-curve that reaches each peak's gain exactly at the peak. The 20 ms hold keeps the gain still across a 25 Hz half cycle. The stage aims 0.02 dB under the ceiling, so the guard is left only the clipper's overshoot.
3. **Two envelopes: a fast peak stage that releases to a slow loudness stage.** A slow envelope follows the held gain:
   - it attacks over 4× Release and releases over 8× Release;
   - the fast envelope releases over Release toward that slow level, scaled by Character, and never further than the peaks allow.

   An isolated peak hardly moves the slow envelope, so the gain comes back quickly. A dense passage holds the gain near the slow level, so it neither pumps nor ripples. Character at 0 releases to unity.
4. **Transient-aware limiting is a control, not a mode.** Transients (0–100%) gives a detected onset up to 6 dB of room over the ceiling, and Clip (0–6 dB) gives every peak room. The gain stage lets peaks through up to that room, and the clipper rounds them.
   - **The onset detector** compares two followers of the frame's power. Both release over 100 ms; one attacks over 0.5 ms and one over 8 ms. An onset counts from 2 dB of lead and fully from 8 dB, and lasts about as long as the slow attack. The detector ignores the ripple of a steady low note.
   - **Sustain sits under the knee.** With any room, sustained material sits under the clipper's knee: Shape's knee, faded in over the first dB of room and at least 0.1 dB. So the knee bends only what the room lets past, and a steady tone stays clean.

   A Transient-aware mode would have been this same control fixed at one setting, so it is not a separate mode.
5. **An oversampled soft clipper.** Edge's curve and linear-phase half-band oversampler (ADR-0108) at 8x, with antiderivative antialiasing, under a ceiling of 1.
   - **With room:** a 9 kHz tone pushed 6 dB into it with a hard knee aliases under −90 dB re the tone.
   - **With no room** (Transients and Clip at 0), the clipper is a plain delay of the same length. The signal passes exactly and the oversampler sleeps.
   - **Switching:** opening the room wakes the oversampler. It fills its filters while the delay plays, then cross-fades over 20 ms; closing the room fades back.
6. **The true-peak guard** is Edge's, BS.1770 4x (`edge_dsp::TruePeakGuard`), 0.01 dB under the ceiling. The output's true peak, as the project's meter reads it, never passes the ceiling.
7. **Stereo.** Link shares the slow envelope's gain between the channels, and Transient Link shares the fast envelope's. Each blends a channel's own held gain toward the louder channel's in log gain, so it never asks less than either channel needs. Mid/side was not built: the links already keep the image, and no measurement asked for it.
8. **No multiband mode.** The transient-aware wideband stage already removes most of what multiband limiting is for: at −10 LUFS the top band's pumping is 2.4 dB against the classic 3.7 dB (1.2 dB in Punchy). A linear-phase split would add about 10 ms of latency and twice the CPU. It was not prototyped, so it ships only if a later measurement shows a win.
9. **Latency and tail.**
   - **Latency** is the fine peak's reach (16 frames), the lookahead less one, the clipper's round trip (92 frames) and the guard (55 frames at 48 kHz). That is 354 frames (7.4 ms) at the default 4 ms.
   - **Tail:** with room to clip, the clipper's filters ring 93 frames past their round trip. With none, Ceiling stops with its input.
10. **Telemetry and panel** (`qml/ceiling`):
    - **Telemetry:** the input peak (before Gain) and the output's true peak; the reduction and its bands (the strip view's); what the clipper and the guard took; the output's momentary, short-term and integrated loudness (since the last reset or Gain change); the latency.
    - **The panel**, in the device kit:
      - the reduction history over ten seconds with its value now and its largest;
      - the IN and OUT meters, with the ceiling drawn across OUT;
      - the loudness against Target. Learn moves Gain by the difference. Limiting makes loudness grow more slowly than gain, and the reading restarts at a Gain change, so a second Learn on the same passage closes the rest;
      - the Drive, Limit, Clip and Stereo cards.
11. **Presets:**
    - **Default:** half the onset room, a nearly hard knee, a 100 ms release to a 60% loudness stage;
    - **Transparent:** 150 ms, 80%, 30% transients;
    - **Loud Club:** +6 dB, −0.3 dBTP, 50 ms, full transients, 1 dB clip, softer knee;
    - **Streaming −14:** the default with a −14 LUFS target;
    - **Punchy:** full transients, a hard knee, half the transient link;
    - **Gentle Bus:** 6 ms, 250 ms, a full loudness stage, a −18 LUFS target.

    On a −14 LUFS drum loop none takes more than 10 dB off a peak.
12. **Dither belongs to export, not the limiter.** Only the writer knows whether the bit depth is reduced, so dither there is off by construction for float WAV and MP3. A limiter that dithered would dither twice, or dither a float master. `DitherQuantizer` adds TPDF noise of ±1 LSB before rounding and shapes it by error feedback, with noise transfer 1 − Σ hₖ z⁻ᵏ:
    - **Flat:** plain TPDF, the library default, bit-identical to the former writer.
    - **Shaped:** Wannamaker's three-tap E-weighted filter.
    - **Psychoacoustic:** Wannamaker's nine-tap F-weighted filter, 18 to 25 dB under flat from 2 to 5 kHz; the export panel's default.

    At 88.2 and 96 kHz both shaped choices take a sixth-order filter with zeros at 5, 12 and 18 kHz, at least 6.9 dB and 10.3 dB under flat to 20 kHz. The fed-back error is held to ±1.5 LSB, so clipping cannot destabilise the loop. Integer WAV and FLAC masters take the choice from the export panel.

## Measured

`tools/limiter_audit` (`oscillate_limiter_audit`; `--classic` measures the classic limiter with Ceiling's former presets) drives each preset to equal integrated loudness under a −1 dBTP ceiling.
- **Programmes:** a synthetic drum loop (124 BPM kick, clap, panned hats, bass, pad), and 30 s excerpts of eight mastered melodic-techno tracks, each normalised to −20 LUFS before the limiter.
- **Trimming the classic limiter:** it lets true peaks through, so it is trimmed after rendering until its true peak sits at the ceiling, as it would have to be set to deliver it. Its overshoot is still reported.
- **Crest factor** is the 16x true peak over RMS in 100 ms windows.
- **Onset loss** is the change, input to output, of each detected onset's first 15 ms over its 30–150 ms body, below 150 Hz, 150 Hz–2.5 kHz and above 2.5 kHz.
- **Pumping** is the RMS of the top band's (above 5 kHz) gain once its 1 s trend is removed.
- **Tones** peak 3, 6 and 9 dB over the ceiling at the three targets.

The full tables are in [the native-effects performance note](../performance/native-effects.md#ceiling-at-equal-loudness). In summary, at equal loudness and an equal true-peak ceiling:
- **On the eight tracks at −9 LUFS,** Default against the classic Loud:
  - crest loss falls from 1.15 to 0.66 dB;
  - mid-band onset loss falls from 1.10 to −0.02 dB;
  - pumping falls from 1.77 to 1.73 dB, and to 1.14 dB in Punchy.
- **On the drum loop at −10 LUFS,** Default against the classic Default:
  - crest loss falls from 1.16 to 0.88 dB;
  - onset loss falls from 0.67/1.23 to 0.08/0.47 dB;
  - pumping falls from 3.70 to 2.42 dB.
- **Tones 6 dB over the ceiling:**
  - THD+N on 50 Hz is −150 dB, against −60;
  - SMPTE IMD is −111 dB, against −65;
  - 1 kHz and CCIF are at the numerical floor for both.
- **True peak.** It never passes the ceiling at 4×, where the classic limiter went over by up to 1.5 dB.
- **Loudest reached on the tracks:** Punchy reaches −7.5 LUFS where the classic Loud stops at −8.0.
- **Where it does not win:** at −14 LUFS, where neither limits much, crest loss on the loop is 0.02 dB higher, from the gain stage's margin.

## Consequences

- **Tests:**
  - `native_ceiling_contract` covers:
    - the true peak under the ceiling for every preset at three ceilings, every rate and a gliding ceiling;
    - clean tones, and distortion against the classic limiter;
    - onsets kept by Transients, and the alias floor;
    - stereo link and the latency;
    - the exact delay with no room, and a click-free room change;
    - block splits, export against live and a seek;
    - hostile input, restrained presets, an allocation-free callback and telemetry.
  - `export_dither_contract` checks each shape's noise against its curve at every rate, the feedback's bound when clipping, the flat path against seeded TPDF, and the writer's use of the shape.
  - `quick_ceiling_panel_contract` adds the loudness readouts, what the clipper and guard took, and Learn.
  - The routing and export fixtures set Ceiling with no room, so it stays an exact latent stage. Their latencies come from `ceiling::latency_frames`, and their oracles are the engine itself.
- **CPU** (`oscillate_ceiling_bench`, i7-10700K):
  - one instance takes 7.0–7.1% of a core at 64 and 128 frames, with a 99th-percentile block of 8.4–10.2% of its period and whole-process CPU equal to the mean;
  - sixteen in a chain take 110–114%;
  - with no room to clip, it takes 2.5%: the 8x clipper is most of the cost and sleeps then.
  - The classic limiter took 0.08%.
- **The 4x guard is the meter's.** Measured at 16x, clipped material can still read up to 0.3 dB over the ceiling (1.1 dB for the classic limiter). A finer guard would need a longer interpolator, more latency, and a meter to match.
