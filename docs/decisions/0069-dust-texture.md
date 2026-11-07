# ADR-0069 — Dust: a bed of background texture, and age for what passes through

**Status:** Accepted; engine, presets and panel built. **Date:** 29 October 2026.
**Builds on:** ADR-0040 (native effect platform), ADR-0058 (keying notes), ADR-0062 (denormals flushed), ADR-0067 (Flyby's use of Expanse as a room).
**Contract:** `include/oscillate/native_dsp/effects/dust.hpp` (layers, telemetry) and `src/native_dsp/effects/dust_table.hpp` (62 parameters).

## Context

Requirement: background noise beds (vinyl, tape, room) for minimal and dub techno. In those styles the bed under the track carries much of the atmosphere:
- crackle and surface noise from a record;
- tape hiss;
- mains hum;
- radio static;
- rain, wind and the air of a room.

A sample loop of these repeats audibly and does not follow the track. It does not duck under the kick, and it cannot age the part it sits under. The texture has to be generated, seeded so an export matches playback, and shaped by a handful of controls a producer can automate.

## Decision

1. **A native effect, Dust (id `dust`),** for any track or bus. It is a generator (it sounds with nothing playing in), reads a sidechain and is keyed by notes (`key_notes`, as Pump is).
   - The output is the aged input at `mix.input` plus the bed at `mix.bed`, through `mix.output`.
   - An output safety stage leaves everything to 0 dBFS untouched, then bends smoothly towards +6 dBFS. Anything not finite is silenced.
2. **Eight layers.** Each has On, Level (dB), Tone (dark to bright) and Width, plus its own character.
   - **Width:** each noise layer runs two independent generators of the same spectrum and places them at ±45° at full width. Width therefore decorrelates the channels without changing level. Events (pops, drops) are panned equal-power within the width.
   - **Vinyl:**
     - **Pops:** Poisson at the rate Crackle sets: `pop_rate_max · (pop_rate_span^c − 1) / (pop_rate_span − 1)`, none at 0.
     - **Pop sizes:** Pareto-distributed, the tail heavier as Dust rises (fine dust to big pops), clamped at 40 times the smallest.
     - **Pop sound:** a half-sine pulse (longer for bigger pops) heard directly, plus the decaying ring it excites in a band of 1–8 kHz. Bigger pops ring lower and longer; Tone brightens them.
     - **Fry:** a micro-crackle floor of hundreds of tiny clicks a second through a band.
     - **Surface noise:** its level comes round with the record.
     - **Rumble:** noise below 18–43 Hz with the tonearm's resonance, its level coming round with the platter.
     - **Scratches:** fixed places on the record, from the seed, that tick once a revolution at 33⅓, 45 or 78 rpm. Wear brings in more of them, raises the surface noise and adds a gentle tanh drive.
   - **Tape:**
     - **Hiss:** rising, with a broad peak that Tone moves from 3 to 9 kHz.
     - **Dropouts:** brief smooth dips (3–24 dB, 15–120 ms). The aged input dips too, and loses its highs in each dip, as tape does.
   - **Hum:** 50 or 60 Hz.
     - **Buzz:** crossfades from pure hum (the fundamental and a few weak harmonics) to a band-limited buzz: 120 phase-aligned harmonics of a rectifier and a clipped transformer, plus a fizz gated at the mains' peaks.
     - **Waveforms:** one-cycle tables built once and shared. The phase comes from the project frame.
     - **Width:** decorrelates only the third harmonic and up, so the fundamental stays centred.
   - **Radio:**
     - **Static:** AM-band static inside a 4.5 kHz channel, with sferics (Pareto-sized atmospheric crackles), fading slowly.
     - **Whistles:** heterodyne whistles from three seeded stations on the dial. Each whistles at its distance from Tuning and is fainter as it gets further away.
     - **Gate:** chops the layer on 1/4, 1/8 or 1/16 from the transport. A seeded 80% of steps open with a 2 ms attack, hold for `gate_hold` of the step and close over 6 ms to −20 dB.
   - **Rain:**
     - **Drops:** Poisson, 15 to 900 a second with Intensity. Radii are exponential about a mean that grows with Intensity.
     - **Drop sound:** a noise tick through the surface's resonance plus a little of it direct. Some drops add a Minnaert bubble: a decaying sine at 3.26 kHz·mm / radius whose pitch rises as it rings.
     - **Surfaces:** Glass rings bright; Roof thuds with a resonant body; Leaves are soft, dense and filtered, with no bubbles.
     - **Wash:** a bed of countless distant drops under the individual ones.
     - **Distance:** low-passes the rain and moves up to half of it into a room. The room is an Expanse inside Dust, as in Flyby: Room mode, wet only, 1.2 s.
   - **Wind:** three resonant bands wander slowly. They move higher and narrower as the wind blows harder, with low buffeting underneath. Gusts swell the strength. Past about 0.55 a whistle (a very narrow band) comes in.
   - **Room tone:** brownish noise below a corner that Size lowers, three faint room modes that Size spaces and that move slowly, and a whisper of air.
   - **Machine:**
     - **Fan:** a blade-pass tone with harmonics, over air pulsing at the blade rate.
     - **Fridge:** the compressor at twice the mains with its harmonics. It cycles on and off from the song position, with relay clicks and a spin-up, over a gurgle.
     - **Servers:** six fans a little apart (beating), panned across the width, with broadband air and a faint high whine.
     - **Speed:** scales them all.
3. **Calibration.** Each layer alone, at Level 0 dB and its defaults, sits at `layer_reference_dbfs` (−18 dBFS RMS): a bed, not a lead. Dusty 33's bed (vinyl and tape at −6 dB) sits about −21 dBFS.
   - **Stationary noise:** normalised by its filters' exact noise power. This is the mean of `|H|²` of the analogue prototypes, integrated on a logarithmic grid. The TPT filters match those prototypes at prewarped frequencies, so the level holds whatever the Tone.
   - **Rain drops:** normalised by their expected energy at the default Intensity, over 4,096 draws of the drop generator. More rain is then louder, as it should be.
   - **Pops:** sparse, so they are set by their peaks: the smallest at −34 dBFS, the biggest 32 dB above. The vinyl layer's level comes from the fry and surface under them.
4. **Age, on the input.**
   - **Band and drive:** a worn record's band (a high-pass rising from 20 to 240 Hz, a low-pass falling from 20 to 3.4 kHz) and a gentle tanh drive. Both are blended in over the first quarter of Age, so Age 0 is the input exactly.
   - **Wow:** a 0.5 Hz sine with a slow random drift, up to `wow_max_deviation` (0.6%).
   - **Flutter:** two irregular sines at 6.3 and 9.7 Hz, up to `flutter_max_deviation` (0.15%).
   - **Mono Below:** folds the side below its frequency to mono, through two cascaded second-order high-passes on the side. At 20 Hz it is off.
5. **Latency: reported, `latency_seconds` (2.5 ms; 120 frames at 48 kHz), fixed whatever the values.**
   - **Why it is not zero:** wow and flutter read a delay line modulated about a base delay. The aged input is therefore late by that base delay on average, even though the modulation is centred on it. Reporting zero would leave every Dust track 2.5 ms behind the others.
   - **What the host does with it:** it compensates, so the input lines up.
   - **The bed:** generated on the same compensated clock. The radio gate, the rotation and the key-note duck read the transport at `frame − latency`.
   - **A sidechain:** heard `latency` ahead, so the duck is already falling when the kick lands.
   - **Bypass-exact:** with no wow and no flutter the delay is a whole number of frames, read exactly. Init is the input delayed by the latency, bit for bit.
   - **The latency never changes,** so no edit needs a route rebuild.
6. **Duck: the bed only, never the input.**
   - **From a sidechain:** a peak follower with a 3 ms attack. A key at `duck_full_dbfs` (−12 dBFS) or louder ducks by the full amount, fading to none at `duck_floor_dbfs`.
   - **From key notes:** each note-on starts a 3 ms attack ending on the note (on the compensated clock), scaled by velocity.
   - **Release:** the time to recover nine tenths.
7. **Determinism.**
   - **Generators:** every generator (xorshift64*) is reseeded from `dust.mix.seed` and the project frame at every start or seek, so an export equals playback from the same point. The record's scratches, the radio's dial and the machines' build come from the seed alone.
   - **Control-tick decisions:** decisions that change state are taken at control ticks, which sit on fixed project frames. These are a layer going silent and resetting, and the rain's room running out its tail. The transport's beat is held to a 2⁻²⁴-beat grid, so any block split renders bit-identically.
   - **Seeks:** a seek fades the bed in over 10 ms and the last output out over 5 ms.
   - **Real-time safety:** nothing allocates on the callback, and denormals are flushed by the runtime (ADR-0062).
8. **Platform: no change needed.**
   - `generator`, `sidechain` and `key_notes` are independent flags. No host reads `generator`: hosts process every insert while it is in the chain.
   - Sidechain routing reads `sidechain`, and the stage refreshes key notes for any engine with `key_notes`. Nothing assumed the three flags were exclusive.
   - The platform contract skips its silence, seek and tail checks for generators, as it does for Signal.

## Consequences

- **Presets:** Dusty 33 (the defaults), Worn 78, Fresh 45, Cassette Deck, Late Night Radio, Static Pulse, Rain on the Window, Storm Front, Wind Through the Yard, Dub Chamber, Basement Hum, Server Room, Warehouse, Lo-fi Bed, Field Day and Init. Weather and space presets leave the input unaged.
- **Tests:** `tests/native_dsp/dust_tests.cpp` checks:
  - each layer's level and spectrum;
  - the pop rate and the scratches' recurrence;
  - the gate against the tempo;
  - seeds, block splits and resets;
  - the duck on both sources;
  - the mono fold, and wow and flutter against their depth;
  - allocation-free processing.

  Golden fingerprints are pinned after the first build (`DUST_REBASELINE=1`).
- **Cost:** measured by `oscillate_dust_bench`; see [docs/performance/dust-effect.md](../performance/dust-effect.md).
- **Not yet done:**
  - the panel and its wiring are separate work;
  - the Fridge's cycle (10–40 s) means a short loop may sit entirely in its on or off phase;
  - presets are tuned by ear against the reference level, not yet fitted to measured reference recordings.
