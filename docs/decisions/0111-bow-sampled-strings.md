# ADR-0111 — Bow: sampled strings, sections and solo, bowed from the mod wheel

**Status:** Accepted
**Date:** 9 November 2026.
**Builds on:** ADR-0089 (Wren: a resident library on `sample_stream`, loops baked by the installer), ADR-0088 (Fret), ADR-0071 (the shared sample core), ADR-0035 (the mod wheel as a performance lane), ADR-0103 (the shared panel parts).

## Why

Requirement: sampled strings for pads and parts that sit under other instruments. Each section and a solo player must sound, with the articulations a string part needs, played in realtime from a keyboard: dynamics on the mod wheel, legato between held notes and keyswitches for the articulation.

## Decisions

### The sources

- **VSCO 2 Community Edition** (Versilian Studios, CC0 1.0) for the sections: the violin, viola and cello sections (sustain with vibrato, spiccato, pizzicato, tremolo, two dynamics, up to two round robins) and the solo contrabass, which Bow layers into the basses section. The solo violin's spiccato, pizzicato and tremolo play in Solo.
  - **Origin.** The SFZ maps say which recordings each articulation plays. The installer accepts a mapped sample only if it lies under one of the five string folders and is byte-identical (the same git blob) to the recording in the 2016 "Seeded Samples" commit. Every string sample passes. The 1.1.0 additions (VSCO 1 legacy, Upright No. 1, Miscellania) are not strings and are not read.
- **University of Iowa MIS** solo strings (pre-2012 session; "may be downloaded and used for any projects, without restrictions"): violin, viola, cello and double bass, arco, pp, mf and ff, each string's chromatic run in one mono AIFF.
  - The Iowa pizzicato runs are not used. Their notes ring into each other, so the cuts do not place them, and their soft takes sit near the noise floor. Solo pizzicati are VSCO's: the solo violin's, and the section's for the others.
- **Not used:** Virtual Playing Orchestra and Sonatina, which include Sampling Plus and CC BY-SA material.

### Installed format

`tools/bow/install_strings.py` (Python standard library, git and ffmpeg) installs the library.

- **Fetch.** It does a partial git clone of VSCO at three pinned commits, checking their tree ids: the samples, the SFZ maps and the seed. It downloads the 143 Iowa arco runs, checking each against a pinned SHA-256.
- **Notes.**
  - A VSCO note's key comes from its map's `pitch_keycenter`. Its dynamic layer comes from the dynamic its file is named with (v1–v3, p or f); a second take of one dynamic, and its order in the map, give its round robin. A recording mapped to two velocity splits counts once.
  - Iowa runs are cut at their silences, as Wren's are. Keys are assigned by position in the run, and a note out of place takes the key it measures as, within the run.
  - Every note's pitch is measured by YIN within two semitones of its key. A note more than half a semitone off is dropped; `cents` records the offset.
  - Of an Iowa note played on two strings, the take nearest its open string is kept.
  - Takes outside the keys Bow plays on their instrument are left out.
- **Sustains and tremolos.**
  - The attack end is where the 10 ms RMS first reaches 90 % of the steady level.
  - A loop of 0.6–2 s is found within 3 s after the attack end, with its ends matched in level and refined by waveform correlation.
  - A 150 ms equal-power crossfade, corrected for the passages' correlation, is baked in before `loop_end`, and the sample ends at `loop_end`, so samples stay short.
- **Shorts** (spiccato, pizzicato) end 60 dB under their loudest 10 ms or 6 dB above their noise floor, whichever comes first, at most 4 s, with a 20 ms fade.
- **Level.** Each set (mode, instrument, articulation) is scaled so its loudest layer's median level is −20 dBFS RMS (sustains and tremolos, over the steady part) or −16 dBFS (shorts, over the first 200 ms). Layers keep their recorded difference, and `level_db` is each note's level after the scale.
- **Packs.** Both sources are 44.1 kHz, so nothing is resampled. Samples are written as 16-bit dithered int16, aligned to 4 KiB, into `bow-stereo.pack` (VSCO) and `bow-mono.pack` (Iowa).

The manifest's first line is `oscillate bow library 1`. Then come `name`, `attribution`, `rate 44100`, `channels 2`, `pack bow-stereo.pack <bytes>` and `mono_pack bow-mono.pack <bytes>`, and one line per sample:

```
sample <channels> <section|solo> <violin|viola|cello|bass> <sustain|staccato|pizzicato|tremolo>
       <key> <layer> <round_robin> <offset> <frames> <attack_end> <loop_start> <loop_end>
       <cents> <scale> <level_db>
```

`loop_end` 0 marks a one-shot. Every sample stays resident (no streaming): 1,137 samples, 311 MiB. The library sits at `$OSCILLATE_BOW_LIBRARY`, else `$XDG_DATA_HOME/oscillate/instruments/bow/strings`.

### The engine

- **Id and keys.** The id is `bow`, version 1. It plays E1–G7. Each instrument plays its own range: violin G3–G7, viola C3–E6, cello C2–E5, bass E1–G4. A note outside the section's range is ignored.
- **Sections.**
  - Violins I, Violas, Cellos and Basses play their instrument.
  - Violins II read the violin takes a whole tone up (repitched), so doubled with Violins I they never play the same recording at the same pitch.
  - Ensemble splits the keyboard by register: basses below E2, cellos to E3, violas to E4, violins above.
  - Each section sits in its own seat across the stage.
- **Solo or Section.** Solo plays the Iowa takes, Section the VSCO ones. A set one mode lacks plays the other's: a solo viola's staccato and tremolo play the section's.
- **Articulations.**
  - **Sustain** and **Tremolo** loop while held.
  - **Legato** is a single line. A note over a held one starts past its attack, at the take's attack end. It fades in equal-power over **Legato time** while the note before bends toward the new pitch over a third of it (within an octave) and fades out.
  - **Staccato** and **Pizzicato** are one-shots that play out, whatever the note-off.
- **Keyswitches.** C0–E0 (MIDI 12–16, below every range) latch Sustain, Legato, Staccato, Pizzicato and Tremolo. They sound nothing and are not counted as notes. A latch holds until the panel's articulation changes or a reset (a seek), which returns to the panel's.
- **Dynamics: the mod wheel** (`bow.perf.wheel`, "Dynamics", a performance lane per ADR-0035).
  - Sustained articulations read the wheel and one-shots their velocity, mapped across the recorded layers.
  - The two layers the dynamics fall between are crossfaded equal-power (different recordings are uncorrelated). Each layer is brought to the level the dynamics play at, interpolated in dB between the layers' own `level_db`.
  - Below 15 % of the wheel the bow lifts toward silence, by up to −24 dB, so a swell can start from nothing.
  - A high shelf follows the wheel, down to −5 dB at 7 kHz at the bottom.
  - The wheel is read as a function of the frame within each control interval, so any block split plays the same.
- **Velocity.** On a sustain, a soft note eases in over up to 140 ms (velocity 1). At full velocity the recording's attack plays as it is.
- **Ensemble** (players, 1–16). Every four players add a layer per note, up to four.
  - Each layer after the first plays a neighbouring take (the key's pick shifted by +2, −2 or +3 semitones and repitched), detuned by up to ±7 cents, up to 22 ms late and spread ±0.22 around the section's seat.
  - Layers are scaled by 1/√layers, so the level holds as the section grows.
- **Humanise** (0–100 %) spreads every player's tuning (±8 cents), timing (up to 20 ms) and level (±1.5 dB).
  - It is seeded by the note's place in an eight-beat cycle, its key and its layer (SplitMix64), so loops repeat and export equals playback.
- **Vibrato** (0–100 %) adds up to 18 cents at about 5.4 Hz (each player ±8 % in rate, from its own phase) to the recorded vibrato. It blooms over 0.6 s after 0.25 s.
- **Tone** (sul tasto, ordinario, sul ponticello) is a filter model on the output:
  - toward sul tasto, a −10 dB shelf at 7 kHz, −3 dB at 4.5 kHz and +1.5 dB below 220 Hz: darker and rounder;
  - toward sul ponticello, +4 dB at 7 kHz, +6 dB at 4.5 kHz and −6 dB below 220 Hz: the fundamental thins and the upper partials turn glassy.
  - No sources record the techniques, so there is no sample layer for them.
- **Release** (0.05–4 s) falls 60 dB over its time while the loop holds.
- **Resampling** uses the shared sinc tables. Each reader picks its table from its fastest speed until the next envelope grid point, so the choice never depends on the block split.
- **Space** is a fixed hall on the Expanse core (size 0.55, 2.8 s decay, 18 ms pre-delay), its mix the control. **Tune** and **Level** follow, then the safety knee.
- **Voices.** At most 64 players sound: 16 notes of four layers. A new one takes a free voice, else the oldest released, else the oldest.
- **Pedals.** The runtime's generic sustain, sostenuto and soft pedals act around the kernel.
- **No library.**
  - The first look reads the library on a background thread and reports `loading`; a Bow created meanwhile is silent and adopts the library when idle.
  - Offline renders wait for that read before their first block (`await_content`), so export never misses it.
  - Missing or invalid, Bow is silent and says why, with the install command and where it looked.

### Parameters

| Id | Name | Range | Timing |
| --- | --- | --- | --- |
| `bow.section` | Section | Violins I, Violins II, Violas, Cellos, Basses, Ensemble | next note |
| `bow.mode` | Players | Solo, Section | next note |
| `bow.articulation` | Articulation | Sustain, Legato, Staccato, Pizzicato, Tremolo | next note |
| `bow.perf.wheel` | Dynamics | 0–100 % (the mod wheel) | live |
| `bow.vibrato` | Vibrato | 0–100 % | live |
| `bow.tone` | Tone | 0–100 % (50 % ordinario) | live |
| `bow.players` | Ensemble | 1–16 players | next note |
| `bow.humanise` | Humanise | 0–100 % | next note |
| `bow.legato` | Legato time | 20–400 ms | live |
| `bow.release` | Release | 0.05–4 s | live |
| `bow.space` | Space | 0–100 % | live |
| `bow.tune` | Tune | ±50 cents | live |
| `bow.level` | Level | −60 to +6 dB | live |

**Presets**, restrained to sit under pads:
- **Sul Tasto Pad** (the default): Violas, Section, Legato, mp, Vibrato 38 %, Tone toward sul tasto, eight players.
- **Warm Section:** Ensemble, Sustain, twelve players.
- **Pizzicato Ostinato:** Violins II, Pizzicato.
- **Tremolo Swell:** Violins I, Tremolo, from p.
- **Solo Cello:** Cellos, Solo, one player.
- **Ensemble Legato:** Ensemble, Legato, sixteen players.

### The panel

The panel follows the approved mockup:
- **Header:** the shared device header with the library chip (ready with its size, loading, missing or damaged) and the presets.
- **Section picker** with the Solo/Section switch.
- **Articulation list** with each keyswitch note; the one playing is filled, and a latched keyswitch is marked.
- **Bow display:** the wheel's last seconds, the newest note's vibrato depth and a playhead dot, in the shared plot frame.
- **Tone slider:** sul tasto, ordinario, sul ponticello.
- **Six knobs:** Dynamics, Vibrato, Ensemble, Humanise, Legato time and Release.
- **Keyboard strip:** keyswitches, the section's playable range, held notes and the voice count.

### Missing libraries, in every sampled instrument

Concert, Fret, Wren, Crate and Bow show one shared device-kit part, `LibraryNotice`:
- the state in words;
- why the library is missing or damaged;
- what plays once it is installed, with the install command and a copy action;
- **Look again**, and **Show in folder**, which opens the library's folder or its nearest existing parent instead of printing a raw path.

Crate has no library: its notice is the empty state, asking for a sample.

## Measured

- **Oracles** (`native_bow_contract`, on a synthetic library of tones with an attack chiff and a level and phase per layer):
  - a missing library is silent, says why and plays once installed;
  - malformed libraries are rejected;
  - keys are in tune in both modes;
  - the wheel crossfades the layers by level;
  - legato joins past the attack, with each sample's step bounded by √2 of the notes' own;
  - keyswitches latch and unlatch;
  - voices stay bounded;
  - Tone darkens and thins as described, and vibrato blooms;
  - Humanise is seeded;
  - block splits and resets render the same, and export equals live playback;
  - the callback never allocates;
  - the tail bound covers a release and the hall.
- **CPU** for a four-part pad is reported from the reference-project tooling in CI, as whole-process CPU, mean and p99.
