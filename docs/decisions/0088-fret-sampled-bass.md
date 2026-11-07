# ADR-0088 — Fret: a sampled bass guitar with Feel, realtime auto-articulation

**Status:** Accepted; implemented (engine, Feel, library installer, panel). The Kick & Bass Lock follower is still to come.
**Date:** 5 November 2026.
**Builds on:** ADR-0071 (Concert's streamed sample library), ADR-0068 (Knock's in-kernel groove), ADR-0072 (Kick & Bass Lock).

## Why

Requirement: a realistic sampled bass guitar built on a good, openly licensed sample library. Three Karoryfer libraries were auditioned through an offline renderer, on an original eight-bar line:
- Black and Blue Basses, fingered;
- Black and Blue Basses, picked;
- Fashionbass.

**Black and Blue fingered** was chosen. A second requirement is auto-articulation, so that flat MIDI sounds played without hand-drawn velocities, running **in realtime rather than written into the MIDI**.

## Decisions

### The library: Black and Blue Basses, "darkblack" fingered

- **What:** a fingered five-string hollowbody by Karoryfer Samples (D. Smolken). It is **CC0 1.0**: the licence file ships in the repository, and GitHub's licence record agrees. Pinned to sfzinstruments `karoryfer.black-and-blue-basses`, commit `6e7d674cdb41be7a54dbccb15472401ad01099b9`, tree `a5509d29a3a7070a447458bd140de64e72b23b75`.
- **Used:** the darkblack articulations, every semitone from B0 to E4 (sounding; the SFZ maps them an octave up, key 35 = B0 at 30.9 Hz, measured):
  - `reg`: plucks, 4 velocity layers (p, mp, mf, f) × 4 round robins;
  - `ghost`: muted ghost notes, 2 layers (p, f) × 4 round robins;
  - `stac`: staccato plucks, 4 round robins;
  - `btb`: behind-the-bridge plucks, 4 round robins;
  - `rel`: string releases, 4 round robins.
- **Not used:** the hollowbody's "btb_extra" set and the picked "babyblue" bass. Either can be added later as more articulations of the same format.
- **Measured:** 44.1 kHz, 24-bit mono, 5 s sustains, tails reaching −92 dBFS. Pitch holds: a hard E1 starts 20 cents sharp and settles to 7, as a real string does. Fashionbass, by comparison, starts a semitone sharp.
- **Attribution** isn't required by CC0. The panel still credits "Black and Blue Basses by Karoryfer Samples, CC0 1.0".

### A shared sample-streaming core, factored out of Concert

Concert (ADR-0071) built the right machinery for sampled instruments too large for memory:
- a raw pack read with positioned reads;
- each note's start kept resident, with the rest streamed into per-voice rings by one immortal process-wide streamer thread;
- generation-tagged atomic words, so the callback never waits;
- `await_content()`, so offline renders never outrun the disk;
- immortal library registries.

That machinery moves out of `concert_library.{hpp,cpp}` into a shared `native_dsp/synth/sample_stream.{hpp,cpp}`, and Concert keeps only its piano-specific manifest and voicing. The shared part is:
- **`sample_stream::Pack`.** An open pack file (`O_RDONLY | O_CLOEXEC`) with its channel count (1 or 2), its rate, and per-sample `{offset, frames, resident, resident_at}`, plus the resident store of int16 frames.
  - **Channels:** Concert's pack is stereo and Fret's is mono, so `bytes_per_frame = channels × 2`.
  - **Checks:** the pack's size must match the manifest, and every sample must lie inside it.
- **Registries.** Immortal, keyed by serial: one per instrument (`Registry` instances named by the engine), so Concert's 255-library budget is not shared with Fret.
- **`sample_stream::Streams`, `StreamSlot`, and the immortal `Streamer` thread.** These are unchanged in behaviour. Rings hold `ring_frames × channels` int16 values.
- **The windowed-sinc resampler:** Concert's 32-tap Kaiser (β = 8) table with 256 phases and its row interpolation, as `sample_stream::SincTable`.
- **The untrusted-manifest helpers:** `fields`, `parse_number` and `read_exactly`.

Concert's behaviour, tests and goldens are unchanged by the move.

### Fret's installed format

`tools/fret/install_black_and_blue.py` (Python standard library, git and ffmpeg) installs the library. It follows Concert's install script throughout:
- **Fetch:** it fetches the pinned commit and checks the tree id.
- **Mapping:** it reads the darkblack maps (`Programs/maps/darkblack_*_map.sfz`) for every sample's key, layer and round robin, and verifies the file set against them.
- **Conversion:** each WAV is converted to 48 kHz with ffmpeg's soxr resampler. The silence before the attack is trimmed to 2 ms (−40 dB re peak) and faded in, then each sample is peak-normalised and TPDF-dithered to int16 **mono**.
- **Output:**
  - `fret.pack`: int16 mono at 48 kHz, each sample aligned to 4 KiB;
  - `fret.manifest`: see below;
  - the licence and an `ATTRIBUTION.txt`.
- **Location:** `$OSCILLATE_FRET_LIBRARY`, else `$XDG_DATA_HOME/oscillate/instruments/fret/black-and-blue-darkblack` (`~/.local/share` by default). The new library replaces an old one only once complete.

The manifest, first line `oscillate fret library 1`, then:

```
name Black and Blue Basses (darkblack, fingered)
attribution Black and Blue Basses by Karoryfer Samples, CC0 1.0
rate 48000
channels 1
pack fret.pack <bytes>
sample <articulation> <key> <layer> <layers> <round_robin> <offset> <frames> <scale> <peak_db>
```

- **`articulation`:** one of `pluck`, `ghost`, `staccato`, `bridge` or `release`.
- **`key`:** the **sounding** MIDI key, 23 (B0) to 64 (E4). Each key is its own sample; there is no repitching between keys.
- **`layer`:** 0-based, softest first, of `layers` for that articulation and key.
- **`round_robin`:** 0-based.
- **`offset` and `frames`:** in pack frames.
- **`scale`:** int16 to full scale.
- **`peak_db`:** the recording's peak before normalisation, so layers keep their recorded levels.

The reader bounds everything as Concert's does. Every key from 23 to 64 must have at least one `pluck`.

### The engine: Fret

- **Id and keys.** The id is `fret`, version 1. It plays keys 23–64; others are ignored, as on the instrument. Note-offs end notes.
- **Polyphony.** Up to 8 note voices and 8 release voices, in fixed arrays with no allocation on the callback. A key struck again fades its earlier sound over 15 ms (the finger stops the string).
- **Velocity layers.** A note plays the layer whose range holds its velocity, like the SFZ's hard split. Within a layer, level is linear in velocity up to the layer's top (the SFZ's `amp_velcurve_<hivel>=1`). The four pluck layers split at 31, 63 and 95; ghost layers split at 63.
- **Round robins.** Each key and articulation counts its own hits, `round_robin = hits mod count`. The count is reset by the transport generation, so export equals playback, and any block split is identical.
- **Release noises.** The string's release sample sounds at note-off at **Release** level, round-robined. 50 % is the library's voicing.
- **Slides.** A note that starts while another is held, within an octave and with **Slide** above zero, does not pluck. The held voice glides to the new pitch over Slide's time (up to 200 ms), through the resampler, and carries on as the new key, as a finger slides up a string. The new note's note-off ends it.
- **Resampling.** Pack frames are read at 48 kHz × the voice's pitch ratio (Tune ±50 cents and slides) through the shared sinc table. A note at its own pitch at 48 kHz is copied exactly.
- **Tone** (after the voices, mono, then centred):
  - **Amp:** blends the DI with an amp voicing: a cabinet-like band-pass (80 Hz to 4.5 kHz, second order) and a soft asymmetric saturator.
  - **Tilt:** first order about 700 Hz, ±6 dB.
  - **Squash:** a feed-forward compressor, 4:1 at full, 10 ms attack and 120 ms release, with auto make-up.
  - **Grit:** drive into the saturator.
  - **Level and safety:** Level, then Concert's safety knee.
- **Streaming.** The first 16,384 frames of every note are resident; releases stay resident whole. One stream slot per note voice. `await_content()` services the streams inline for offline renders.
- **No library.** Fret is silent and says why. The panel shows the install command, a copy button and Look Again, as Concert's does.

### Feel: auto-articulation in the kernel, in realtime

Feel decides each note's velocity and articulation **in the kernel, when the note starts**. Notes in clips stay as drawn, so every Feel control changes the sound while a loop plays, with nothing to rewrite. It reads only what is known at the note's start:
- its place in the bar, from the transport;
- its own length (`NoteEvent::frames`, known for clip notes);
- the tempo;
- its key and velocity;
- the note before it on this kernel.

It never reads later notes, so it behaves the same for clips and live playing.

**Rules.** The prototype, `feel.py`, was tuned by ear on the original line; each figure is a named constant.

- **Places.** A note within 1/48 beat of a grid point is at that place: the one, a beat, an eighth or a sixteenth.
- **Base velocity.** Each style gives every place a weight (below). The velocity is `Centre (92) + Dynamics × (weight − 0.75) / 0.25`.
- **Gestures.** Each adds a share of Dynamics:
  - **Run:** +0.15 per note of a sixteenth run, up to 4 notes, so a fill builds toward where it lands.
  - **After a rest:** +0.25 when the gap before the note is half a beat or more.
  - **Octave pop:** +0.35 when the note jumps up an octave or more.
  - **Push:** +1.0 for an off-beat (eighth or sixteenth) note held past the next eighth.
- **Ghost.** A short note (a sixteenth or less) on an off-beat that repeats the previous note's key, and isn't a pop, plays the **ghost** articulation at velocity 44 (Ghost Notes: Auto).
- **Staccato.** Any other short note on an off-beat plays **staccato**.
- **Live notes** (length unknown, `frames == 0`) are never short. On an off-beat repeat of the previous key, a note played at velocity 50 or below is a ghost, so a soft touch still ghosts.
- **Humanize.** It adds ±Humanize velocity, seeded by the note's place in a two-bar cycle and its key. A loop therefore repeats its groove exactly, and export equals playback.
- **Amount.** It blends between the note's own velocity (0) and Feel's (1). Below 50 % every note keeps the pluck articulation.
- **Pocket.** It plays off-beat notes 0–20 ms late (the one and the beats stay put). Late notes use Knock's pending-hit queue (ADR-0068 §18), keyed to the kernel's own frame clock, so any block split renders the same samples. Ahead of the beat is not offered, because it would need lookahead.

| Style | the one | beats | eighths | sixteenths | default Pocket |
| --- | ---: | ---: | ---: | ---: | ---: |
| As drawn | — | — | — | — | 0 ms |
| Funk | 1.0 | 0.8 | 0.7 | 0.5 | 0 ms |
| Driving | 1.0 | 0.9 | 0.85 | 0.6 | 0 ms |
| Laid back | 0.9 | 0.8 | 0.75 | 0.65 | 8 ms |

"As drawn" plays the notes' own velocities and articulation Auto plays plucks. Feel is a pure function, `fret::feel::decide(const Context&, const Previous&, const Settings&)`, so it is tested without the engine.

**Articulation.** Auto lets Feel choose per note. Pluck, Staccato, Ghost or Behind the bridge forces one articulation on every note, for a muted, staccato or bridge part.

### Parameters

| Id | Name | Range | Timing |
| --- | --- | --- | --- |
| `fret.feel.style` | Style | As drawn, Funk, Driving, Laid back | next note, not automatable |
| `fret.feel.amount` | Amount | 0–100 % | live |
| `fret.feel.dynamics` | Dynamics | 0–30 | live |
| `fret.feel.pocket` | Pocket | 0–20 ms | live |
| `fret.feel.ghosts` | Ghost notes | Off, Auto | next note |
| `fret.feel.humanize` | Humanize | 0–10 | live |
| `fret.articulation` | Articulation | Auto, Pluck, Staccato, Ghost, Behind the bridge | next note |
| `fret.release` | Release | 0–100 % | live |
| `fret.slide` | Slide | 0–200 ms (0 off) | live |
| `fret.tone.amp` | DI ↔ Amp | 0–100 % | live |
| `fret.tone.tilt` | Warm ↔ Bright | −6 to +6 dB | live |
| `fret.tone.squash` | Squash | 0–100 % | live |
| `fret.tone.grit` | Grit | 0–100 % | live |
| `fret.tune` | Tune | ±50 cents | live |
| `fret.level` | Level | −60 to +6 dB | live |

**Presets:** Funk Pocket (default: Funk, Dynamics 20, Release 60 %, Amp 35 %, Squash 45 %), Straight DI (As drawn, all tone off), Driving Rock, Laid Back Soul, Muted Thumb (Ghost forced, Amp 50 %) and Bridge Bite (Behind the bridge, Grit 30 %).

### The panel

The panel:
- **Feel:** the four style tiles; a two-bar lane showing the notes about to play, coloured by articulation (pluck, staccato, ghost), with pushed notes ringed; Amount, Dynamics and Pocket; Ghost notes Auto or Off.
- **Articulation and Tone cards.**
- **Library plate:** credit, state, install command.

A "Write feel into the clip" button is dropped: Feel is realtime and stays in the instrument.

## Build order

Each step is committed with its tests:
1. `sample_stream` extracted, with Concert ported onto it.
2. The install script and its synthetic test library.
3. Feel, as a pure function with an oracle test against the prototype's decisions on the original line.
4. The engine.
5. The panel.
6. The catalog entry and the Kick & Bass Lock follower (Fret is a bass).

## Not verified yet

The tone stage's voicing and the default Dynamics are by ear on headphones only, and so are the slide time and Pocket feel. CPU and underruns must be measured with whole-process CPU at real-time pace, light and heavy, p99, before the engine ships.
