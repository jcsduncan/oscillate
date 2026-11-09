# ADR-0116 — Hush: a gate and downward expander

**Status:** Accepted and implemented. **Date:** 9 November 2026.
**Builds on:** ADR-0040 (native effect platform), ADR-0043 (one native insert path), ADR-0092 (native inserts read their key), ADR-0103 (shared device panel parts).
**Contract:** `include/oscillate/native_dsp/effects/hush.hpp`, the table `src/native_dsp/effects/hush_table.hpp`.

## Context

Multi-mic drum kits need their bleed tightened, gated reverbs need a gate keyed by another track, and quiet sources need their noise floor lowered between phrases. None of the built-in effects does this: Press compresses above its threshold, Strata's lower band expands but has no hold, hysteresis or lookahead, and Chop gates on the grid, not on the signal.

## Decision

1. **A native effect, Hush (id `hush`),** on the effect platform: a parameter table and a kernel, no document, a sidechain input. Parameters: Threshold (−80 to 0 dBFS), Hysteresis (0 to 20 dB), Range (−80 to 0 dB; −80 reads −∞ and mutes), Mode (Gate or Expander), Ratio (1.1 to 10), Attack (0.1 to 100 ms), Hold (0 to 500 ms), Release (1 ms to 4 s), Lookahead (off, 0.5, 1, 2, 5 or 10 ms), Key (Input or Sidechain), Key HP and Key LP (20 Hz to 20 kHz), Listen, Stereo Link (0 to 100 %) and Flip.
2. **The key.** The key is the routed sidechain when Key is Sidechain and one arrives, otherwise the input. It passes a fourth-order Butterworth high-pass and low-pass (24 dB/octave each), so a kick mic's gate can ignore the hats and a snare's the kick. A peak detector follows it, rising at once and falling with a 10 ms time constant. Stereo link moves each channel's detector towards the louder channel's; fully linked, both channels share one gain.
3. **The gain computer.** A state per channel: it opens when the detector reaches the threshold and closes once the detector has stayed under threshold − hysteresis for the hold time, so a key hovering at the threshold switches once and never chatters. Open, the target gain is 0 dB. Closed, it is the range (Gate) or `(level − threshold) · (ratio − 1)` dB, no lower than the range (Expander). The gain moves towards its target in decibels at a constant rate: Attack is the time from the range to unity and Release from unity to the range, so no edge steps and the timings are exact. With a muting range the gain moves between 0 and −120 dB and is exactly 0 at the bottom. Flip mirrors the gain within the range, so the key ducks the input.
4. **Lookahead** delays the audio and the listened key, not the detector, so the gate opens before the transient arrives. The delay is the latency the engine reports; a change of it takes the route's rebuild path, so it is not automatable.
5. **Listen** plays the filtered key in place of the gated audio, crossfading over 20 ms.
6. **Telemetry:** input and output peaks per channel (held 1 s), the key's detector level, the gain reduction (held 0.3 s; 80 dB reads a mute), the gate's state, both thresholds, the latency, and a scrolling view of the last 3 s in 128 slices, each the input's peak at the output's clock and the least gain applied in it.
7. **Presets,** each measured on the source it is for: Tight Kick (keyed under 200 Hz, 1 ms lookahead, mutes), Snare Bleed (key 150 Hz to 6 kHz, −30 dB), Tom Bleed (key 60 Hz to 1.5 kHz, long release, −24 dB), Gated Verb Key (sidechain key, 250 ms hold, 40 ms release, mutes), Hum Gate (key above 200 Hz, −20 dB), Soft Expander (1.5:1 down to −15 dB) and Vocal Breath (2:1 down to −10 dB). Each lets its hits through within 1 dB and lowers the gaps by no more than its range.
8. **Panel** (`qml/hush`): the scrolling view with the input as a ghost under what the gate let through, the open threshold solid and the close threshold dashed; the input, reduction and output meters and the state; Threshold, Hysteresis, Range and Ratio (idle for a gate); Attack, Hold and Release with the lookahead and its latency; the mode, flip and stereo link; the key source, its filter, listen, and a badge when a sidechain keys it.

## Consequences

- **Tests:** `native_hush_contract` (the closed gain is exactly the range and a mute is silence, open is bit-exact, no chatter at the threshold and chatter without hysteresis, attack, hold and release timed to a millisecond, the expander's curve, lookahead ahead of the transient and its latency, the key filter, the external key, listen, stereo link, flip, block splits, export against live, hostile input, restrained presets, a seek starts clean, no allocation, telemetry) and `quick_hush_panel_contract` (every parameter has a control, the text fits, controls, telemetry, the real Hush, presets; screenshots with `HUSH_PANEL_SCREENSHOT_DIR`). `effect_export_parity` and the platform contracts cover it with every engine.
- **CPU:** per channel and frame, eight biquad sections in the key filter, the detector, the state and one `pow` when the gain moves; a steady gain reuses the last value.
