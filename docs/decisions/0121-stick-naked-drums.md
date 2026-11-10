# ADR-0121 — Stick: Naked Drums, the default kit

**Status:** Accepted; implemented.
**Date:** 10 November 2026.
**Amends:** ADR-0114 (its kits, pieces, articulations, mic groups, key map, defaults and presets).

## Why

A listening shootout of eight sampled kits against Stick's three DrumGizmo kits put Naked Drums first: a well-recorded Yamaha studio kit with five velocity layers and ten round robins on the kick and both snares, five toms and 16 mic channels, close and distant. It becomes Stick's default kit; the shootout's weakest kits leave.

## Decisions

### The kits

| Kit | Source | Channels | Recorded | Hits kept |
| --- | --- | ---: | --- | ---: |
| **Naked Drums** (default) | Wilkinson Audio, SFZ conversion by kinwie | 16 | sticks; two snares, five toms, splash, china | 585 |
| **DRS Studio** | DRSKit 2.1, Jes Eiler (DRSDrums) and the DrumGizmo team | 13 | sticks and brushes | 499 |

- **Muldjord and Garage are dropped.** Naked covers Muldjord's role (a rock kit with rooms, four or more toms, a china) with a better recording; Garage measured weakest. DRS Studio stays for its brushes, the only ones in any kit.
- **Not used from Naked:** the crash bells and chokes, the second splash and the second china (no General MIDI key).

### Naked's provenance and licence

- **Source:** https://github.com/sfzinstruments/WilkinsonAudio.NakedDrums at commit `407732a548606ac715ad8f3fbac63d9f4df77c0d` (tree `1cabc9919668f4e957cb45f02e83ed052071286b`): Wilkinson Audio's recordings, once a free Kontakt instrument, converted to SFZ with FLAC samples by kinwie.
- **Licence:** the repository's LICENSE, Creative Commons Attribution 4.0 International. The grant rests on that licence and on the repository's commit `51a7032d4913529dac06bb485fc0768cc3d11da3` (1 July 2020), which removed a NonCommercial term "as we have confirmation from Wilkinson that this is OK"; the commit after it restored the licence's original text.
- **Checked at the pinned commit:** the LICENSE and the README's licence section are the only terms; no folder holds another notice; the SFZ files carry a "© Wilkinson Audio 2016" line and no other terms; every one of the 3,840 FLAC files holds only its stream information and an empty Vorbis comment (no tags).
- **Credit:** the manifest's `attribution` (shown in the library popup), ATTRIBUTION.txt beside the kit and the README carry the notice: creator, source, licence and link, and that the samples may be modified for Stick. ATTRIBUTION.txt adds the recording and conversion credits, the commits and every change the conversion makes.
- **Identifiable samples:** `sources.tsv` beside the kit names, for every hit in the pack, each source file (its path in the repository) and file channel on each of its channels.
- **Nothing is redistributed by the project.** The installer fetches the source and converts it on the machine it is installed on; Stick adds no terms and no protection to the converted samples.

### Installing Naked

`tools/stick/install_kits.py --kit naked` (Python standard library, git and ffmpeg):

- **Fetch.** git fetches the pinned commit (git checks every object's hash); the commit, its tree, a clean checkout, the 3,840 samples and the licence's first line are checked.
- **Read.** The SFZ text is read as data: `#define` applied in order, the longest name first; `#include` followed only inside the checkout; bounded sizes. Each of its mic files (`Data/stereo/<layer>_<part>.txt`) gives a mic's regions; a hit is one key, articulation, velocity layer and round robin, with a file per mic. The hits per stroke are checked against the pinned SFZ's.
- **Map.**

| SFZ | Stick |
| --- | --- |
| Kick (C2) | kick |
| Snare 1 (D2), its side-stick (C♯2) | snare, side-stick |
| Snare 2 (E2) | snare `alt`, the second snare |
| Toms 1–5 | tom1–tom5 |
| Hats: pedal; tight and closed; half; open | hi-hat pedal; closed (one pool); half; open |
| Ride bow and bell | ride, bell |
| Crash 1, crash 2 (edges), splash 1, china 1 | crash1, crash2, splash, china |

- **Applied as the SFZ plays them:** each mic's trim (`master_volume`), the hats' articulation trims (`group_volume`) and the per-layer volumes; toms 3 and 4 tuned down 200 and 70 cents (played slower before resampling).
- **Velocity.** Naked's samples are normalised per velocity layer (a soft kick's files peak as high as a hard one's); its SFZ gives them their dynamics by velocity tracking (`amp_veltrack` 99 %: (v/127)² in amplitude). The installer applies that gain at each layer's top velocity, so each hit's measured power, and Stick's choice of hit by power, follows the layers: soft notes play the soft recordings, quieter, as the SFZ plays them.
- **Channels.** Naked's 16 mic channels become 16 pack channels:

| Channel | Group | Hears |
| ---: | --- | --- |
| 0 | `kick_in` | kick |
| 1 | `kick_sub` | kick |
| 2 | `snare_top` | snare: the two top mics summed |
| 3 | `snare_bottom` | snare |
| 4 | `toms` | tom1–tom5, each tom on its own mic |
| 5 | `hihat` | hi-hat |
| 6 | `ride` | ride |
| 7, 8 | `overheads` | left, right |
| 9 | `room` | the close room, one mic in the centre |
| 10, 11 | `far_room` | left, right |
| 12, 13 | `mid_side` | left, right (decoded) |
| 14, 15 | `china_oh` | left, right |

  - The two snare-top mics always play together on one fader, so they are summed.
  - Each tom was recorded on its own mic only (the others' files hold no bleed), so one channel carries all five without loss and the mixer keeps one Toms fader.
  - A mic that did not record a hit (Naked's hat mic is silent on the pedal) stays silent and undithered.
- **Hits** are measured, trimmed and normalised as ADR-0114's, at most 64 per stroke (Naked's 60 closed hats all stay). Five kicks with two strokes are dropped.

### Sixteen-bit packs

The pack stays little-endian int16. Measured on one hit per stroke and velocity layer: the 16-bit dither and rounding sit 96.2 dB under each hit's (normalised) peak; the installer cuts each hit where every mic has fallen 60 dB under that peak, so everything kept is at least 36 dB over the 16-bit floor, and gain applied later raises the recording and the dither alike. Where the recordings' own noise shows (the noisiest mic of many hits, 33–90 dB under the peak), it lies above the 16-bit floor. A 24-bit pack would take half as much memory again (73 to 110 MiB resident, 2.4 to 3.5 GiB on disk) for nothing audible.

### Memory

- **Naked Drums:** 585 hits on 16 channels: **73 MiB resident** (4,096 frames of each hit), a 2.4 GiB pack.
- **DRS Studio:** 51 MiB resident, a 1.8 GiB pack.
- The streamer's 16-channel limit holds (Naked has 16); each kernel's rings stay 16 MiB.

### Mics in the manifest

`mic <channel> <group> <piece[+piece...]|-> <mono|left|right|centre>`:

- A close mic names every piece recorded on it (`tom1+tom2+tom3+tom4+tom5`). A hit of one of them plays it as its own: no Bleed switch, its own Pan. Any other piece's hit is bleed, panned with the mic's first piece.
- `centre` is a single distant mic, played equally left and right.

### The engine

- **Pieces:** kick, snare, hi-hat, tom1–tom5, crash1, crash2, ride, splash, china (13). `ride2` is gone with Muldjord.
- **Articulations:** hit, alt, side-stick, bell, closed, half, open, pedal; `rimshot` is gone (no kit recorded one). `alt` is a second drum or stroke: Naked's second snare, DRS Studio's kick with its beater bouncing.
- **Keys:** E2 (40) plays the second snare; G3 (55) the splash; B3 (59), the second ride in General MIDI, the ride. The six tom keys split over five toms (D3 and C3 both tom 1).
- **Voices per piece:** kick 2, snare 3, hi-hat 4, each tom 2, crashes and ride 3, splash and china 2: 32.
- **Mic groups:** `kick_in`, `kick_out`, `kick_sub`, `snare_top`, `snare_bottom`, `toms`, `hihat`, `ride`, `overheads`, `room`, `far_room`, `mid_side`, `china_oh` (13). A piece's Room send feeds the rooms (`room`, `far_room`, `mid_side`); its Overheads send the overheads and the china pair. Dry turns the rooms off and the overheads and china pair down 6 dB; Room turns the close mics down 6 dB and the rooms up 6 dB.
- **Defaults** ("Natural"): Naked Drums as its SFZ mixes it: every mic at 0 dB but the hi-hat, ride and china mics at −14 dB; the close mics panned as the SFZ pans them (hi-hat −0.3, ride +0.3, toms −0.6 to +0.6); nothing shortened, not humanised. 138 parameters.
- **Presets:** Natural, Close Mics (Naked dry, the drums shortened), Rooms, DRS Studio (DrumGizmo's mix), Brushes (DRS Studio's whiskers).

### The panel

The kit drawing places 13 pieces (five toms, the splash beside the hi-hat, the china over the floor toms); the mixer's strips narrow to fit Naked's twelve groups.

## Measured

- **Oracles** (`native_stick_contract`, on a synthetic kit with a shared tom channel and a centred room): `shared_close_mics_and_a_centred_room` (a shared close channel plays each of its pieces as its own, panned by that piece; the centred mic sounds equally left and right; other pieces do not bleed into it with Bleed off), the malformed mic lines (a centred or empty close mic, a repeated piece), the new key map and presets. The installer's tests (`tools/stick/test_install_kits.py`) read and install a synthetic kit laid out as Naked's SFZ.
