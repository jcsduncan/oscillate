# ADR-0060 — Anvil: a drawn kick synth (ninth native engine)

**Status:** Accepted; engine, presets, preview and panel built. **Date:** 25 October 2026.
**Builds on:** ADR-0027 (native synth platform), ADR-0035 (pedals), ADR-0037 (telemetry), ADR-0057 (synth documents: curves and media), ADR-0058 (drawn curves in Pump).

## Context

Requirement: a first-class kick synth for techno (rumble) and house. A dedicated engine was chosen over deepening Pulse's kick pads, because the kick needs drawn pitch, amplitude and drive curves over its own waveform, four layers (synthesised clicks plus user samples) and a tuned rumble, which do not fit a drum-kit pad.

## Decision

1. **One kick, drawn.** Anvil (`anvil`) is monophonic. A note-on starts a hit; note-offs and pedals change nothing (`models_pedals` is true and the kernel ignores them, so the runtime's generic sustain never holds a kick).
2. **The document is the body** (ADR-0057). Three curves over the hit's length (`anvil.length`, 50 ms–2 s):
   - `pitch`: y 0..1 is a logarithmic frequency from 20 Hz to 2 kHz (`anvil::hz_of_y`, `y_of_hz`), so segments blend in log frequency and a straight line is straight on a log axis;
   - `amp` (gain) and `drive` (0..1).
   - Every curve uses the platform's one bend (tension −1..1) and step segments. A missing curve plays the default shape ("Warehouse Rumble"). The only media slot is `click`, a managed sample for click type Sample, summed to mono and windowed-sinc resampled to the engine rate on control (at most 2 s); an unchanged sample keeps its resampling across document edits.
3. **Layers.**
   - *Body:* a phase-accumulating oscillator synthesised at 4x and decimated by the platform's half-band chain. Waves Sine, Triangle, Soft Square and Harmonic are band-limited additively (Chebyshev recurrence, partials below the base Nyquist); Harmonics adds a 2nd and 3rd partial (or sets the Harmonic wave's slope). Start phase, a 0.25 ms rise and a 2 ms fall into the length's end. The drive curve times Body Drive (scaled by Velocity Drive) saturates each 4x sample, peak-preserving.
   - *Tuning:* Tune (±24 st) transposes the whole curve. Key *Follow note* transposes it so the drawn tail lands on the played key's frequency; *Fixed* plays it as drawn.
   - *Click:* Tick (sine burst), Noise (high-passed), Beater (band-passed noise and a felt thump), FM Snap (index kept under Nyquist) or Sample (start offset, pitch, played for Length then faded). Length is the time to −60 dB. Click Velocity lets harder hits click more.
   - *Sub:* a sine at the tail note (±1 octave) following the amplitude curve through its own attack.
   - *Rumble:* a comb tuned to the sounding hit's tail note, its loop low-passed (and its delay compensated for that low-pass's phase, and its feedback for its gain, so the comb rings at the tail with the set decay), into a four-line Householder network for space and width, then driven, low-passed by Tone and ducked by a peak follower of the body's envelope. It keeps ringing across retriggers and retunes over 20 ms when the tail moves (a Follow-note bass line).
4. **Master:** sum → drive (Tape, Tube, Clip, Fold; 4x) → DC blocker → tone (12 dB low-pass, 0..1 maps 200 Hz–20 kHz logarithmically) → punch (a hit-locked transient gain with a 0.3 ms rise) → glue (Pulse's glue compressor) → output (dB) → a soft ceiling (knee 0.85, ceiling 0.97) at 2x, a base-rate soft guard for the half-band's overshoot (0.97 to 0.99), then a hard guard at 0.995. Decaying states are flushed so silence never runs on denormals.
5. **Never a click.** A retrigger fades the outgoing hit over 3 ms (raised cosine) under the new one; on/off switches, wave changes and drive-type changes glide over 20 ms raised cosines; a seek fades the last output over 64 frames. Every parameter is live and automatable.
6. **Preview.** `anvil::render_preview` renders one hit through the same kernel class, at 32-frame control ticks as the runtime does, over max(length, one beat): the mono master, the rumble alone after its duck, the kick's length, the tail as played and the peak. The panel draws it under the curves after each edit.
7. **Telemetry:** position in the hit (−1 idle), last note, last velocity, and the output peak (held, 300 ms release), dBFS.
8. **Presets:** 25. Techno: Warehouse Rumble (the defaults), Berlin Dry, Industrial Hammer, Hypnotic, Peak Time, Dub Techno, Rolling Rumble, Hard Groove, Bunker, Warehouse Sub. House: Tech House Thump, Deep House Round, 909 Tribute, Minimal Tick, Disco Kick, Garage Punch, Organic Thud, Lo-Fi House. Hard: Hardstyle Tok, Hard Techno Screech. 808: 808 Long, Trap Boom. Breaks: DnB Punch, Breaks Knock. And Init. Each carries its body as a preset document.

## Consequences

- No project-format change: an engine id, a parameter table and a document of existing kinds.
- The tail bound is the hit, the click, 2.4 rumble decays (the comb and the network share it) and the glue's release.
- Tests (`native_anvil_contract`): the registry and presets; documents; the body tracking its drawn pitch within 15 cents; the tail in tune within 2 cents, tuned and following the key; the amplitude curve as the envelope; a retrigger without a step; the rumble ringing at the tail and ducking under the hit; every drive bounded with aliases 30 dB down; the ceiling; block splits and repeat runs identical; no allocation; the tail bound; the preview equal to the kernel's render; telemetry; every preset sane. The engine-wide switch and pedal contracts include Anvil.
- Cost (i7-10700K, 48 kHz): 1.8–2.8 % of a core at 128 frames; the one-beat preview 8–10 ms, so a panel redrawing on every drag renders it at 24 kHz ([docs/performance/anvil-synth.md](../performance/anvil-synth.md)).

## Revision: kicks, not toms (25 October 2026)

The first presets sounded like toms. Measured, three faults made them so:

- **Drive erased the decay.** Both drives normalised `tanh(g x) / tanh(g)`, which lifts everything quiet: a body 6 dB down came out about 1 dB down, so every hit held a level and was then cut. The body drive now shapes the wave before the amp curve (its curve adds harmonics over the hit; the amp curve alone sets the level), and the master drive is up to 18 dB into a unity-small-signal shaper (Tape, Tube, Clip, Fold) with half the gain taken back, so it rounds the loud part of the hit and leaves the decay as drawn.
- **Bodies followed no kick.** Pitch glided for 30–60 ms to a 57–74 Hz tail, and the amp curves held then fell. Every preset body is now sampled from an analog kick: pitch `tail + (start - tail) e^(-t/tau)` with tau 4–12 ms, and a level that holds a few milliseconds and decays exponentially (tau 55–220 ms for kicks, longer for 808s). The points stay ordinary curve points.
- **The rumble drowned the kick.** At its old scale the rumble sat about 4 dB under the hit for the whole beat, a drone at the tail note. Its output is halved, so a bed sits mid-knob, and each rumble preset is set so the end of the beat lies 12–18 dB under the hit.

The reference is the kick-only intros of 26 melodic techno and house tracks from a private reference set (low-passed at 700 Hz, kicks found on the tagged tempo grid). Of the clean ones, the median kick reaches about 180 Hz by 8 ms, 90 Hz by 15 ms and 55 Hz by 30 ms, settles at about 50 Hz, and decays about 4, 7, 10, 15.5 and 21 dB by 50, 100, 150, 200 and 300 ms. The house presets now fall in that range (909 Tribute: 162, 87 and 59 Hz; 5.3, 8.3, 11.7, 15.7 and 23.3 dB). Each preset's output is set from four-bar loops to sit at or below -10 LUFS integrated and -1 dBTP.

## Revision: fitted to reference kicks (25 October 2026)

The analog-law presets were still too tonal. Measured against the clean kick-only intros (per-track medians), they put 15 dB too much energy at 150-400 Hz in the attack (the audible "pew"), carried a 3rd harmonic 20 dB too strong in the body, and had no knock at 400 Hz-1.5 kHz.

- **Body drive scaling.** At 5 % the body drive already mixed in 40 % of a hard-shaped wave (3rd harmonic -24 dB). It now grows across the knob: 5 % gives about -40 dB, 10 % a real kick's -34 dB, full drive -10 dB.
- **Knock from the attack, clean body.** A fit of the engine's parameters to the reference features (attack band balance, harmonics at multiples of the fundamental, decay) found the knock in the attack driven hard while its pitch is still high, not in the click. Presets now use drive curves that are hot for the first 10-20 ms and a trace after (`drive_attack`, `drive_attack_long`), sine bodies, a smaller sweep (150-280 Hz with tau 3-6 ms), little master drive, a light click for air and little punch.
- **Result (house).** Attack 150-400 Hz -20 to -25 dB (reference -29), 400 Hz-1.5 kHz -33 to -44 (reference -40), body 3rd harmonic -27 to -51 (reference -34), level 50 ms after the hit -3 to -11 dB (reference -6). Techno keeps a harder attack (knock about -22 dB) with a body 3rd harmonic near -29 dB.

## Length modes (2026-11-03)

A hit's length can follow the song as well as a time. **Length Mode** chooses:

- **Time**: Length in milliseconds, as before.
- **Sync**: a note value from 1/32 to one bar, including dotted and triplet values, at the tempo under the hit. The length glides like Length when the tempo moves.
- **Note**: each note in a clip sets its own hit's length. The curves are drawn over that length, so it renders exactly as Time would with Length set to the note's length.
  - The platform supplies the length. A note-on in the compiled MIDI plan now carries its frames to its note-off (`BlockMidiEvent::frames`, `NoteEvent::frames`). It is 0 when nobody knows it yet: keys played live, and notes from note effects.
  - Such a hit runs over Length and ends 5 ms after its key is let go.

Every mode stays between 20 ms and 2 s, so the tail bound holds. Sync and Note assume the longest hit. Kick & Bass Lock gives Nova each hit's envelope for its note's length and the tempo at the hit, so the duck matches the hit in every mode.
