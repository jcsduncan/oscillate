# ADR-0121 — Stick: Naked Drums and a production chain per mic group

**Status:** Accepted; implemented.
**Date:** 10 November 2026.
**Supersedes in part:** ADR-0114: its kits and default kit, pieces, articulations, mic groups, key map, defaults and presets. ADR-0114's other decisions stand: the streamed packs, background loading, the hit choice by power, round robins, the hi-hat pedal, the mic mixer, the per-piece controls and the panel.

## Why

A listening shootout of eight sampled kits against Stick's three DrumGizmo kits put Naked Drums first: a well-recorded Yamaha studio kit with five velocity layers and ten round robins on the kick and both snares, five toms and 16 mic channels, close and distant. It becomes Stick's default kit; the shootout's weakest kits leave.

A recorded kit is the start of a drum sound, not the end: an engineer gates, EQs, compresses, shapes and drives each mic and the kit's buses. Stick does the same inside the instrument, so its presets can shape one well-recorded kit into distinct, record-ready sounds.

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
- **Velocity.** Naked's samples are normalised per velocity layer (a soft kick's files peak as high as a hard one's); its SFZ gives them their dynamics by velocity tracking (`amp_veltrack` 99 %: (v/127)^1.98 in amplitude). The installer applies that gain at each layer's top velocity, so the hits' measured powers order by layer. Stick then plays by power as for every kit (a velocity asks for a power over the stroke's range), which approximates the SFZ: soft notes play the soft layers, quieter, though not at the SFZ's exact velocity boundaries or curve.
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
- **Keys:** E2 (40) plays the second snare; G3 (55) the splash; B3 (59), the second ride in General MIDI, the ride. The six tom keys split over five toms (D3 and C3 both tom 1). A second drum the kit did not record plays its plain stroke: B1 plays Naked's kick, E2 DRS Studio's snare.
- **Voices per piece:** kick 2, snare 3, hi-hat 4, each tom 2, crashes and ride 3, splash and china 2: 32.
- **Mic groups:** `kick_in`, `kick_out`, `kick_sub`, `snare_top`, `snare_bottom`, `toms`, `hihat`, `ride`, `overheads`, `room`, `far_room`, `mid_side`, `china_oh` (13). A piece's Room send feeds the rooms (`room`, `far_room`, `mid_side`); its Overheads send the overheads and the china pair. Dry turns the rooms off and the overheads and china pair down 6 dB; Room turns the close mics down 6 dB and the rooms up 6 dB.
- **Defaults** ("Natural"): Naked Drums as its SFZ mixes it: every mic at 0 dB but the hi-hat, ride and china mics at −14 dB; the close mics panned as the SFZ pans them (hi-hat −0.3, ride +0.3, toms −0.6 to +0.6); nothing shortened, not humanised, the chain off.
- **Presets:** Natural, House Tight, Funk Pocket, Break Crunch, Lo-Fi Dust, Big Room (below), DRS Studio (DrumGizmo's mix) and Brushes (DRS Studio's whiskers).

### The production chain

Each mic group has an insert chain, in signal order, and a send; then two buses. Every stage is built from the native effects' own kernels (ADR-0040), not reimplemented, each at settings with no latency:

| Stage | Kernel | Stick's controls | Fixed |
| --- | --- | --- | --- |
| Gate | Hush | threshold, release | gate mode, keyed from its input, range −80 dB, no lookahead |
| EQ | Shape | low cut (off at 20 Hz), low shelf (100 Hz), bell (frequency, gain, Q 1), high shelf (8 kHz), high cut (off at 20 kHz) | cuts 12 dB/octave; a flat band is off |
| Compressor | Press | threshold, ratio, attack, release, makeup | knee 6 dB |
| Transients | Strike | attack, sustain (±18 dB) | one band, minimum phase, no lookahead |
| Saturation | Forge | Tape, Tube, Transistor or Clip; drive; mix | auto gain |
| Parallel send | — | the group's level into the parallel bus (off at −60 dB) | |

- **Parallel bus:** every group's send, through a Press (threshold, ratio, attack, release, makeup; defaults −30 dB, 10:1, 1 ms, 100 ms, +12 dB), returned under the kit at its own level.
- **Kit bus:** glue (Press), EQ (Shape, as a group's), clip (Forge's Clip curve with auto gain), each switched; then Stick's level and safety knee. Edge is not used: its true-peak guard has latency.
- **Off costs nothing.** A group whose stages are all off and whose send is closed mixes on the direct bus, as without the chain; an off stage is not processed. Its settings change nothing (tested sample for sample).
- **Switching.** A stage switches in or out over a 10 ms crossfade between its input and its output; coming in, its kernel starts clean. The first tick takes each stage as set, as an export starts, so a reset renders as the first pass did.
- **Buses.** Each voice mixes its channels into the direct bus and one bus per group whose chain sounds this tick, all at the voice's level and pan, resampled alike; the chain runs per control tick (32 frames), so block splits render the same.
- **Realtime.** Every kernel is created with the Stick kernel, on control; nothing allocates on the callback (`the_callback_does_not_allocate` runs with every stage on, and the drums reference counts no callback allocation).
- **Tail:** the kit's longest hit plus each chain's stages' tails (the longest group chain, the parallel bus, the kit bus).
- **Parameters:** 24 per group (gate 3, EQ 7, compressor 6, transients 3, saturation 4, send 1), 6 for the parallel bus, 15 for the kit bus: 471 in all.

### Shaped presets

Measured on one kick and one snare at velocity 118 (`oscillate_nova_audition --drums`, `hits/`), with the shootout's metrics: fundamental, the share of energy in each band over the first 0.5 s, the time to fall 20 and 40 dB from the peak, and the crest over the first 100 ms.

| Preset | Shaping | Kick: sub / body / box / click dB, t40 | Snare: f0, body / box / click dB, t40, crest |
| --- | --- | --- | --- |
| **Natural** (default) | none | −2.0 / −5.8 / −25.6 / −31.6, 179 ms | 182 Hz, −4.5 / −6.4 / −10.9, 273 ms, 14.2 dB |
| **House Tight** | kick short, its sub up, box cut, compressed; snare attack up, sustain down; rooms closed; glue, low cut | −1.7 / −12.9 / −38.0 / −34.2, 79 ms | 186 Hz, −4.3 / −7.5 / −7.5, 105 ms, 15.5 dB |
| **Funk Pocket** | snare compressed, attack up, soft velocity curve (ghosts lift); kick box cut; a little room; gentle glue | −1.3 / −10.1 / −31.8 / −34.2, 106 ms | 267 Hz, −5.2 / −7.5 / −12.2, 141 ms, 16.1 dB |
| **Break Crunch** | kick, snare, overheads and room smashed in parallel (12:1, +16 dB); snare, overheads and room driven; clipped | −2.5 / −5.0 / −25.3 / −31.5, 95 ms | 182 Hz, −3.4 / −7.4 / −10.8, 174 ms, 10.3 dB |
| **Lo-Fi Dust** | band-limited (90 Hz–5.5 kHz), tape on the kick and room, compressed and clipped, near mono (wide pairs out, close mics centred), loose | −4.0 / −2.4 / −21.6 / −28.9, 105 ms | 269 Hz, −4.9 / −5.3 / −11.7, 181 ms, 10.5 dB |
| **Big Room** | rooms up 2–4 dB, the far room compressed 6:1, rooms gated short; snare's room send up; glue | −2.8 / −4.8 / −21.7 / −25.8, 168 ms | 182 Hz, −4.1 / −6.4 / −11.2, 280 ms, 15.8 dB |
| DRS Studio, Brushes | DRS Studio's mix; its brushes | | |

- **Against the shootout:** Natural matches Naked as the shootout heard it (kick 70 Hz, sub −2.6 / body −4.6 dB, t40 92 ms; snare body −10.5 / box −6.2 / click −11.3 dB). The shaped kits move away from it where they mean to: House Tight's kick is the cleanest and shortest of all ten kits measured (box −38 dB, t40 79 ms); Funk Pocket's snare the snappiest (crest 16.1 dB); Break Crunch's and Lo-Fi Dust's the densest (crest 10.3 and 10.5 dB, below every shootout kit's).
- **Against the reference kicks** (ADR-0060's median of 26 melodic house and techno intros): the reference kicks fall 4, 7, 10, 15.5 and 21 dB by 50, 100, 150, 200 and 300 ms; every acoustic kick here falls 15–22 dB by 50 ms. An acoustic kick is not those records' kick, so the comparison guides House Tight's length and sub only.
- **Level:** each preset's groove lies within 2.5 dB of Natural's loudness (−25.4 LUFS integrated) and peaks at or under −1 dBFS.

### CPU and memory

Measured on the drums reference (two Sticks playing a busy groove on Naked Drums, the master chain, 20 s through the paced fake callback, seven helpers; i7-10700K, GCC 15.2, release build):

| Sticks | 64 frames: process CPU, p99 (100 ms) | 128 frames | Callback p99 of period (64) |
| --- | --- | --- | --- |
| Natural ×2 (chain off) | 25.7 %, 29.5 % | 22.1 %, 26.2 % | 27.1 % |
| Natural and House Tight | 30.7 %, 37.5 % | 30.0 %, 35.2 % | 36.0 % |
| Break Crunch ×2 (sends, three saturators, clip) | 59.1 %, 66.0 % | 56.2 %, 65.3 % | 48.0 % |

The same run without the chain's code (Natural ×2 before it) took 25.2 % at 64 frames: switched off, the chain costs no more than the run-to-run spread (about 1 %).

No block ran over its period and the callback never allocated. Peak resident memory is 150 MiB for the whole process with Naked loaded (73 MiB of it the kit's starts, shared by every Stick).

### The panel

- The kit drawing places 13 pieces (five toms, the splash beside the hi-hat, the china over the floor toms); the mixer's strips narrow to fit Naked's twelve groups.
- **Chain lamps.** Each strip shows a lamp per stage (lit when on) and one for its parallel send. Clicking them opens the group's chain over the mixer and the piece: each stage in a tile, its switch in the title, its knobs dimmed while off; the saturation's curve as a segment. **Kit buses** in the kit row opens the parallel bus and the kit bus the same way.

## Measured

- **The chain** (`native_stick_contract`): `the_chain_off_changes_nothing`, `inserts_shape_their_own_group` (a gate shuts its group and not the room; a low cut thins its group and not the overheads; saturation adds the kick's third harmonic; a compressor lifts a soft hit; attack lifts the start), `the_parallel_and_kit_buses` (the return adds, closed it adds nothing; the clip lowers the crest), `a_stage_switches_in_without_a_step`; block splits, resets, allocation and the tail run with every stage on. `quick_stick_panel_contract` opens a chain from a strip and the buses from the kit row.
- **Oracles** (`native_stick_contract`, on a synthetic kit with a shared tom channel and a centred room): `shared_close_mics_and_a_centred_room` (a shared close channel plays each of its pieces as its own, panned by that piece; the centred mic sounds equally left and right; other pieces do not bleed into it with Bleed off), the malformed mic lines (a centred or empty close mic, a repeated piece), the new key map and presets. The installer's tests (`tools/stick/test_install_kits.py`) read and install a synthetic kit laid out as Naked's SFZ.
