# ADR-0033 — Drift: granular texture synth (seventh native engine)

**Status:** Accepted; implemented. **Date:** 19 October 2026.
**Builds on:** ADR-0027 (the native-synth platform), and ADR-0028 to ADR-0032.

## Goal

Drift turns any chord into a playable, evolving ambient texture. It is beautiful by default, deterministic, and needs no assets.

Like every native engine, Drift adds no project-format, session, playback or export changes.

**Non-goals for v1:**
- user samples or audio input;
- live grain telemetry to the UI;
- MPE;
- a spectral (FFT) resynthesis engine;
- a reverb freeze, because a frozen reverb tail has no tail bound and its export would never end.

## Voice

Drift plays 12 notes, and each note is its own grain cloud.

- **Source.** Each note writes a rolling 4 s buffer at the note's pitch. The buffer mixes three layers:
  - **Wavetable:** Lumen's generated tables and its wavetable oscillator, with table, position and slow drift.
  - **Partials:** an additive bank of 16 partials with tilt, odd/even balance and inharmonic stretch.
  - **Air:** noise through a band-pass that tracks the note.
- **Grains.** Up to 32 grains per voice, from a fixed pool.
  - **Size:** 5–500 ms.
  - **Density:** 0.5–200 grains/s, with chaos: how irregular the spacing is.
  - **Shape:** Hann, Tukey, Decay or Swell.
  - **Position:** how far behind the write head grains centre.
  - **Scan:** −2 to 2. 1 keeps pace with the writer; 0 holds an absolute buffer point, stretching time.
- **Grain pitch.** A base offset (±24 st), plus random intervals (off, octaves, fifths or pentatonic) taken with a set probability, plus jitter in cents.
- **Spread.** Reverse probability and stereo spread.
- **Spectral blur.** One Blur macro deepens spray, jitter and reverse together, on top of their own knobs. The mod wheel adds Blur too.
- **Freeze.** A switch that stops writing, so grains keep scanning the held moment for as long as the note sounds. It can be automated as a stepped lane (ADR-0034), like every discrete parameter.
- **Tempo sync.** Grain triggers lock to a division from 1/32 to 1 bar.
- **Rest of the voice:**
  - an amp ADSR, with velocity;
  - a post-grain multimode filter (LP 12, LP 24, BP 12 or HP 12);
  - two LFOs: one to position and density, one to filter and size, each free or synced.

## Effects

Chorus, then a BBD delay (synced or free), then a shimmer reverb, then the output level and the safety knee.

- **Shimmer reverb** is a new 8-line feedback delay network. Controls: size, decay (0.5–30 s), damping, shimmer amount and mix.

## Quality bar (tested)

- **Grains (COLA).** Hann grains at overlap 2, spawned regularly from a constant buffer, sum to a constant within 1e-9.
- **Grain pitch.** A grain at rate r reading a sine of frequency f outputs r·f, within 0.5 cent.
- **Reverse grains** output the exact time-reversed segment.
- **Freeze.** A frozen buffer with scan 0, spray 0 and regular grains repeats with the grain period, frame-exactly.
- **Sync.** Synced grain onsets land on the exact frames for the tempo and division.
- **Partial bank.** Partial n sits at n·f·√(1 + B·n²), within 0.5 cent.
- **Clean-path pitch.** One partial, spray 0, no pitch offset: the engine output is within 0.5 cent.
- **Shimmer FDN.**
  - With shimmer 0 and no damping, the measured RT60 is within 10 % of the decay setting.
  - The pitch shifter turns f into 2f, with the residual at f below −30 dB.
  - The network stays bounded at every setting.
- **Inherited contracts.** Block-split invariance, export equal to live playback, no allocation in the callback, the tail bound, and deferral of next-note settings.
- **CPU.** At most 15 % of one core at 48 kHz with 64-frame blocks, for 12 notes, density 50/s and 200 ms grains, with shimmer on.

## Presets

16, with golden fingerprints:
- Nebula (the default), Glass Rain, Frozen Choir, Shimmer Halo;
- Dust Motes, Tidal Pad, Stutter Clock (synced), Reverse Bloom;
- Granite Drone, Aurora, Spectral Smear, Ice Cave;
- Pulse Grains (synced), Deep Field, Wind Organ, Init.

## UI

A Nebula panel: deep-space dark, with aurora gradients and glowing controls.

- **Hero visual.** A grain cloud with buffer position across and pitch up. It is a deterministic preview built from the current settings with the same scheduler law, not live telemetry. It shows the scan head, the spray width, reverse grains in a second hue, and a frost overlay when frozen.
- **Sections:**
  - Source, with a small spectrum sketch;
  - Grains, with the Blur macro;
  - large Freeze and Sync toggles;
  - Amp and Filter;
  - the LFOs;
  - Shimmer reverb, with a decay sketch;
  - Delay, Chorus and Output.

## Design notes

### Clock and seeding

Each voice counts integer frames from its note-on.

- **Free triggering.** Grain k+1 spawns max(1, round(rate / density × (1 + chaos·(2u − 1)))) frames after grain k.
- **Synced triggering.** Grain k spawns at ⌈k × frames_per_division⌉, with the same rebase on a tempo change as Torque's sequencer.
- **Random draws.** Every u comes from the voice's generator, seeded by its slot and the note's age, so live playback and export draw identical sequences.

### Grain reads

A grain of n output frames reads the buffer at rate r: forwards, or backwards when reversed. It reads with 4-point cubic interpolation.

- **Placement.** Its start is the scan centre plus spray, clamped so that every sample it reads has already been written and is not overwritten before it is read.
  - **Newest bound:** the start is at most W + n(1 − r) − 4 for a forward grain, or W − 4 − r·n for a reversed one. W is the write count at spawn.
  - **Oldest bound:** the start is at least W − L + 4 + n, where L is the buffer length.
  - **Frozen buffer:** W is fixed and nothing is overwritten.
- **Early in a note.** The buffer has not wrapped, so the oldest bound is 0. Grains at r ≤ 1 can start at once; higher grains wait until enough history exists.
- **Gain.** Grains are scaled by √(2 / max(2, density × size)). Sparse grains stay at unity, and dense clouds sum to roughly constant power. Hann grains at overlap 2 sum exactly to 1.

### Shimmer FDN

- **Lines.** Eight delays, from 29.7 to 67.1 ms (scaled by size), mixed by a Householder matrix (x − (2/N)·Σx).
- **Decay.** Each line has gain g_i = 10^(−3·d_i / (RT60·rate)), which gives an exact RT60 at DC.
- **Damping.** A one-pole low-pass in each line.
- **Modulation.** Slow ±0.3 ms delay modulation, read with cubic interpolation.
- **Shimmer.** Each line's feedback crossfades between the plain signal and an octave-up copy: g_i·((1 − s)·m_i + s·shift_i(m_i)).
  - The shifter is two delay taps sweeping at twice speed, 50 ms apart, crossfaded with complementary windows, so its gain is at most 1.
  - The loop gain therefore never exceeds g_i < 1, so the network is always bounded.
- **Stereo.** Even lines feed the left output and odd lines the right.

## Implementation decisions (a794d82)

- **Alias-free pitched grains.** Each voice writes its source through a chain of half-band decimators into mip levels at 1/2, 1/4, 1/8 and 1/16 of the rate. Every level holds the same 4 s.
  - A grain at rate r reads the level where its rate is at most 1. Octave and +24 grains therefore never fold: an oracle measures a near-Nyquist tone played an octave up, and its folded image is below −70 dB.
  - Grains at rate 1 or lower read the full-band level, so the clean-path pitch and overlap-add oracles stay exact.
- **Band-limited shimmer.** The octave shifter band-limits its input with an 8th-order Butterworth low-pass at 0.2 of the rate before doubling. Its folded image is below −60 dB.
- **Reverse fallback.** A reversed grain needs a whole span of history. Early in a note it plays forwards instead of being dropped, so slow-attack reverse presets still speak at once.
- **Freeze.**
  - **Engaging.** It engages on the control grid. The writer then runs on for one in-flight grain span (four grain lengths), so grains already playing never read stale data. Frozen grains avoid the oldest frames that this run-on overwrites.
  - **Notes started frozen.** A note started while Freeze is on captures 1 s first.
  - **Cost.** Once its guard is written, a frozen voice computes no source.
  - **Looping.** A frozen moment loops. Sprayed or scanned grains that pass either end wrap round the valid window instead of piling onto its edge. Live, grains still clamp to the newest audio.
- **Partials.** The partials are complex rotations (four multiplies a frame), renormalised every control tick, and recomputed only when pitch, stretch, tilt or balance change.
- **Seeding.** Each voice's generator is seeded by its slot and its note's age. Draws per grain are fixed in number, so enabling a feature never shifts the random sequence.
- **Voice retirement.** Voices retire on the control grid when their amp envelope is idle.
- **Filter type** applies from the next note.
- **CPU.** 11.2 % of a core for the budget case (target 15 %; see `docs/performance/drift-synth.md`).
