# ADR-0068 — Knock: physically modelled percussion (eleventh native engine)

**Status:** Accepted; engine, shared modal library, kits and panel built. **Date:** 29 October 2026.
**Builds on:** ADR-0027 (native synth platform), ADR-0036 (Concert's physical model), ADR-0037 (telemetry), ADR-0048 (parallel playback), ADR-0062 (denormals).
**Panel design:** [ui/knock-mockup.html](../ui/knock-mockup.html). Its script is the sonic reference: materials, mode clouds, kits and formulas.

## Context

Requirement: the thuds, knocks, taps, clicks and plops of modern minimal techno and house top loops and underbelly. Three constraints shape the engine:
- **Not tonal by default.** Dense, scattered mode clouds and a noisy grit exciter, with a Tone control that runs from a "thing" (a knock on an object) to a "note".
- **Not old-fashioned or analogue.** A Finish stage (Snap, Length gate, Lift high-pass, Crush), a Bend that goes both ways (rising is a water-drop "plop"), and kits by genre.
- **Not too long.** A global Decay macro, and kit defaults that are short and tight.

## Decision

1. **Eight pads on keys C1..G1** (`knock`, `knock::first_key` 36). Each is a one-shot with two voices: a new hit takes the quieter one and the outgoing voice fades over 64 frames. Note-offs and pedals are ignored: `models_pedals` is true and the kernel does nothing with them, as Anvil's does, so the runtime's generic sustain never holds a hit.
2. **A hit is physics.**
   - *Striker:* Mallet, Stick, Finger or Brush. Each is a mass in nonlinear contact (the Stulov law, `modal::ContactLaw`) with the resonator's displacement at the strike point, sub-stepped eight times a sample.
   - *Hardness:* sets the stiffness so that, at velocity 100, contact lasts 6.5 ms (soft) to 0.23 ms (hard). A finger stays 1.7 times longer and a brush 1.2 times. With exponents above 1, a faster strike is shorter and brighter by itself; velocity also shifts hardness by +0.35 (v − 0.8), scaled by Velocity (`knock_striker.hpp`).
   - *Surface:* the resonator meets the striker as a mass 40 times its own, however many modes ring.
   - *Grit:* a noise force into the modes. A brush gives 35 ms, sine-windowed; the others give max(2 ms, 4 × contact), decaying. Its amount is (brush ? 1.2 : 0.1) + 0.9 (1 − Tone).
     - Grit is the surface's roughness felt through the contact, so it is low-passed at one over the contact time: a soft mallet's grit is a dull rub and a hard stick's a bright scratch. A brush's bristles scrape on their own and stay full band.
     - A pad's grit, click and air noise are the same on every hit, as a sample's would be. A few milliseconds of fresh noise varies by several dB from draw to draw, which made "machine" hits (Hand 0) uneven. Hand varies the strike instead.
   - *Click:* high-passed noise straight to the output.
3. **The resonator is a mode cloud.**
   - *Modes:* the material's modes (Wood, Skin, Metal, Glass, Tube, Card) plus round((1 − Tone) × 34) extras, scattered by up to (1 − Tone) × 0.45 octave.
   - *Stable clouds:* the random draws come from a seed of pad and material only. Tone takes a prefix of the same draws, so automating Tone or Pitch moves one cloud rather than drawing a new one.
   - *Decay, damping and strike position:* as in the mockup. Modes above 0.45 × the rate are dropped.
   - *Coupled resonator:* a second cloud, driven by the first's output at unity gain at resonance × Coupling.
   - *Body:* six low modes of a box at {1, 1.48, 2.1, 2.9, 3.7, 4.6} × (60 + (1 − Size) 260) Hz, driven by both, plus decorrelated low-passed air in each ear.
4. **Bend.** A frequency factor max(0.05, 1 + Bend × 1.2 e^(−t/45 ms)) is applied to the resonator, recomputed every 16 frames from the hit, so block splits never change it. A bent mode keeps the level and force coupling it was designed with.
5. **Finish, per voice, in order:**
   - Lift: two one-pole high-passes, 12 dB/oct.
   - Snap: (1 + 2.5 s e^(−t/5 ms))(1 − 0.45 s (1 − e^(−t/20 ms))).
   - Length: a gate with a 3 ms fade, after which the voice ends. Length at its maximum (2 s) means no gate.
   - The pad's level.
   - Crush: a sample-and-hold of 1 + round(c² × 14) frames from the hit and quantisation to 2^(16 − 11c) steps at full scale.
   - Level, pan, room send and Lift follow the panel on ringing voices; everything else is read at each hit.
6. **Global Decay** (10–150%) scales every T60, the air and the gates. It only ever shortens a gate.
7. **Levels are calibrated, not estimated.** The mockup normalised each rendered hit to its peak. Knock instead renders each pad's hit once with the kernel's own voice, at velocity 100 and level 0 dB, without the hand, whenever the pad's sound (or Decay or Velocity) changes. Every hit of that sound is then brought to a short-term (2 ms RMS) level of −14 dB before its level, or to a −6 dBFS sample peak if that comes first.
   - **Why short-term RMS, not the peak:** a click's or grit's peak is one random sample, so pads matched by peak differed by several dB in loudness. The peak ceiling holds a one-sample click, which is heard quieter than its RMS says, out of the safety clipper.
   - **Result:** every pad of every kit sits within 3 dB of the target over several hits with its Hand, except clicks held by the ceiling, and none peaks above −1 dBFS.
   - **Determinism:** the calibration is a function of the values alone, so export equals playback and block splits do not matter.
   - **What it measures:** the source alone. Lift and Length act after it as a filter and a gate, and the global Decay is left out, so turning them changes the level as a filter or gate would and never recalibrates. The kits' pad levels carry what Lift and Length take away, so each kit sounds as designed.
   - **Cost:** a short render of one voice with coarse contact sub-steps, stopping 3 ms after the strike's excitation ends (a Bend does not hold it open). The kernel calibrates a pad ahead of its hits once its sound has held for eight control ticks, one pad per tick, so a kit chosen while playing is ready within a few milliseconds. A pad whose knob is turning is calibrated once per hit, in the note-on: recalibrating on every tick of a turn cost more than a core. An analytic estimate was tried first: against a simulation of the kits it spread from −15 to 0 dBFS.
8. **Room:** a small shared stereo eight-line Householder network. Size sets its T60 (0.2–2.8 s) and damping; pads send post-finish by Room.
9. **Hand.** Per pad and per hit since the last reset (as Pulse seeds its noise), the hand jitters hardness ±0.15h, position ±0.08h, tune ±14h cents and pan ±0.08h.
10. **Telemetry:** the latest pad (−1 before any hit), hits since reset (wrapping at 2^24), each pad's output peak, the output peak in dBFS (peak followers with a 300 ms release) and where each pad's latest hit landed (−1..1, pan, hand and Motion included; 0 before any).
11. **`design_pad()`** runs the same design code as the kernel and returns each mode's frequency, level relative to the strongest and T60, for the panel's ringing-modes view.
12. **A shared modal library** (`modal_dsp.hpp`):
    - `ContactLaw`: Concert's felt law. `piano::Hammer` now computes through it, bit for bit.
    - `TwoPoleMode`: Concert's soundboard body resonator. The soundboard uses it.
    - `Striker` and the rigid-contact formulas.
    - `ModalBank`: a structure-of-arrays bank in rotation form with single-precision state. It covers the struck, sub-stepped and driven steps, retuning, energy and retiring.
13. **Kits:** Minimal (the defaults), Tech House, Dub Techno and Organic, ported exactly from the mockup (linear levels as dB). Eight more follow: Microhouse Clicks, Afro Percussion, Kitchen Foley, Glitch Crush, Deep Underbelly, Rubber Bounce, Metal Workshop and Glass Garden. Presets keep only the values that differ from the defaults; kits are written as whole pads and diffed at compile time.

## Width and Swing (29 October 2026)

The mockup's Width section and its groove generator asked for two more things a sample kit cannot do: a stereo image that comes from the object, and a shuffle that the kit plays itself, whatever writes its notes.

14. **Spread places each mode, not the hit.** Every resonator, coupled and body mode is placed at a seeded position in −1..1 times Spread, at equal power (√2 cos θ, √2 sin θ, unity at the centre).
    - *Why per mode:* a struck plate or a box radiates each mode from its own place, so a spread hit sounds like a wide object, not a mono hit panned. Widening a whole hit (a stereo delay or decorrelation) smears the transient instead.
    - *Stable image:* the seed is the pad and the mode's index only (resonator, then coupled, then body), as the mockup's `spreadRng`, so moving Pitch, Tone or Decay keeps each mode where it was.
    - *Mono-safe lows:* modes below 150 Hz stay centred, so a spread thud folds to mono without losing its low end.
    - *What stays mono:* the banks still ring each other mono, as one object does, and the click stays where the stick lands, centred before the pad's pan.
    - *Live:* Spread follows the panel on ringing voices; a change re-places a voice's modes at the next control tick. `ModalBank` keeps a per-mode share of each ear (`place`, `stereo_output`), so its mono output and Concert's use of it are unchanged.
15. **Motion moves each hit.** Fixed; Ping-pong (±Depth by the pad's hit count since the last reset, first hit left); Scatter (a seeded draw in ±Depth from the pad and its hit count); Orbit (Depth × sin(2π × beat / period), the period 1, 2, 4 or 8 bars of the project's meter, `knock.orbit`). The offset adds to the pad's pan and hand.
    - Orbit reads the song position at the hit: the transport at the control tick plus the frames since, at the transport's tempo (120 BPM when none is known). That position is rounded to 1/65536 of a beat, under a frame at any tempo, so the transport's rounding under different block splits never moves a hit.
16. **Haas** (0–20 ms) delays the ear away from where the hit lands (the right when it lands in the centre) through a per-voice line sized when the kernel is made. It is read at each hit.
17. **Width never recalibrates.** Spread, Motion, Depth, Haas and Swing are not calibrated controls: the probe hears the pad mono and without Haas, and equal-power placing keeps each mode's power. A pad's stereo energy at Spread 1, with or without Haas, is within 0.5 dB of its mono energy.
18. **Swing plays off-beat sixteenths late.** A note within 1/48 beat of an off-beat sixteenth (beat fraction 0.25 or 0.75), on a pad whose Swing follow is On, plays `swing_delay_beats(swing)` beats later: 0.5 is the triplet (1/12 beat), 0.75 the dotted sixteenth (1/8 beat). It applies to any MIDI the pad gets, clips or a live keyboard.
    - *Scheduling:* the kernel counts the frames it has rendered. A swung note becomes a pending hit due at that clock plus the delay in frames at the tempo of its tick, in a fixed queue of 64 (a further hit plays on time rather than allocating). `render()` splits its span at each due frame and plays the hit there, so it lands on its exact frame however the host splits its blocks, survives block boundaries and control ticks, and is dropped by a reset (seek or loop).
    - *Why deterministic:* the due frame is a function of the note's frame and the tempo only, never of where a block ends, so any block split renders the same samples, and export equals playback.
    - *Why in the kernel:* the swing is the kit's own groove, so the pattern generator writes straight notes and the user's clips and live playing swing alike. The tail bound allows a swung last hit at 30 BPM.
19. **Kit defaults.** Each pad's Width comes from its sound (the mockup's `defaultWidth`, `knock_table::default_width`): a brush scatters (Spread 0.6, Depth 0.55, Haas 3 ms), a pad below 220 Hz stays narrow and still (0.15), one above 1.8 kHz ping-pongs (0.5, Depth 0.45), and the rest spread their modes (0.4). The same thresholds give `pad_role()` (low, mid, high) for the pattern generator. Dub Techno's ringing metal lid and Glass Garden's high glass orbit instead. Swing is 0 except Tech House (25%), Afro Percussion (30%) and Rubber Bounce (20%); every pad follows it.

## Key (5 November 2026)

Toms, thuds and pitched knocks were hard to tune by ear against a bass line, because Pitch is a frequency in hertz. A pad can now follow the project's key.

20. **A following pad plays the nearest note of the key.** Each pad has Key follow (`knock.padN.key.follow`, Off or On). With it On and a project key set, the pad's Pitch is replaced by the nearest note of the key's scale (major, or natural minor), equal tempered at A4 = 440 Hz; halfway between two notes goes down (`knock::in_key`). Coupled Tuning is a ratio of Pitch, so the coupled resonator moves with it. Without a key, or with the switch Off, the pad plays its own Pitch.
    - *Defaults from the sound:* low thuds and mid knocks follow; high ticks and shakers (a brush, or a resonator above 1.8 kHz), mostly noise, keep their pitch (`knock_table::default_key_follow`). Saved kits without the control take that default.
    - *In the kernel:* the kernel moves the following pads' Pitch in a copy of its values before anything reads them. Calibration therefore treats a key change exactly as turning Pitch, and Key follow itself is not a calibrated control.
    - *Pitch automation steps:* a following pad's Pitch moves from note to note.
21. **The key is heard.** A key change was presentation only, like a name. It now advances the audible revision (`ChangeSet::key`; `same_audio` compares the key).
    - *Hand-off:* the synth runtime takes the key lock-free (`NativeSynthRuntime::offer_project_key`) and gives it to the kernel at the next block start (`SynthKernel::project_key`).
    - *Seamless:* routed playback has a key-only path that offers the key to every running synth without rebuilding the graph, so nothing stops or restarts.
    - *Export equals playback:* players offer the snapshot's key when prepared.

## Consequences

- **Parameters:** 239 (29 per pad and 7 globals). The first release had 189 (23 per pad and 5 globals); Width, Swing and Key are appended to each pad's block, and Width and Swing after the globals. Ids are persisted forever. Choices (striker, materials, Motion, Swing and Key follow, and the Orbit period) are live but not automatable, as in Crate.
- **Not in v1:**
  - *Per-pad multi-output:* the platform kernel renders one stereo output. Parameters are per pad, so separate outputs can be added later without a format change.
  - *The Recording striker* (strike with your own sound from Crate) is deferred.
- **Cost:** a full kit rings up to 16 voices of up to 102 modes. Contacts cost eight sub-steps a sample while they last. See [performance/knock-synth.md](../performance/knock-synth.md).
- **Concert is unchanged:** its goldens and fingerprints hold, because the shared law and resonator perform the same operations in the same order.
