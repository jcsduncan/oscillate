# ADR-0067 — Flyby: a sound passing the listener, properly

**Status:** Accepted; implemented. **Date:** 29 October 2026.
**Builds on:** ADR-0040 (native effect platform), ADR-0058 (keying notes), ADR-0065 (Nova's FX rack).
**Panel design:** [ui/flyby-mockup.html](../ui/flyby-mockup.html).

## Context

Requirement: move a sound like a train passing, from one ear through the centre to the other, convincingly. A panning LFO only moves a level difference. A real pass-by also changes:
- the pitch (Doppler);
- the level and brightness with distance;
- the time between the ears and the head's shadow;
- how much of the room you hear against the source.

All of these change at once, from one moving position.

## Decision

1. **A native effect, Flyby (id `flyby`),** for any track or bus. The path is in metres around the listener, who faces +y:
   - a **Pass**, a cubic Bézier through four draggable points;
   - an **Orbit**, a circle with a direction;
   - either one at a **Height**.

   The input is summed to mono, because a moving source is a point.
2. **Motion.**
   - **Loop** runs round at Speed on the song's clock.
   - **Once** makes one pass from each note-on of the tracks keying Flyby (`key_notes`, as in Pump). Between passes the source rests at the start, or is silent.
   - **Sync** makes one pass every Length (1/4 bar to 16 bars) of the song.
   - **Position** places the source wherever automation says.

   Loop and Sync come from the song position, so an export equals playback.
3. **Doppler, properly.** Each ear reads one delay line at the source's emission time. That time is found by iterating `delay = |P(u − u′·delay)| / c`, so the pitch heard is `f / (1 + v_r / c)` with `v_r` taken at emission. At 380 km/h the simpler delay from the current distance would be about 170 cents flat.
   - **Timing:** the travel time is measured from the path's closest approach, so the moment of passing lands where it is placed.
   - **Doppler amount:** Doppler scales that travel time, from 0 to 200%.
   - **Wraps and restarts:** a jump in the travel time, from a loop wrapping or a pass restarting, crossfades the read heads over 20 ms instead of sweeping the pitch.
4. **The head (Headphones).**
   - **Ear timing:** Woodworth's formula, `(a/c)(θ + sin θ)` from the lateral angle, so overhead sources have none.
   - **Shadow:** Brown and Duda's first-order spherical-head filter on each ear, using that ear's angle to the source.
   - **Speakers:** a constant-power pan replaces both, since both ears hear both speakers.
5. **Distance.**
   - **Level:** `(Reference / max(d, Nearest))^Distance`, capped at +24 dB.
   - **Air:** absorption of about 1.5e-9·f² dB per metre (ISO 9613-1 at 20 °C and 50% humidity), as a one-pole low-pass at its 3 dB point. Air goes up to 400%.
   - **Room:** an Expanse inside Flyby, wet only and sized by Size. It is fed at a constant level, so the balance of source to room follows distance as it does in a real space.
6. **Latency:** half the widest ear-timing gap plus two frames, in whole frames: 18 at 48 kHz. The dry path is delayed to match, so Mix never combs.
7. **Nova.** The FX rack gains a sixteenth module, `flyby`. It drives the same kernel on preset paths: left to right, right to left, an arc over the front, an orbit, or overhead, at a closest distance. Inside Nova, Sync keeps time from the tempo, because the rack knows the tempo but not the song position. The rack's kernel driver now advances the beat it hands the kernels.

## Consequences

- **Presets:** Train, Car Pass, Racing, Jet Flyover, Bee, Circle Overhead, Vocal Throw, Fly-In, Slow Drift, Draw It Yourself and Train on Speakers.
- **Not yet done:** full HRTF elevation cues (pinnae) are left out. Height affects distance and ear timing only, and front/back is told apart only by the head shadow.
