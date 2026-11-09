# ADR-0114 — Stick: sampled multi-mic acoustic drums, streamed from disk

**Status:** Accepted
**Date:** 9 November 2026.
**Builds on:** ADR-0071 (Concert: resident starts, streamed rest), ADR-0111 (Bow: background reads, seeded Humanise, the library notice), ADR-0035 (performance lanes), ADR-0103 (the shared panel parts).

## Why

Requirement (#341): a real acoustic drum kit for house, funk and lo-fi parts, recorded on many mics, mixed like a studio kit: velocity that follows the recordings, round robins that never machine-gun, a hi-hat opened by its pedal, and each mic's level with or without the bleed between them.

## Decisions

### The sources

Three DrumGizmo kits, each under the Creative Commons Attribution 4.0 licence (no NonCommercial or NoDerivatives term):

| Kit | Source | Mics | Recorded | Hits kept |
| --- | --- | ---: | --- | ---: |
| **DRS Studio** (default) | DRSKit 2.1, Jes Eiler (DRSDrums) and the DrumGizmo team | 13 | sticks and brushes (whiskers) | 499 |
| **Muldjord** | MuldjordKit 3, Lars Muldjord | 15 | sticks; two kicks, four toms, two rides, a china | see below |
| **Garage** | ShittyKit 1.2, Sardonicus | 8 | sticks | 136 |

- **Credit.** Each kit's credit and the licence's link are in its manifest's `attribution`, the library popup and `ATTRIBUTION.txt` beside the kit. Muldjord's carries the credit its page asks for: "Drum samples provided by DrumGizmo.org".
- **Not used:** The Aasimonster (its room and kick are out of time with each other) and CrocellKit (a metal kit, 5.5 GB).
- **Nothing is redistributed.** The installer converts each kit on the machine it is installed on.

### Installed format

`tools/stick/install_kits.py [--kit drs|muldjord|garage]` (Python standard library and ffmpeg) installs the kits, all three by default.

- **Fetch.** It downloads each kit's zip from drumgizmo.org, resuming a partial one, and checks it against the MD5 its page publishes. The zips stay in `downloads/` unless `--remove-downloads`.
- **Read.** The drumkit and instrument XML are parsed as data, bounded in size; every file they name must lie inside the zip. Nothing in a zip is run.
- **Map.** Pinned tables map each kit's channels to mic groups (below) and its instruments to Stick's pieces, articulations and strikers. Instruments a kit records twice for one stroke (DRS's tip and shank, Garage's left and right hands) join one pool, ordered by power.
- **Channels.**
  - Muldjord's trigger channel is not a microphone and is dropped; its snare bottom is inverted, as its page advises.
  - Garage's overhead and room pairs are heard from the audience and are swapped, so every kit is heard from the drummer's seat (measured: its floor tom and ride are louder in its left overhead).
- **Hits.** Each hit is decoded at 48 kHz (SoX resampler when ffmpeg has it) and measured on its own close mics, or the overheads for a cymbal:
  - a hit whose own mics never reach -70 dBFS has no stroke in it, and is dropped (Muldjord's empty open hat);
  - a hit with two strokes is dropped: its attack envelope (the steepest slope per 5 ms, held over 15 ms, so a kick's slow beating is not mistaken for strokes) rises 12 dB over the dip after the first stroke, within 24 dB of its peak (Muldjord's double ride hits);
  - its **power** is the RMS of its own mics over its first 50 ms;
  - it is trimmed to 1 ms before its stroke (with a fade-in), and where every channel falls 60 dB under its peak or to twice the recording's noise floor (at most 20 s, with a 20 ms fade-out);
  - it is normalised to its peak and dithered to 16-bit.
- **At most 48 hits per stroke**, spread evenly over their powers.
- **Pack.** `stick.pack` holds every mic interleaved (little-endian int16), each hit starting on a whole frame and a 4 KiB boundary.

The manifest's first line is `oscillate stick library 1`. Then come `name`, `attribution`, `rate 48000`, `channels <mics>`, `pack stick.pack <bytes>`, then:

```
mic <channel> <kick_in|kick_out|snare_top|snare_bottom|toms|hihat|ride|overheads|room>
    <piece|-> <mono|left|right>
hit <piece> <articulation> <stick|brush> <offset> <frames> <scale> <power_db>
```

Close mics hear one piece, in mono; the overheads and room are left and right of a pair.

### Memory: the starts in RAM, the rest streamed

The kits are 2–5 GB of multi-channel audio, too much to hold. Stick streams them as Concert does:

- **Resident.** The first 4,096 frames (85 ms) of every hit stay in memory. Measured: **DRS Studio 51 MiB** (499 hits on 13 mics), **Garage 8 MiB**; Muldjord below. A kit loads only when a Stick chooses it, and stays loaded until the process exits.
- **Streamed.** The rest is read into each voice's ring by the shared streamer (`sample_stream`), which now carries packs of up to 16 channels. Each Stick kernel holds 32 rings of 16,384 frames of 16 channels: 16 MiB.
- **On disk.** DRS Studio's pack is 1.8 GiB, Garage's 333 MiB.
- **Late data** plays as silence and is counted; the panel shows it. Offline renders service the streams before each block, so export never waits.

### Loading the chosen kit

- `SynthEngine` gains `fetch_content`. The runtime calls it on control when it is created and whenever it is offered values. Stick's starts reading the chosen kit on a background thread, unless it is loaded or unchanged since a failed look.
- The kernel adopts its kit once it is read (a reloaded copy of the same kit once idle). Choosing another kit fades what the last one plays over 30 ms.
- `await_content` waits for the read before an offline render's first block.
- Missing or invalid, Stick is silent and says why, with the kit's install command.

### The engine

- **Id and keys.** The id is `stick`, version 1. Notes follow the General MIDI drum map, written here as the piano roll names them (C4 is 60):

| Key | Plays | Key | Plays |
| --- | --- | --- | --- |
| B1 (35) | kick, the second drum or stroke | A♯2 (46) | hi-hat, open |
| C2 (36) | kick | C♯3 (49) | crash 1 |
| C♯2 (37) | snare, side-stick | D♯3 (51) | ride |
| D2 (38) | snare | E3 (52) | china |
| E2 (40) | snare, rimshot | F3 (53) | ride bell |
| F♯2 (42) | hi-hat at the pedal's openness | G3 (55) | the second ride's bell |
| G♯2 (44) | hi-hat pedal | A3 (57) | crash 2 |
| F2–D3 (41, 43, 45, 47, 48, 50) | the toms, the lowest key the lowest tom | B3 (59) | ride 2 |

  - The six tom keys split evenly over the kit's toms: DRS Studio's three take two each.
  - G3 is the splash in General MIDI; Stick plays Muldjord's second ride bell there, as that kit's own map does.
  - A key whose stroke the kit did not record plays nothing: DRS Studio has no rimshot, Muldjord no pedal.
- **What each kit plays:**
  - **DRS Studio:** the kick with its beater buried (C2) and bouncing (B1); snare and its rim click as side-stick; hi-hat closed (tip and shank), half open, open (and its tip) and pedal; three toms; two crashes; the ride and its bell. Brushes for the snare, toms, hi-hat, ride and crashes.
  - **Muldjord:** two kicks; snare and its rest stroke as side-stick; hi-hat closed and open; four toms; two crashes, two rides with bells, a china.
  - **Garage:** kick; snare (both hands and hard hits) and side-stick; two toms; hi-hat closed, open and pedal; ride and bell; two crashes.
- **Velocity → hit.**
  - The piece's curve shapes velocity v as v^(2^(1.5 c)), c from -1 (soft) to 1 (hard).
  - It asks for a power along the stroke's recorded range, at least 30 dB below its loudest.
  - The hit recorded nearest that power plays, brought to it: louder by at most 3 dB, quieter without bound.
  - With **Round robins** on, any hit within 2 dB of the target may play (at most six), never the stroke's last.
  - Choices are seeded by the note's place in an eight-beat cycle and its key (SplitMix64), so loops repeat and export equals playback.
- **The hi-hat.**
  - **Hi-hat pedal** (`stick.perf.pedal`, a performance lane like the mod wheel, ADR-0035) is its openness.
  - F♯2 plays at the pedal's openness, crossfading equal-power between the two recordings around it (closed, half, open).
  - A hat played less open than its recording rings shorter: it reaches -60 dB at that share of its length, no sooner than 0.1 s.
  - A stroke chokes ringing hats 0.2 more open than it, over 30 ms. The pedal note chokes any open hat, and so does the pedal closing below 15 %.
- **Each piece's hits.** At most two kicks, three snares, four hat hits, two of each tom and three of each cymbal ring together. A new hit fades the oldest over 60 ms: drums are re-struck, cymbals ring on. 32 voices in all.
- **Mics.** Every hit plays every mic of the kit, as recorded, mixed down to one stereo pair:
  - **Pan** places a piece's own close mics; bleed in a close mic follows that mic's piece. A piece without its own mic (a crash, the china) pans by balancing its overheads.
  - **Bleed** off keeps each hit to its own close mics, the overheads and the room.
  - Each group's **Level**, **Mute** and **Solo**; any solo silences the unsoloed groups.
  - **Mix:** Dry (overheads -6 dB, no room), Studio (the faders as set) and Room (close mics -6 dB, room +6 dB), over the faders. Mutes, solos, mixes and Bleed move over 10 ms.
  - Each channel's weights change at a control tick from the first source frame the voice has not read, so any block split mixes alike.
- **Per piece:**
  - **Tune** (±12 semitones, from the next hit) resamples with the shared sinc tables; at 48 kHz and no Tune a hit plays its frames exactly.
  - **Decay** below 100 % adds a decay reaching -60 dB at that share of the hit's length.
  - **Level**, **Pan**, **Velocity curve**, and the **Overheads** and **Room** sends: the piece's level in each pair.
- **Kit-wide:**
  - **Strikers:** sticks, or brushes where the kit recorded them (the kick's beater and the pedal play either way).
  - **Round robins.**
  - **Humanise** (0–100 %): each hit up to 8 ms late and ±1.5 dB, seeded as the round robins are.
  - **Level**, then the safety knee.
- **Note-offs** do nothing: drums play out. The runtime's generic pedals stay (the soft pedal softens hits).

### Parameters

| Id | Name | Range | Timing |
| --- | --- | --- | --- |
| `stick.kit` | Kit | DRS Studio, Muldjord, Garage | next note |
| `stick.mix` | Mix | Dry, Studio, Room | live |
| `stick.bleed` | Bleed | Off, On | live |
| `stick.perf.pedal` | Hi-hat pedal | 0–100 % (0 closed) | live, automatable |
| `stick.humanise` | Humanise | 0–100 % | next note |
| `stick.round_robin` | Round robins | Off, On | next note |
| `stick.striker` | Strikers | Sticks, Brushes | next note |
| `stick.level` | Level | −60 to +12 dB | live |
| `stick.mic.<group>.level` | Level | −60 to +12 dB | live |
| `stick.mic.<group>.mute`, `.solo` | Mute, Solo | Off, On | live |
| `stick.<piece>.tune` | Tune | ±12 st | next note |
| `stick.<piece>.decay` | Decay | 5–100 % | live |
| `stick.<piece>.level` | Level | −60 to +12 dB | live |
| `stick.<piece>.pan` | Pan | −1 to 1 | live |
| `stick.<piece>.curve` | Velocity curve | −1 (soft) to 1 (hard) | next note |
| `stick.<piece>.overheads`, `.room` | Overheads, Room | −60 (off) to +6 dB | live |

Groups: `kick_in`, `kick_out`, `snare_top`, `snare_bottom`, `toms`, `hihat`, `ride`, `overheads`, `room`. Pieces: `kick`, `snare`, `hihat`, `tom1`–`tom4`, `crash1`, `crash2`, `ride`, `ride2`, `china`. 119 parameters.

**Presets:**
- **Tight Funk** (the default): DRS Studio, Studio mix, the kick (45 %) and snare (62 %) shortened, the toms to 70 %, Humanise 18 %.
- **Dry Break:** close mics only, no bleed, short kick, snare and toms.
- **Room Kit:** Muldjord in its room, every hit ringing out.
- **Garage Lo-Fi:** Garage, room mix, the snare a semitone down.
- **Brushy Soft:** DRS Studio on brushes, light touches lifted.
- **As Recorded:** DRS Studio with nothing shortened.

### The panel

The panel follows the approved mockup:
- **Header:** the shared device header, with the library chip (kit, mics and resident size; loading, missing or damaged) and the presets.
- **Kit picker** (each kit's description, and whether it is installed) and the **Dry / Studio / Room** mixes.
- **The kit from above:** each piece the kit has, with its key, lit as it sounds. Click a piece (or Tab and Space) to show its controls. Under it, the **hi-hat pedal**.
- **Mics:** a strip per mic group the kit has: meter, fader, level, mute and solo; **Bleed** in the title. Without the kit, the shared `LibraryNotice` stands in its place, with the kit's install command.
- **This piece:** its hits, articulations and keys, and seven knobs (Tune, Decay, Level, Pan, Vel curve, Overheads, Room).
- **Kit row:** Strikers (inactive on a kit without brushes), Round robins, Humanise, Level and the voices sounding.

Changed from the mockup:
- **Snares** became **Overheads**, the piece's level in the overhead pair. A snare-wire knob would only repeat the snare-bottom fader for the snare, and would mean nothing for the other pieces.
- **Room size** was dropped: the room is recorded, so its size cannot change. The room mics' level and the Room mix stand in for it.
- **Key names** follow the piano roll (kick C2 for MIDI 36), where the mockup used C1.

## Measured

- **Oracles** (`native_stick_contract`, on a synthetic kit whose mics each play their own frequency and whose strokes have hits at five powers):
  - a missing kit is silent, says why and plays once installed;
  - a kit chosen loads in the background before an offline render's first block;
  - malformed kits are rejected;
  - the General MIDI map;
  - velocity follows the recorded power, monotonically, with round robins on or off, and the curves bend it;
  - round robins never repeat the hit before, and repeat as seeded;
  - the pedal opens the hat; a closed stroke, the pedal note and the pedal closing each choke it;
  - Bleed, mutes, solos and the Dry mix route each hit's mics, and pans place close mics and balance cymbals;
  - Tune, Decay and brushes;
  - every output frame of a hit is its recording, through the resident start and on into the stream;
  - voices are bounded, each piece's too;
  - telemetry lights the piece and its mics;
  - Humanise is seeded;
  - block splits and resets render the same: export equals playback;
  - the callback never allocates;
  - the tail bound covers a cymbal tuned down.
- **Panel** (`quick_stick_panel_contract`): the library states and install command, the layout and telemetry decode, the notice and Look again, a strip per group, piece selection and mutes.
- **CPU** for a busy groove on two Sticks is reported from the reference-project tooling (the drums reference) as whole-process CPU, mean and p99.
