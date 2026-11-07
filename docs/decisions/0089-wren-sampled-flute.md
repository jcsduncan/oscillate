# ADR-0089 — Wren: a sampled concert flute with Breath, realtime phrasing

**Status:** Accepted; implemented (library installer, Breath, engine, panel).
**Date:** 6 November 2026.
**Builds on:** ADR-0088 (Fret: `sample_stream`, Feel in the kernel), ADR-0071 (Concert's sample library), ADR-0068 (Knock's pending-hit queue), and the project key reaching kernels (ADR-0068 addendum §20–21).

## Why

Requirement: a sampled concert flute. Three open libraries were auditioned through an offline renderer, playing one original eight-bar line (D dorian, 88 BPM) the same way, in the same room and at the same loudness:
- the University of Iowa MIS flute;
- the VSCO 2 CE flute (CC0);
- the Ixox flute (CC-BY 4.0).

**Iowa** was chosen as the best-sounding.

Fret's lesson carries over: notes are drawn flat and the instrument should play them like a player, in realtime, without rewriting the MIDI. For a flute, that playing is breath: phrasing, vibrato that blooms on long notes, swells, slurs, tonguing and grace notes.

## Decisions

### The library: University of Iowa MIS flute (pre-2012 session)

- **What:** Lawrence Fritts's recordings at the Electronic Music Studios. A concert flute in an anechoic chamber; one Neumann KM 84; 16-bit, 44.1 kHz, mono.
  - Chromatic scales played note by note, B3 to D♭7.
  - Three dynamics (pp, mf, ff), each with and without vibrato: 22 recordings.
- **Terms:** the MIS page says the recordings "may be downloaded and used for any projects, without restrictions". There is no formal licence. The installer records that statement and its URL in `ATTRIBUTION.txt`, and the panel credits "University of Iowa Musical Instrument Samples (Lawrence Fritts)".
- **Not used:** the 2014 session (24-bit/96 kHz stereo single notes), because it has only ff, and velocity needs three dynamics.
- **Measured:**
  - Cutting the runs at their silences gives 227 notes, 1.1–3.5 s long (median 2.2 s).
  - Keys are assigned by position in each run, and tuning comes from each note's measured pitch: the high register is played up to 56 cents off (the C7 notes about +51).
  - One ff note (non-vibrato D♯5) cracked to its twelfth and is dropped, and a duplicate pp take of key 71 is not used, leaving 225. A missing note plays its neighbour, repitched.
  - Some notes swell for over a second before their attack end (32 after 0.5 s, the longest 1.35 s); a slur, which starts at the attack end, skips that swell.
  - Pitch drift within a note is under ±10 cents, apart from vibrato.

### Installed format

`tools/wren/install_iowa_flute.py` (standard library plus ffmpeg) does the following:
- **Fetch.** It downloads the 22 AIFF files from the MIS site. Each is checked against a SHA-256 pinned in the script, since there is no git commit to pin; `--source` takes copies already downloaded.
- **Cut.** It cuts the runs as the audition did:
  - a 10 ms RMS envelope with an onset 18 dB and an offset 10 dB above the file's 10th-percentile floor, keeping notes of 0.3 s or more and 10 ms of pre-roll;
  - each note's pitch is measured 150–450 ms after its onset.
- **Convert.** It resamples to 48 kHz with soxr, trims the silence before the attack to 2 ms (−40 dB re peak) with a fade-in, then peak-normalises and TPDF-dithers to int16 mono, recording `peak_db`. Each sample's `scale` turns its int16 values back into the recording's level, so layers keep their recorded dynamics; `peak_db` is informational and the kernel does not apply it again.
- **Mark.** It finds each note's **attack end**: where the envelope first reaches 90 % of its steady level. Slurs start there.
- **Loop.** It finds a **loop**, so a held note can last beyond its recording:
  - in the steady part (from 40 % to 85 % of the note), the start and end are matched by waveform correlation, and the end is moved to a zero crossing;
  - in vibrato notes the loop spans a whole number of vibrato cycles, measured from the pitch track;
  - the loop is crossfaded over up to 120 ms into the pack (shortened to 90 or 60 ms on 23 high vibrato notes whose passages drift out of phase), so the kernel loops without a crossfade of its own;
  - loops are 0.50–1.08 s long, and each sample ends with a 5 ms fade-out.

The manifest's first line is `oscillate wren library 1`. It then lists `name`, `attribution`, `rate 48000`, `channels 1`, `pack wren.pack <bytes>`, and:

```
sample <vibrato|plain> <key> <layer> <offset> <frames> <attack_end> <loop_start> <loop_end> <cents> <scale> <peak_db>
```

- **`key`:** the MIDI key it was played as; `cents` is its measured offset, which the kernel corrects.
- **`layer`:** 0 pp, 1 mf or 2 ff.
- **Size:** the pack is about 46 MB, all resident. It uses `sample_stream::Pack` with `resident = frames`, so there is no stream slot and no disk access after loading.
- **Location:** `$OSCILLATE_WREN_LIBRARY`, else `$XDG_DATA_HOME/oscillate/instruments/wren/iowa-flute`.

### The engine: Wren

- **Id and keys.** The id is `wren`, version 1. It plays keys 59–97; outside that range a note is ignored.
- **Voices.** Mono (the default) or Poly 4.
  - **Mono:** a new note while one sounds is a **slur**. The new note starts at its sample's attack end and takes over by an equal-power crossfade over **Slur time** (default 45 ms). The old voice bends toward the new pitch over the first third of the crossfade, the way a finger change sounds. A note starting after the last has ended is **tongued**: it plays its full attack.
  - **Poly 4:** every note is tongued, for two flutes or chords.
- **Each voice** reads a **plain** sample and, when Breath gives the note vibrato, the **vibrato** sample of the same key and layer, from the same position. It blends from plain to vibrato with an equal-power curve that rises over **Bloom** after the **Bloom** delay, up to **Vibrato** depth. Both samples loop at their loop points while the note is held.
- **Dynamics.** Velocity picks the layer, with hard splits at 60 and 100, and trims within it, up to −6 dB at the layer's bottom.
  - **Within a note:** Breath's swell is a gain curve, with Air tracking the gain at 0.5 dB per dB, since a flute brightens as it gets louder. It is not a crossfade between layers, which would phase between different takes.
  - **Note-off:** a 90 ms release (40 ms for staccato). The voice keeps looping through it, because past the loop end the recording is the unbaked original, with a small seam.
- **Resampling.** The shared sinc table, at 48 kHz × the pitch ratio, with cents, Tune and the slur bend.
- **Tone** (after the voices):
  - **Air:** a high shelf at 5 kHz, ±6 dB.
  - **Warmth:** a low shelf at 350 Hz, ±4 dB.
  - **Space:** a built-in room, because the samples are anechoic and sound bare without one. It reuses the Expanse reverb core as a fixed small hall with Space as its mix, 0–100 % (default 30 %), and is turned off at 0. As built, it runs an Expanse kernel inside Wren's: the Hall at size 0.4 with the prototype room's 2.6 s decay and 22 ms pre-delay. Expanse's reset now also restarts its modulation, so a reset hall plays exactly as a new one and export equals playback after a seek.
  - **Level and safety:** Level, then the safety knee.
- **No library.** Wren is silent and says why, with the install command, as Fret does.

### Breath: Feel for a flute, in the kernel

Breath decides each note's playing **when it starts**, from what is known then:
- its place in the bar and the tempo;
- its own length (`NoteEvent::frames`; 0 for live notes);
- its key and velocity;
- the previous note on this kernel;
- the project key.

Like Fret's Feel, it is a pure function, `wren::breath::decide(const Context&, const Previous&, const Settings&)`, tested without the engine. The prototype is `flute_feel.py`, tuned on the audition line; every figure below is a named constant.

- **Velocity.** `Centre + Dynamics × (weight − 0.8) / 0.2`, where the weight is the style's for the note's place (the one, a beat, an eighth or a sixteenth). Then:
  - **long note:** + Dynamics × 0.35 for a beat or more;
  - **after a rest:** + Dynamics × 0.2 after half a beat or more of silence;
  - **register:** + 0.5 per semitone above D5 (less below), since a flute's upper register speaks louder.
  - **Live notes** skip the long-note term.
- **Vibrato.** Notes of at least the style's vibrato length get vibrato after Bloom. **Live notes** get it whenever they are held past Bloom. The kernel does not need the length: the blend starts at the delay whether the note is long or not, and a short note ends before it shows.
- **Swell.** Notes of at least the style's swell length lean in up to Swell dB and ease off before their end. **Live notes** don't swell, because their end isn't known.
- **Ornaments.** A clip note of a beat or more, on the one or a beat, after a leap of at least the style's leap, gets a **grace** with the seeded chance Some or Lots gives. The grace is the project key's scale step above, played on the beat for the style's grace time (Lyrical 70 ms, Airy 35 ms, a folk cut), and the note is then slurred from it. Without a project key the step above is a whole tone. Breath has no lookahead and needs none: the grace takes its time from the note itself.
- **Tonguing.** Fast repeated notes (a sixteenth apart, same key, not slurred) alternate t and k. The k plays −2 dB with its first 8 ms faded in, as a softer back-of-the-tongue onset.
- **Humanize.** It adds ±Humanize velocity and up to ±3 cents per note (scaled by Humanize, ±3 at 10), seeded by the note's place in a two-bar cycle and its key (SplitMix64), so loops repeat exactly and export equals playback.
- **Amount.** It blends between the note's own velocity and Breath's. Vibrato, swell and ornaments scale with it.

| Style | the one | beat | eighth | sixteenth | centre | vibrato from | delay | depth | swell from | grace leap |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| As drawn | — | — | — | — | — | never | — | — | never | — |
| Lyrical | 1.0 | 0.9 | 0.82 | 0.76 | 84 | ¾ beat | 160 ms | 100 % | 1½ beats | 3 semitones |
| Airy | 1.0 | 0.85 | 0.75 | 0.7 | 78 | 1½ beats | 350 ms | 55 % | 2 beats | any |

As built: Dynamics, Swell and Bloom are Lyrical's at their defaults (16, 3 dB, 400 ms). Airy plays them scaled as the prototype's Airy did (Dynamics ×14/16, Swell ×½, Bloom ×1.5), and its long-note and rest shares are its own (0.2 and 0.25, Lyrical 0.35 and 0.2). Some and Lots are the prototype's grace chances (0.6 and 0.75): Lyrical Air pairs Lyrical with Some and Folk Breeze Airy with Lots, as the prototype did. The prototype always took a fast repeat's second note as k; Breath alternates for real, a k only after a t. Without a project key graces step a whole tone; the project key is major or natural minor (the prototype's D dorian is not a project key).

**Articulation** sets how notes join:
- **Auto:** slur on overlap, tongue otherwise, staccato for notes under a third of a beat.
- **Legato:** also slurs notes separated by up to a sixteenth.
- **Tongued:** every note tongued.
- **Staccato:** every note short and light.

### Parameters

| Id | Name | Range | Timing |
| --- | --- | --- | --- |
| `wren.breath.style` | Style | As drawn, Lyrical, Airy | next note |
| `wren.breath.amount` | Amount | 0–100 % | live |
| `wren.breath.dynamics` | Dynamics | 0–30 | live |
| `wren.breath.vibrato` | Vibrato | 0–100 % | live |
| `wren.breath.bloom` | Bloom | 80–500 ms | next note |
| `wren.breath.swell` | Swell | 0–6 dB | next note |
| `wren.breath.ornaments` | Ornaments | Off, Some, Lots | next note |
| `wren.breath.humanize` | Humanize | 0–10 | live |
| `wren.articulation` | Articulation | Auto, Legato, Tongued, Staccato | next note |
| `wren.slur` | Slur time | 10–150 ms | live |
| `wren.voices` | Voices | Mono, Poly 4 | when idle |
| `wren.tone.air` | Air | −6 to +6 dB | live |
| `wren.tone.warmth` | Warmth | −4 to +4 dB | live |
| `wren.tone.space` | Space | 0–100 % | live |
| `wren.tune` | Tune | ±50 cents | live |
| `wren.level` | Level | −60 to +6 dB | live |

**Presets:**
- **Lyrical Air** (the default): Lyrical, Ornaments Some, Space 30 %.
- **Folk Breeze:** Airy, Ornaments Lots.
- **Plain Flute:** As drawn, Space 0.
- **Breathy Pad Lead:** Lyrical, Air +4, Space 60 %.
- **Staccato Birds:** Staccato, Humanize 4.
- **Two Flutes:** Poly 4, Lyrical.

### The panel

The panel:
- **Breath:** the three style tiles; a two-bar lane showing the notes about to play, coloured by tongue and staccato, with slur arcs, vibrato waves from where they bloom, and grace notes; six sliders; Ornaments.
- **Articulation card,** with Slur time.
- **Tone card:** Air, Warmth, Space, Tune and Level.
- **Library plate,** with the credit.
- **Header:** Mono or Poly 4, and the preset.

## Build order

Each step is committed with its tests:
1. The install script, with a synthetic test library (tones with a fixed attack end and loop).
2. Breath, as a pure function, with an oracle test against the prototype's decisions on the audition line.
3. The engine: voices, slurs, the vibrato blend, loops, tone and Space.
4. The panel.
5. The catalog entry.

## Build notes

- **Level.** The Iowa takes sit low (a typical ff note peaks near −18 dBFS, the loudest vibrato takes near 0 dBFS), so Wren takes no output trim before Level.
- **Staccato** (the articulation) holds every note at most a third of a beat and plays it 3 dB lighter.
- **Graces** need the step above inside 59–97; above D♭7 the note plays without one.
- **Envelopes** (fades, swell, the vibrato blend) are evaluated every 16 frames of a voice's life and interpolated, on the voice's own clock, so any block split renders the same.

## Not verified yet

The following were chosen by ear on headphones only:
- the Breath constants;
- the slur crossfade and bend (a full bend leaves both voices at one pitch for the last two thirds of the crossfade, where their phases can partly cancel);
- the Staccato articulation's 3 dB lightness and the output level;
- the loop crossfades (up to 120 ms);
- Space's fixed hall.

Loops still have to be checked by ear on held notes of 8 s or more in every register.

CPU must be measured as whole-process CPU at real-time pace, light (the audition line) and heavy (Poly 4 with fast slurred runs), with mean and p99, before the engine ships.
