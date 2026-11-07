# 08a — Nova panel ↔ engine contract

**Status:** Draft. **Parent spec:** [08-nova-synth.md](08-nova-synth.md). **Decision:** [ADR-0057](../decisions/0057-nova-flagship-synth.md). **Panel:** `apps/daw_quick/qml/nova/`, tested by `nova_panel_tests`.

The Nova panel was written before the engine. This file is what the engine must publish so the panel works unchanged: the flat parameter table, the document conventions, the modulation sources, the FX modules and the telemetry frame. Everything here is binding on both sides; a change is made here first, then in `NovaContract.js` (panel) and the engine.

The panel's stub descriptor in `tests/ui/quick/nova_panel_tests.cpp` builds exactly this table. When the engine is registered, the test switches to the real descriptor and its checks (every control names a real id; every id has a control) hold the two together.

---

## 1. Conventions

- **Ids.** Every flat parameter id starts with `nova.` and uses dotted groups. Oscillators are `a`, `b`, `c`; filters, envelopes, LFOs and macros are numbered from 1; route slots from 0; FX slot uids from 1.
- **Table order.** Groups appear in the order of §2. Within a group, the order is the order of the rows. Ids are stable forever once released.
- **Automatable.** Continuous `live` parameters are automatable and are **modulation destinations**. Discrete parameters (stepped and choice) are neither, as `validate_engine` requires. The panel refuses a modulation drop on a discrete knob.
- **Timing.** All parameters are `live` unless marked *next note*.
- **Glide.** Continuous parameters glide 20 ms from the panel (`glide_seconds = 0.02`). Pitch parameters glide 5 ms.
- **Units** are the `ParameterDescriptor::unit` strings: `Hz`, `dB`, `st`, `ct`, `s`, `ms`, `%`, `°`, `×` or empty. A `%` parameter stores 0..1 (or −1..1) and displays ×100.
- **Defaults** below are the engine's init patch (`preset_document(nullopt)` plus the flat defaults). Presets change them.

## 2. Flat parameters

Notation: `lin a..b` is linear; `log a..b` is logarithmic; `step a..b` is integer; `choice [..]` lists the choices in index order. `skew` is the descriptor's skew. **D** marks a modulation destination (continuous, automatable).

### 2.1 Master

| Id | Range | Unit | Default | Notes |
|---|---|---|---|---|
| `nova.master` | lin −60..+6 | dB | 0 | **D**. Output level after the FX. |

### 2.2 Oscillators (×3: `<o>` = `a`, `b`, `c`)

| Id | Range | Unit | Default (A / B / C) | Notes |
|---|---|---|---|---|
| `nova.osc.<o>.on` | choice [Off, On] | | On / Off / Off | |
| `nova.osc.<o>.type` | choice [Classic, Wavetable, Sample, Multisample, Granular, Spectral] | | Wavetable / Classic / Classic | *next note* is **not** used: the engine crossfades types itself (spec §9.5). |
| `nova.osc.<o>.level` | lin 0..1 | % | 0.75 | **D** |
| `nova.osc.<o>.pan` | lin −1..1 | % | 0 | **D** |
| `nova.osc.<o>.position` | lin 0..1 | % | 0 | **D**. Wavetable frame position; granular and spectral scan position; sample start offset scan for Multisample. |
| `nova.osc.<o>.shape` | choice [Sine, Triangle, Saw, Square, Pulse] | | Saw | Classic only. |
| `nova.osc.<o>.pw` | lin 0.01..0.99 | % | 0.5 | **D**. Classic pulse width. |
| `nova.osc.<o>.sync` | lin 1..8 | × | 1 | **D**. Classic hard-sync ratio. |
| `nova.osc.<o>.drift` | lin 0..1 | % | 0.15 | **D**. Classic analog drift. |
| `nova.osc.<o>.interpolation` | choice [Crossfade, Spectral] | | Crossfade | Wavetable frame interpolation (NOVA-OSC-02). |
| `nova.osc.<o>.warp.<w>.mode` (`<w>` = 1, 2) | choice [Off, Bend +, Bend −, Sync, Window Sync, Mirror, Asym, Quantise, PWM, Flip, Remap, Fold, FM, PM, AM, RM] | | Off | Indices 12–15 are cross-oscillator. Remap draws curve `warp.<o>.<w>` (§3.2). |
| `nova.osc.<o>.warp.<w>.amount` | lin 0..1 | % | 0 | **D** |
| `nova.osc.<o>.warp.<w>.source` | choice [Osc A, Osc B, Osc C, Sub, Noise] | | B (for A), C (for B), A (for C) | Cross-oscillator modes only. |
| `nova.osc.<o>.unison.voices` | step 1..16 | | 1 | Click-free count changes (NOVA-OSC-09). |
| `nova.osc.<o>.unison.detune` | lin 0..1200, skew 3 | ct | 18 | **D**. Outer-voice spread either side. |
| `nova.osc.<o>.unison.curve` | choice [Linear, Exponential, Super, Inverse, Random] | | Super | Inverse: `sign(x)·√abs(x)`, inner voices spread out. Random: each voice a seeded distance on its own side of the centre, fixed per stack size (ADR-0073). |
| `nova.osc.<o>.unison.blend` | lin 0..1 | % | 0.6 | **D**. Centre (0) against outer (1) level. |
| `nova.osc.<o>.unison.width` | lin 0..1 | % | 0.8 | **D** |
| `nova.osc.<o>.unison.stack` | choice [Off, Octave, 2 Octaves, Fifth, Fifth + Octave, Chord] | | Off | |
| `nova.osc.<o>.unison.position_spread` | lin 0..1 | % | 0 | **D**. Per-voice table offset. |
| `nova.osc.<o>.unison.warp_spread` | lin 0..1 | % | 0 | **D** |
| `nova.osc.<o>.octave` | step −4..4 | | 0 | |
| `nova.osc.<o>.semitone` | step −24..24 | st | 0 | |
| `nova.osc.<o>.fine` | lin −100..100 | ct | 0 | **D** |
| `nova.osc.<o>.pitch` | lin −48..48 | st | 0 | **D**. Continuous coarse pitch, so routes can sweep pitch (NOVA-OSC-08). Shown in the matrix, not on the card. |
| `nova.osc.<o>.phase` | lin 0..360 | ° | 0 | **D**. Start phase. |
| `nova.osc.<o>.random_phase` | lin 0..1 | % | 1 / 0.3 / 1 | **D** |
| `nova.osc.<o>.route` | choice [Filter 1, Filter 2, Direct] | | Filter 1 | Used in SPLIT routing (§2.5); SERIAL and PARALLEL feed every non-direct source to both. |
| `nova.osc.<o>.sample.root` | step 0..127 | | 60 | Sample / Multisample root key. |
| `nova.osc.<o>.sample.keytrack` | choice [Off, On] | | On | |
| `nova.osc.<o>.sample.start` | lin 0..1 | % | 0 | **D** |
| `nova.osc.<o>.sample.loop` | choice [Off, Forward, Ping-pong, Reverse] | | Forward | |
| `nova.osc.<o>.sample.loop_start` | lin 0..1 | % | 0 | **D** |
| `nova.osc.<o>.sample.loop_end` | lin 0..1 | % | 1 | **D** |
| `nova.osc.<o>.sample.crossfade` | lin 0..1 | % | 0.05 | **D**. Fraction of the loop length. |
| `nova.osc.<o>.grain.size` | log 10..500 | ms | 80 | **D** |
| `nova.osc.<o>.grain.density` | log 1..200 | Hz | 24 | **D**. Grains per second per voice, at most 64 sounding. |
| `nova.osc.<o>.grain.scan_rate` | lin −2..2 | × | 0 | **D**. Playhead speed; 0 holds `position`. |
| `nova.osc.<o>.grain.spray` | lin 0..1 | % | 0.2 | **D**. Position jitter. |
| `nova.osc.<o>.grain.pitch_spray` | lin 0..24 | st | 0 | **D** |
| `nova.osc.<o>.grain.window` | choice [Hann, Gauss, Tukey, Triangle] | | Hann | |
| `nova.osc.<o>.grain.reverse` | lin 0..1 | % | 0 | **D**. Reverse probability. |
| `nova.osc.<o>.spectral.fft` | choice [1024, 2048, 4096, 8192] | | 4096 | |
| `nova.osc.<o>.spectral.shift` | lin −24..24 | st | 0 | **D** |
| `nova.osc.<o>.spectral.stretch` | lin 0.5..2 | × | 1 | **D** |
| `nova.osc.<o>.spectral.smear` | lin 0..1 | % | 0 | **D** |
| `nova.osc.<o>.spectral.tilt` | lin −12..12 | dB | 0 | **D**. Spectral filter, dB per octave. |
| `nova.osc.<o>.spectral.freeze` | choice [Off, On] | | Off | |

51 parameters per oscillator.

### 2.3 Sub and noise

| Id | Range | Unit | Default | Notes |
|---|---|---|---|---|
| `nova.sub.on` | choice [Off, On] | | Off | |
| `nova.sub.shape` | choice [Sine, Triangle, Saw, Square] | | Sine | |
| `nova.sub.octave` | choice [−2, −1, 0] | | −1 | |
| `nova.sub.level` | lin 0..1 | % | 0.5 | **D** |
| `nova.sub.pan` | lin −1..1 | % | 0 | **D** |
| `nova.sub.route` | choice [Filter 1, Filter 2, Direct] | | Direct | The direct-out flag (NOVA-FLT-03). |
| `nova.noise.on` | choice [Off, On] | | Off | Which noise plays is media slot `noise` (§3.4). |
| `nova.noise.level` | lin 0..1 | % | 0.15 | **D** |
| `nova.noise.pitch` | lin −48..48 | st | 0 | **D** |
| `nova.noise.keytrack` | choice [Off, On] | | Off | Off: pitch is fixed. |
| `nova.noise.tone` | lin −1..1 | % | 0 | **D**. Tilt, dark to bright. |
| `nova.noise.mode` | choice [Loop, One-shot] | | Loop | |
| `nova.noise.pan` | lin −1..1 | % | 0 | **D** |
| `nova.noise.route` | choice [Filter 1, Filter 2, Direct] | | Filter 1 | |

The noise layer's AM (`nova.noise.am.*`, its level following an oscillator) is appended at the table's end: §2.12c.

### 2.4 Filters (×2: `<f>` = 1, 2)

| Id | Range | Unit | Default (1 / 2) | Notes |
|---|---|---|---|---|
| `nova.filter.<f>.on` | choice [Off, On] | | On / Off | |
| `nova.filter.<f>.model` | choice (22, below) | | Ladder 24 / Formant | |
| `nova.filter.<f>.cutoff` | log 8..22000 | Hz | 2000 / 800 | **D** |
| `nova.filter.<f>.resonance` | lin 0..1 | % | 0.2 | **D** |
| `nova.filter.<f>.drive` | lin 0..24 | dB | 0 | **D** |
| `nova.filter.<f>.fat` | lin 0..1 | % | 0 | **D** |
| `nova.filter.<f>.keytrack` | lin −1..2 | % | 0 | **D**. −100 %..+200 %. |
| `nova.filter.<f>.pan` | lin −1..1 | % | 0 | **D** |
| `nova.filter.<f>.mix` | lin 0..1 | % | 1 | **D** |

**Models**, in index order. The panel groups them by these index ranges.

| Group | Indices | Models |
|---|---|---|
| Analog | 0–4 | Ladder 24, Ladder 12, Diode, MG Low, OTA |
| Clean | 5–12 | LP 12, LP 24, BP 12, BP 24, HP 12, HP 24, Notch, Peak |
| Character | 13–18 | Formant, Vowel, Comb +, Comb −, Phaser, Flanger |
| Morph | 19–21 | LP→BP→HP, Dual Peak, Ring |

| Id | Range | Default |
|---|---|---|
| `nova.filter.routing` | choice [Serial, Parallel, Split] | Serial |

### 2.5 Envelopes (×4: `<e>` = 1..4)

ENV 1 is the amp envelope; its `velocity` is the velocity → amp depth (NOVA-VCE-03).

| Id | Range | Unit | Default (ENV 1 / others) |
|---|---|---|---|
| `nova.env.<e>.attack` | lin 0..20, skew 4 | s | 0.002 / 0.01 |
| `nova.env.<e>.hold` | lin 0..10, skew 4 | s | 0 |
| `nova.env.<e>.decay` | lin 0..20, skew 4 | s | 0.3 / 0.5 |
| `nova.env.<e>.sustain` | lin 0..1 | % | 1 / 0.4 |
| `nova.env.<e>.release` | lin 0..20, skew 4 | s | 0.15 / 0.4 |
| `nova.env.<e>.attack_curve` | lin −1..1 | % | 0 |
| `nova.env.<e>.decay_curve` | lin −1..1 | % | 0.4 |
| `nova.env.<e>.release_curve` | lin −1..1 | % | 0.4 |
| `nova.env.<e>.loop` | choice [Off, On] | | Off |
| `nova.env.<e>.legato` | choice [Off, On] | | Off |
| `nova.env.<e>.velocity` | lin 0..1 | % | 0.7 / 0 |

All continuous envelope parameters are **D**. Segment curves use the bend function of §3.2.

### 2.6 LFOs (×8: `<l>` = 1..8)

| Id | Range | Unit | Default | Notes |
|---|---|---|---|---|
| `nova.lfo.<l>.rate` | log 0.01..50 | Hz | 2 | **D**. Used when `sync` is Hz. |
| `nova.lfo.<l>.sync` | choice [Hz, 8 bars, 4 bars, 2 bars, 1 bar, 1/2 D, 1/2, 1/2 T, 1/4 D, 1/4, 1/4 T, 1/8 D, 1/8, 1/8 T, 1/16 D, 1/16, 1/16 T, 1/32 D, 1/32, 1/32 T, 1/64] | | 1/4 | |
| `nova.lfo.<l>.mode` | choice [Trig, Env, Free] | | Trig | Free is song-locked (NOVA-MOD-03). |
| `nova.lfo.<l>.rise` | lin 0..10, skew 3 | s | 0 | **D** |
| `nova.lfo.<l>.delay` | lin 0..10, skew 3 | s | 0 | **D** |
| `nova.lfo.<l>.smooth` | lin 0..1 | % | 0 | **D** |
| `nova.lfo.<l>.phase` | lin 0..360 | ° | 0 | **D** |
| `nova.lfo.<l>.scope` | choice [Voice, Global] | | Voice | |
| `nova.lfo.<l>.grid` | step 1..32 | | 8 | Editor snap grid only; not heard. |

The shape is curve `lfo.<l>` (§3.2). Polarity belongs to each route (§3.1), so an LFO has no polarity parameter; the panel's BIPOLAR pill sets the polarity of all of that LFO's routes.

### 2.7 Macros and chaos

| Id | Range | Unit | Default |
|---|---|---|---|
| `nova.macro.<m>` (`<m>` = 1..8) | lin 0..1 | % | 0 |
| `nova.chaos.<c>.rate` (`<c>` = 1 Lorenz, 2 logistic) | log 0.01..50 | Hz | 1 |
| `nova.chaos.<c>.smooth` | lin 0..1 | % | 0.5 |

All are **D**. Macro names are labels `macro.<m>` (§3.5).

### 2.8 Modulation route slots (×256: `<s>` = 0..255)

| Id | Range | Unit | Default | Notes |
|---|---|---|---|---|
| `nova.mod.<s>.amount` | lin −1..1 | % | 0 | **D** (routes may modulate other routes' amounts). Inert while no route uses slot `<s>`. |

The route itself (source, aux, destination, polarity, curve, output scale) is document data (§3.1).

### 2.9 FX bus settings

| Id | Range | Unit | Default | Notes |
|---|---|---|---|---|
| `nova.fx.split` | choice [Off, On] | | Off | On: Main's modules process only the band above the crossover; the low band bypasses them. |
| `nova.fx.split_frequency` | log 40..2000 | Hz | 150 | **D** |
| `nova.fx.main.oversample` | choice [Off, 2×, 4×] | | 2× | |
| `nova.fx.bus.<b>.oversample` (`<b>` = 1, 2) | choice [Off, 2×, 4×] | | Off | |
| `nova.fx.bus.<b>.send` | lin 0..1 | % | 0 | **D**. Send from Main into the bus. |
| `nova.fx.bus.<b>.send_point` | choice [Pre, Post] | | Post | Before or after Main's modules. |
| `nova.fx.bus.<b>.return` | lin −60..6 | dB | 0 | **D**. Bus return level into Main's output. |

### 2.10 FX slot parameters (×24: `<u>` = 1..24)

Slot uids 1..24 are a pool shared by the three chains (`fx.main`, `fx.bus.1`, `fx.bus.2`, at most 8 modules each). Every uid has the same eighteen parameters; what `p0..p15` mean depends on the module in that slot (§5). Reordering moves the uid, so a module keeps its settings and automation.

| Id | Range | Default | Notes |
|---|---|---|---|
| `nova.fx.<u>.p0` … `nova.fx.<u>.p15` | lin 0..1 | per module (§5) | **D**. Generic knob travel; §5 gives each module's mapping. |
| `nova.fx.<u>.mix` | lin 0..1 | 1 | **D** |
| `nova.fx.<u>.on` | choice [Off, On] | On | Bypass with a 20 ms crossfade (NOVA-FX-03). |

When the panel adds a module to a free uid, it writes that module's defaults (§5) into `p0..p15` (0 where §5 lists none), sets mix and on, then submits the document. A uid with no module in any chain is inert.

### 2.11 Arpeggiator and clip sequencer

| Id | Range | Unit | Default | Notes |
|---|---|---|---|---|
| `nova.arp.on` | choice [Off, On] | | Off | |
| `nova.arp.mode` | choice [Up, Down, Up/Down, Down/Up, Converge, Diverge, Played, Thumb, Pinky, Random, Shuffle, Random walk, Chord, Strum, Shape, Euclidean] | | Up | ADR-0078. Order: Converge plays outside in (lowest, highest, second lowest…), Diverge inside out; Thumb repeats the lowest between the others, Pinky the highest. Chance: Random picks a seeded note each step, Shuffle every note once per cycle in a seeded order, Random walk a place or two from the last (restarting from the first every 64 steps). Harmony: Chord plays every held note, Strum spreads them over `strum` of the step, alternately up and down. Shape: the Note lane is the place in the held notes. Euclidean: Up on the hits of `euclid.*`. |
| `nova.arp.rate` | choice [1/1, 1/2, 1/2 T, 1/4 D, 1/4, 1/4 T, 1/8 D, 1/8, 1/8 T, 1/16 D, 1/16, 1/16 T, 1/32, 1/32 T, 1/64] | | 1/16 | |
| `nova.arp.gate` | lin 0..1 | % | 0.6 | **D** |
| `nova.arp.swing` | lin 0..0.75 | % | 0 | **D** |
| `nova.arp.octaves` | step 1..4 | | 1 | |
| `nova.arp.velocity` | choice [Played, Pattern] | | Played | Played: the played velocity × the Velocity lane; Pattern: the lane alone. |
| `nova.arp.retrigger` | choice [Off, On] | | On | |
| `nova.arp.latch` | choice [Off, On] | | Off | |
| `nova.arp.note_mode` | choice [Held, Scale] | | Held | How the Note lane moves the note: by places in the held notes (wrapping), or by degrees of `scale` (of the held chord when the scale is Chord), crossing octaves. |
| `nova.arp.scale` | choice [Chord, Major, Minor, Dorian, Phrygian, Lydian, Mixolydian, Locrian, Harmonic minor, Melodic minor, Major pentatonic, Minor pentatonic, Blues, Chromatic] | | Chord | Chord is no scale: degrees and the chord lane come from the held notes. |
| `nova.arp.root` | choice [C, C#, D, D#, E, F, F#, G, G#, A, A#, B] | | C | The scale's root. |
| `nova.arp.passing` | choice [Off, On] | | Off | With a scale: its tones between the lowest and highest held note join them (not in Played, Chord or Strum). |
| `nova.arp.strum` | lin 0..1 | % | 0.35 | **D**. Strum: the share of a step (of a hit) the chord spreads over. |
| `nova.arp.euclid.hits` | step 0..32 | | 5 | Euclidean: hits per cycle, at most `steps`. Step i (from the pattern start) is a hit when ((i + rotate) mod steps) × hits mod steps < hits. |
| `nova.arp.euclid.steps` | step 1..32 | | 16 | |
| `nova.arp.euclid.rotate` | step 0..31 | | 0 | |
| `nova.arp.fill` | lin 0..1 | % | 0 | **D** (an automatable switch, on at ≥ 0.5). The panel's FILL holds it while pressed. |
| `nova.arp.variation` | choice [Off, 2 bars, 4 bars, 8 bars, 16 bars] | | Off | The last bar of every N of the song plays as a fill, with the Note and Octave lanes read 1–3 steps on (seeded per bar). |
| `nova.arp.seed` | step 0..9999 | | 0 | Every seeded choice (Random, Shuffle, Random walk, Chance, variation) is a hash of the seed and the step, so any position renders alike. |
| `nova.arp.groove_amount` | lin 0..1 | % | 1 | **D**. How much of the groove's timing and velocity. |
| `nova.arp.groove.steps` | step 0..32 | | 0 | The groove copied from the pool (ADR-0059): 0 is none. Set by the panel's GROOVE list; label `arp.groove` names it. |
| `nova.arp.groove.grid` | choice [1/4, 1/8, 1/8 T, 1/16, 1/16 T, 1/32] | | 1/16 | Its steps per quarter. |
| `nova.arp.groove.<n>.timing` (`<n>` = 1..32) | lin −0.5..0.5 | | 0 | Not automatable. A fraction of a groove step: the arp step on it moves by timing × amount (swing and groove together at most 0.24 of a step early, 0.75 late). |
| `nova.arp.groove.<n>.velocity` | lin 0..2 | | 1 | Not automatable. Velocity × (1 + (v − 1) × amount). |
| `nova.arp.lane.<lane>.length` | step 1..32 | | 16 | Each lane loops at its own length: step k (counted from the pattern start, or the song with Retrigger off) reads value (k mod length). |
| `nova.arp.lane.note.<n>` (`<n>` = 1..32) | step −12..12 | | 0 | Places or degrees (`note_mode`); Shape's note. |
| `nova.arp.lane.octave.<n>` | step −2..2 | | 0 | Octaves on every note of the step. |
| `nova.arp.lane.velocity.<n>` | lin 0..1 | % | 1 | Not automatable. |
| `nova.arp.lane.gate.<n>` | lin 0..1 | % | 1 | Not automatable. Multiplies `nova.arp.gate`; 0 is a rest. |
| `nova.arp.lane.tie.<n>` | choice [Off, On] | | Off | Holds into the next step (a slide with portamento). |
| `nova.arp.lane.ratchet.<n>` | step 1..4 | | 1 | Hits sharing the step; the later ones at 0.8 of the velocity. |
| `nova.arp.lane.chance.<n>` | lin 0..1 | % | 1 | Not automatable. The odds the step plays (seeded). |
| `nova.arp.lane.condition.<n>` | choice [Always, 1:2, 2:2, 1:3, 2:3, 3:3, 1:4, 2:4, 3:4, 4:4, Fill, Not fill, First, Not first] | | Always | X:Y plays on the X-th of every Y passes of the condition lane; Fill while `fill` or on a variation bar; First on the first pass. |
| `nova.arp.lane.accent.<n>` | choice [Off, On] | | Off | +0.2 velocity and the `arp.accent` source (§4). |
| `nova.arp.lane.mod.<n>` | lin 0..1 | % | 0 | Not automatable. The `arp.mod` source (§4) of the step's notes. |
| `nova.arp.lane.chord.<n>` | choice [Off, +Oct, 5th, Triad, 7th, Sus] | | Off | Notes stacked over a single-note mode's note: diatonic to `scale`, or the next held notes when it is Chord. |
| `nova.seq.on` | choice [Off, On] | | Off | |
| `nova.seq.clip` | choice [Clip 1, Clip 2, Clip 3, Clip 4] | | Clip 1 | The clip that plays with the transport. |
| `nova.seq.trigger` | choice [Transport, Note] | | Transport | Note: holding key `root + i` plays clip `i + 1`. |
| `nova.seq.root` | step 0..127 | | 24 (C1) | |
| `nova.seq.follow` | choice [Off, On] | | On | Transpose follows the played key. |

### 2.12 Global

| Id | Range | Unit | Default | Notes |
|---|---|---|---|---|
| `nova.global.voicing` | choice [Poly, Mono, Legato] | | Poly | |
| `nova.global.polyphony` | step 1..32 | | 16 | |
| `nova.global.portamento` | lin 0..10, skew 4 | s | 0 | **D** |
| `nova.global.portamento_curve` | lin −1..1 | % | 0 | **D** |
| `nova.global.portamento_mode` | choice [Legato only, Always] | | Legato only | |
| `nova.global.portamento_rate` | choice [Constant time, Constant rate] | | Constant time | |
| `nova.global.portamento_sync` | choice [Off, On] | | Off | On: the glide lasts `portamento_division` at the tempo when the note starts (per octave in Constant rate) instead of `portamento` seconds. Without a transport Nova keeps 120 BPM. |
| `nova.global.portamento_division` | choice [1/1, 1/2, 1/4 D, 1/4, 1/4 T, 1/8 D, 1/8, 1/8 T, 1/16 D, 1/16, 1/16 T, 1/32 D, 1/32, 1/32 T, 1/64, 1/128] | | 1/32 | The synced glide's note value. |
| `nova.global.drift` | lin 0..1 | % | 0.1 | **D**. Per-voice analog wander. |
| `nova.global.bend_range` | step 0..48 | st | 2 | |
| `nova.global.velocity_curve` | lin −1..1 | % | 0 | Not automatable. |
| `nova.global.mpe` | choice [Off, On] | | Off | |
| `nova.global.mpe_range` | step 0..96 | st | 48 | Per-note bend range. |
| `nova.global.quality` | choice [Eco, High, Ultra] | | High | |
| `nova.global.tuning` | choice [12-TET, Scale file] | | 12-TET | The scale file is a later document field (phase 8). |
| `nova.global.a4` | lin 400..480 | Hz | 440 | Not automatable. |
| `nova.perf.bend` | lin −1..1 | | 0 | Performance input: MIDI pitch bend reaches native synths as automation of this parameter. The panel has no control for it. |
| `nova.perf.wheel` | lin 0..1 | | 0 | MIDI CC 1, the same way. |
| `nova.perf.pressure` | lin 0..1 | | 0 | Channel pressure, the same way. |

The `nova.perf.*` parameters are automatable (that is how MIDI reaches them) but are not offered as modulation destinations; the `bend`, `modwheel` and `aftertouch` sources (§4) read them.

### 2.12a Kick Lock

Kick & Bass Lock ([ADR-0072](../decisions/0072-kick-bass-lock.md)). Nova follows the notes of unmuted Anvil tracks whose `anvil.lock` is on (Bass note, which also retunes the kick's tail, or Keep tone, which does not). Below the crossover the output ducks under each followed hit's own amplitude curve (read ahead from the project, no detector or latency) and can fold to mono; above it the output is untouched. Off is an exact passthrough. The panel shows them as the **KICK LOCK** card on the GLOBAL page.

| Id | Scale | Unit | Default | Notes |
|---|---|---|---|---|
| `nova.lock.on` | choice [Off, On] | | Off | The card's LED. Fades in and out over 20 ms. |
| `nova.lock.duck` | lin 0..1 | % | 0.65 | **D**. How far the low band dips at the kick's peak. |
| `nova.lock.crossover` | log 60..250 | Hz | 120 | **D**. Fourth-order Linkwitz-Riley split; the high band is the input minus the low band. |
| `nova.lock.mono` | choice [Off, On] | | On | Folds the low band to mono. |

### 2.12b Analogue

Nova Analogue ([ADR-0073](../decisions/0073-nova-analogue.md)). Every note voice (by allocator slot, 0..39, or by voice card with `nova.analogue.cards` ≥ 1) and every unison voice (by place in its stack, 0..15) gets fixed offsets from the seed: a part's value is `clamp(u × tolerance × Age × 1.6, −1, 1)` × the part's maximum, `u` seeded in −1..1, unless a Voice Lane value is painted (§3.7). Plus a slow drift per voice. Off renders exactly as without Analogue. The panel shows them on the VOICES page.

| Id | Scale | Unit | Default | Notes |
|---|---|---|---|---|
| `nova.analogue.on` | choice [Off, On] | | Off | |
| `nova.analogue.age` | lin 0..1 | % | 0.35 | **D**. Scales every tolerance together. |
| `nova.analogue.seed` | step 1..9999 | | 4127 | The set of voice cards. |
| `nova.analogue.pitch` | lin 0..1 | % | 0.6 | **D**. Max ±12 cents. |
| `nova.analogue.cutoff` | lin 0..1 | % | 0.5 | **D**. Max ×1.18^±1 (±18 %, symmetric in octaves). |
| `nova.analogue.resonance` | lin 0..1 | % | 0.3 | **D**. Max ×(1 ± 0.10). Notes only; no lane. |
| `nova.analogue.env` | lin 0..1 | % | 0.5 | **D**. Attack, hold, decay and release of all envelopes, max ×1.25^±1. |
| `nova.analogue.level` | lin 0..1 | % | 0.4 | **D**. Max ±1.5 dB. |
| `nova.analogue.pan` | lin 0..1 | % | 0.4 | **D**. Max ±40 %: a note voice's balance; added to a unison voice's pan. |
| `nova.analogue.shape` | lin 0..1 | % | 0.3 | **D**. Max ±8 % on pulse width, position and warp amounts. No lane. |
| `nova.analogue.drift.amount` | lin 0..1 | % | 0.4 | **D**. Wander up to ±8 cents. Not scaled by Age. |
| `nova.analogue.drift.rate` | lin 0..1 | % | 0.3 | **D**. 0.02 + 0.6 × value Hz; each voice ±25 %. |
| `nova.analogue.drift.cutoff` | lin 0..1 | % | 0.25 | **D**. Note voices' cutoff wander, up to ×1.10^±1. |
| `nova.analogue.freerun` | choice [Off, On] | | On | New notes start each oscillator where it would be had it run since the project's start (seeded per voice). Applies at any Age and scope. |
| `nova.analogue.scope` | choice [Notes + unison, Notes only, Unison only] | | Notes + unison | Which voices get offsets and drift. |
| `nova.analogue.cards` | step 0..16 | | 0 | Voice cards. 0 ("Per voice"): note voices are numbered by allocator slot. N: each note that takes a fresh voice takes card `n mod N`, `n` counting such notes since the kernel's last reset (each new transport generation: seek, play from a position, export; mono and legato retriggers keep their card); its offsets, painted `notes.*` values (voice = card), drift and free-running identity and Mod A/B are that card's. The panel's lanes show N bars. |
| `nova.analogue.reroll` | lin 0..1 | % | 0 | **D**. Each note adds `reroll × u′` to every part's value (painted or seeded), scaled like the seeded part (× tolerance × Age × 1.6; Mod A/B unscaled), clamped to −1..1; `u′` seeded from the seed, the voice (card, or place in the stack) and the note's number `n`. Only while Analogue is on (Mod A/B too); 0 is exact. |
| `nova.analogue.flutter.amount` | lin 0..1 | % | 0 | **D**. Fast pitch jitter per voice, up to ±15 cents (useful far lower). Seeded white noise drawn once per control tick through a one-pole low-pass at Tone, normalised to a spread of 0.35 and clamped to ±1. Each note voice and unison voice (per scope) has its own. Not scaled by Age. 0 is exact. |
| `nova.analogue.flutter.tone` | log 2..60 | Hz | 12 | **D**. The flutter noise's low-pass corner: how fast it jitters. |
| `nova.analogue.glide` | lin 0..1 | % | 0.3 | **D**. Glide tolerance: each note voice's portamento time scales by (1.35)^offset, so worn cards slide a little faster or slower. A seeded part appended after the others (their seeds are unchanged); in mono, voice cards or Re-roll make successive slides differ. |

### 2.12c Noise AM

The noise layer's level follows an oscillator of the same voice at audio rate, as when a noise oscillator's level is modulated by oscillator A. The followed signal is that oscillator's output before its level knob (so an oscillator at level 0 can drive it), `x` ≈ −1..1, mapped to `u = clamp((x + 1) / 2, 0, 1)`; the noise is multiplied by `1 − amount + amount × u^(2^(2 × curve))`. Off renders exactly as without it. Shown on the OSC page's Sub and Noise card.

| Id | Scale | Unit | Default | Notes |
|---|---|---|---|---|
| `nova.noise.am.source` | choice [Off, Osc A, Osc B, Osc C] | | Off | |
| `nova.noise.am.amount` | lin 0..1 | % | 1 | **D**. 0 leaves the noise unchanged. |
| `nova.noise.am.curve` | lin −1..1 | % | 0 | **D**. The exponent 2^(2 × curve), 0.25..4: below zero the noise stays up through most of the cycle (smoother), above zero it sounds only near the oscillator's peaks (gated, buzzier). |

### 2.13 Count

| Block | Parameters |
|---|---|
| Master | 1 |
| Oscillators | 153 |
| Sub and noise | 14 |
| Filters and routing | 19 |
| Envelopes | 44 |
| LFOs | 72 |
| Macros and chaos | 12 |
| Route amounts | 256 |
| FX bus settings | 11 |
| FX slots | 432 |
| Arpeggiator and sequencer | 455 |
| Global and performance | 17 |
| Kick Lock | 4 |
| Analogue | 19 |
| Noise AM | 3 |
| Portamento sync, Analogue glide | 3 |
| **Total** | **1515** (limit 2048) |

## 3. Document conventions

The panel edits the document through `NativeSynthDocumentModel` (`native_synth_document_model.hpp`), which submits whole documents through `NativeSynthPanelAdapter::submit_document`.

### 3.1 Routes

`NativeModRoute{slot, source, aux, destination, bipolar, tension, scale}`.

- `slot` 0..255 owns `nova.mod.<slot>.amount`. A new route takes the lowest slot above every slot in use, and only when 255 is taken the lowest free one, so a deleted route's slot (and any automation left on it) is reused as late as possible.
- `source` and `aux` are source ids (§4). `aux` is empty for none.
- `destination` is the full id of a **D** parameter, including `nova.mod.<s>.amount` and `nova.fx.<u>.p<k>`. One source may reach one destination once; the panel's drop on an existing pair selects that route instead of adding another.
- **Signal.** Let `s` be the source's unipolar value 0..1 (§4) and `a` the aux's (1 when empty). The route's signal is `r = bipolar ? 2s − 1 : s`, shaped by `bend(|r|, tension) · sign(r)`, times `a`, times `scale`. Its contribution to the destination is `amount × that`, in the destination's **normalised** units (knob travel), added to the knob position and clamped to 0..1.
- **Output scale.** `scale` 0..1 (default 1) is the matrix's OUTPUT column, held on the route (`NativeModRoute::scale`, codec key `"scale"`) rather than as a flat parameter: the flat table has no room for 256 more slots (§2.13), and the scale is rarely automated.
- A drop creates `{bipolar: false, tension: 0, scale: 1}` and sets the amount to +30 %.

### 3.2 Curves

`NativeCurve{id, points}`; points `{x, y, tension, segment}` with x strictly increasing from 0 to 1 and y in 0..1.

| Id | Use |
|---|---|
| `lfo.1` … `lfo.8` | LFO shapes. A missing curve plays the default triangle `(0, 0) (0.5, 1) (1, 0)`; the panel writes the curve on the first edit. |
| `warp.<o>.<w>` | The Remap warp's transfer curve for oscillator `<o>`, slot `<w>`. Missing: identity `(0, 0) (1, 1)`. |

**Bend.** A segment from point *i* to *i* + 1 at fraction `t` is `y_i + (y_{i+1} − y_i) · u`, where `u = t` for tension 0, `u = t^(1 + 4k)` for tension `k > 0` and `u = 1 − (1 − t)^(1 − 4k)` for `k < 0`. A `step` segment holds `y_i`. Route curves and envelope segment curves use the same `bend(t, k)`.

**Editing.** A point drag is one document gesture (begin, update…, commit). The first and last points keep x = 0 and x = 1, and their y values move together so the cycle loops seamlessly. Double-click adds a point; right-click removes one; Alt-drag on a segment sets its tension; the S key or the menu toggles a segment between curve and step.

### 3.3 Chains

`NativeChain{id, modules}` with ids `fx.main`, `fx.bus.1`, `fx.bus.2`, each at most 8 `NativeChainSlot{uid, module}`. Module ids are in §5. A missing chain is empty. Uids are unique across all chains and come from 1..24.

### 3.4 Media

| Id | Holds | `frame_length` |
|---|---|---|
| `osc.a`, `osc.b`, `osc.c` | The oscillator's wavetable (Wavetable, Granular from a table) or sample (Sample, Multisample, Granular, Spectral) | 2048 for a table, 0 for a sample |
| `noise` | The noise oscillator's audio | 0 |

**Built-in names** (`NativeBuiltinMedia{name}`) the engine generates. The panel lists exactly these until the engine publishes its own list through the controller:

- Tables: `basic_shapes`, `glass_formant`, `reese_saw`, `vowel_morph`, `fm_bell`, `grit_fold`, `spectral_pad`, `analog_pulse`.
- Noises: `white`, `pink`, `brown`, `analog_hiss`, `vinyl`, `breath`.

A missing `osc.<o>` slot plays `basic_shapes`; a missing `noise` slot plays `white`.

### 3.5 Labels

`macro.1` … `macro.8`: the macro names shown under the knobs (at most 64 bytes). Missing: "Macro n".

`arp.pattern` and `arp.groove` (ADR-0078): the arp pattern and the groove last loaded, shown on the ARP · CLIP page. They only name what the parameters hold.

### 3.6 Sequences

`clip.1` … `clip.4`. The length is at most 4 bars of 4/4 (`16 × ticks_per_quarter`); the default is 1 bar. The panel's grid is a 1/16 grid; it toggles notes 1/16 long at velocity 100. The panel shows starts and lengths in quarter notes and converts with `timeline::ticks_per_quarter`.

### 3.7 Voice lanes

`NativeVoiceLane{id, values}` with values `NativeVoiceValue{voice, value}` (codec key `"voice_lanes"`, written only when non-empty): the Voice Lanes a user painted (§2.12b). Voices strictly increase; values are −1..1; at most 64 lanes of 64 values. A voice not listed keeps its seeded value.

| Id | Voices | Lanes |
|---|---|---|
| `notes.<lane>` | 0..39 (allocator slots; with voice cards, the card 0..N−1) | `pitch`, `cutoff`, `env`, `level`, `pan`, `phase`, `moda`, `modb` |
| `unison.<o>.<lane>` (`<o>` = `a`, `b`, `c`) | 0..15 | `pitch`, `level`, `pan`, `phase` |

A lane value of ±1 is the part's maximum (§2.12b). `phase` offsets the start phase by ±180° when Free-running is off. `moda` and `modb` are not scaled by Age and feed `voice.mod_a` and `voice.mod_b` (§4). Nova rejects an unknown id, a lane the kind lacks, a voice out of range or a non-finite value.

**Panel API** (on `controller.document`): `analogueLane(kind, osc, lane, count)` returns `[{value, edited}]` for voices 0..count−1 exactly as the engine plays them (`nova::analogue_lane_values`); it reads the current values, so re-read it when parameters change. `setAnalogueLaneValue(kind, osc, lane, voice, value)` paints one voice; between `begin_edit` and `commit_edit` the writes are one undo step. `clearAnalogueLane(kind, osc, lane)` returns a lane to the seed. `analogue_revision` increments whenever `voice_lanes` change, including undo and redo.

## 4. Modulation sources

Every source has a unipolar value `s` in 0..1, which telemetry publishes (§6) and routes use (§3.1).

| Id | Panel name | `s` |
|---|---|---|
| `env.1` … `env.4` | ENV 1–4 | The envelope level. Per voice. |
| `lfo.1` … `lfo.8` | LFO 1–8 | The curve's y at the LFO phase. Per voice or global (`scope`). |
| `macro.1` … `macro.8` | the macro's label | `nova.macro.<m>` |
| `velocity` | Velocity | velocity / 127 |
| `note` | Note | key / 127 |
| `modwheel` | Mod Wheel | CC 1 |
| `aftertouch` | Aftertouch | channel pressure, or the note's poly pressure |
| `mpe.slide` | Slide | MPE CC 74 |
| `mpe.pressure` | Pressure | MPE per-note pressure |
| `chaos.1` | Chaos 1 | Lorenz x, mapped to 0..1 |
| `chaos.2` | Chaos 2 | logistic map, 0..1 |
| `random` | Random | a new value per note |
| `bend` | Bend | (pitch bend + 1) / 2 |
| `voice.mod_a` | Voice Mod A | The note voice's Mod A lane (§3.7), (v + 1) / 2. Available whether or not Analogue is on. |
| `voice.mod_b` | Voice Mod B | The same for Mod B. |
| `voice.spread` | Voice Spread | 0.5 for a note voice (the matrix runs per note voice). |
| `arp.mod` | Arp Mod | The Mod lane's value on the arp step that started the voice's note (ADR-0078); 0 for a played note. |
| `arp.accent` | Arp Accent | 1 when that step was accented, else 0. |

## 5. FX modules

Module ids and their `p` mappings. Unlisted `p` values are unused and stay 0. **Mode** is a choice carried on a `p` value: index = `round(p × (n − 1))`, and the panel writes `index / (n − 1)`. The module strip shows a knob for every `p` value the mapping uses (EQ: its band nodes and their controls), MIX and the mode menu, so each parameter a module uses has a control.

| Module id | Main knobs | Mapping | Defaults |
|---|---|---|---|
| `distortion` | DRIVE p0, TONE p1, BIAS p2, MIX | p0 0..48 dB; p1 tilt −1..1 (`2p − 1`); p2 bias −1..1; **p7 mode** [Tube, Soft, Hard, Diode, Fold, Rectify, Sine Fold, Asym, Bit Crush, Downsample, X-Shaper, Overdrive] | p0 0.3, p1 0.5, p2 0.5, p7 0, mix 1 |
| `filter` | CUTOFF p0, RES p1, DRIVE p2, MIX | p0 log 8..22000 Hz; p1 0..1; p2 0..24 dB; **p7 model** (the 22 filter models, §2.4) | p0 0.6, p1 0.2, p2 0, p7 0 |
| `chorus` | RATE p0, DEPTH p1, FEEDBK p2, MIX | p0 log 0.01..10 Hz; p1 0..1; p2 0..0.95; p3 delay 1..30 ms; **p7 voices** [2, 4] | p0 0.45, p1 0.4, p2 0, p3 0.3, mix 0.5 |
| `flanger` | RATE p0, DEPTH p1, FEEDBK p2, MIX | p0 log 0.01..10 Hz; p1 0..1; p2 −0.95..0.95 (`1.9p − 0.95`); **p7 mode** [Classic, Through-zero] | p0 0.35, p1 0.5, p2 0.7, mix 0.5 |
| `phaser` | RATE p0, DEPTH p1, FEEDBK p2, MIX | p0 log 0.01..10 Hz; p1 0..1; p2 0..0.95; p3 centre log 100..8000 Hz; **p7 stages** [4, 8, 12] | p0 0.35, p1 0.6, p2 0.4, p3 0.5, p7 0.5, mix 0.5 |
| `delay` | TIME p0, FEEDBK p1, HP/LP p2, MIX | p0 log 1..2000 ms, or with p5 ≥ 0.5 the division `round(p0 × 14)` of [1/64, 1/32 T, 1/32, 1/16 T, 1/16, 1/16 D, 1/8 T, 1/8, 1/8 D, 1/4 T, 1/4, 1/4 D, 1/2, 1/2 D, 1/1]; p1 0..1.1 (`1.1p`); p2 feedback filter −1 (LP) .. +1 (HP); p5 sync (≥ 0.5 on); **p7 mode** [Stereo, Ping-pong, Tape] | p0 8/14, p1 0.4, p2 0.5, p5 1, p7 0.5, mix 0.25 |
| `reverb` | SIZE p0, DECAY p1, DAMP p2, MIX | p0 0..1; p1 log 0.1..30 s; p2 0..1; p3 pre-delay 0..250 ms; **p7 type** [Hall, Plate, Room, Shimmer] | p0 0.6, p1 0.5, p2 0.4, p3 0.1, mix 0.25 |
| `compressor` | THRESH p0, RATIO p1, ATTACK p2, MIX | p0 −60..0 dB; p1 log 1..20; p2 log 0.1..100 ms; p3 release log 10..1000 ms; p4 makeup 0..24 dB; **p7 sidechain** [Self, Voice sum] | p0 0.7, p1 0.4, p2 0.5, p3 0.5, p4 0, mix 1 |
| `multiband` | DEPTH p0, TIME p1, UP/DN p2, MIX | p0 0..1; p1 0..1 (attack and release scale); p2 −1 (down only) .. +1 (up only); p3, p4, p5 low, mid, high output −12..12 dB | p0 0.5, p1 0.5, p2 0.5, p3 0.5, p4 0.5, p5 0.5, mix 1 |
| `eq` | four band nodes | Band b = 0..3: p(4b) **type** [Off, Bell, Low Shelf, High Shelf, Low Cut 12, Low Cut 24, High Cut 12, High Cut 24, Notch, Band Pass]; p(4b+1) frequency log 20..20000 Hz; p(4b+2) gain −24..24 dB (bells and shelves); p(4b+3) Q log 0.25..16. Simper SVF sections; the 24 dB cuts are a Butterworth pair whose peaked section's Q scales with the band's (Q 0.707 is fourth-order Butterworth). A type change crossfades over 10 ms. | Low Shelf 100 Hz, Bell 400 Hz, Bell 2.5 kHz, High Shelf 8 kHz, all 0 dB; Q 0.707 on the shelves, 1 on the bells; mix 1 |
| `dimension` | SIZE p0, WIDTH p1, MOTION p2, MIX | p0 0..1; p1 0..1; p2 0..1 | p0 0.5, p1 0.7, p2 0.3, mix 0.4 |
| `hyper` | DETUNE p0, RATE p1, WIDTH p2, MIX | p0 0..1 (up to ±50 ct); p1 log 0.05..5 Hz; p2 0..1; **p7 voices** [2, 3, 4, 5, 6, 7] | p0 0.3, p1 0.4, p2 0.8, p7 0.6, mix 0.5 |
| `utility` | GAIN p0, WIDTH p1, MONO p2, PAN p3 | p0 −24..24 dB; p1 0..2 (`2p`); p2 bass mono below log 20..500 Hz, off at 0; p3 −1..1 | p0 0.5, p1 0.5, p2 0, p3 0.5, mix 1 |
| `convolve` | PRE p0, LOW CUT p1, HIGH CUT p2, MIX | p0 0..250 ms; p1 log 20..2000 Hz; p2 log 1000..20000 Hz. The IR is media slot `fx.<uid>.ir` (managed audio). | p0 0, p1 0, p2 1, mix 0.3 |
| `shifter` | SHIFT p0, FEEDBK p1, SPREAD p2, MIX | p0 shift `5000 (2p − 1)³` Hz; p1 feedback 0..0.9 (soft-clipped: the barber-pole); p2 spread 0..1 (the right channel shifts by `shift × (1 − 2 p2)`); **p7 mode** [Shift, Ring] (Ring keeps both sidebands). A Hilbert pair of allpass chains and a quadrature oscillator. | p0 0.6 (+40 Hz), p1 0, p2 0, p7 0, mix 0.5 |
| `flyby` | PATH p0, DISTANCE p1, SPEED p2, MIX | The Flyby effect (ADR-0067) on a preset path. **p0 path** [Left → Right, Right → Left, Arc Over, Orbit, Overhead]; p1 closest distance log 1..60 m (straight paths reach 4× it either side, 30..150 m; Orbit's radius; Overhead's height); p2 speed log 1..1000 km/h; **p3 run** [Loop, Sync, Position]; **p4 length** [1/4 Bar, 1/2 Bar, 1 Bar, 2 Bars, 4 Bars, 8 Bars, 16 Bars] (Sync keeps time with the tempo from where the module started: the rack knows the tempo, not the song position); p5 position 0..1; p6 Doppler 0..200 % (`2p`); **p7 listen** [Headphones, Speakers]; p8 air 0..400 % (`4p`); p9 room 0..1; p10 height 0..150 m (not Overhead). | p0 0, p1 14 m, p2 90 km/h, p3 0, p4 3/6, p5 0, p6 0.5, p7 0, p8 0.25, p9 0.35, p10 0, mix 1 |

## 6. Telemetry

One float frame per instance, published at about 60 Hz through the existing telemetry vector (`max_telemetry_values = 2048`). Per-voice values come from the newest sounding voice. Every value is finite. Size: **2038** floats.

| Offset | Count | Value |
|---|---|---|
| 0 | 1 | Layout version, `1`. |
| 1 | 1 | Voices sounding. |
| 2 | 1 | Polyphony limit. |
| 3 | 1 | CPU: the last block's processing time over the block's duration (0..1, may exceed 1). |
| 4 | 1 | Newest note key, or −1. |
| 5 | 1 | Transport playing, 0 or 1. |
| 6 | 2 | Reserved, 0. |
| 8 | 3 | Oscillators A, B, C: modulated position 0..1. |
| 11 | 3 | Oscillators A, B, C: unison voices sounding, or grains sounding for Granular. |
| 14 | 5 | Peaks (linear 0..1+) of A, B, C, Sub, Noise. |
| 19 | 2 | Filters 1, 2: modulated cutoff, Hz. |
| 21 | 2 | Filters 1, 2: modulated resonance, 0..1. |
| 23 | 4 | ENV 1–4 level 0..1. |
| 27 | 4 | ENV 1–4 stage: 0 idle, 1 attack, 2 hold, 3 decay, 4 sustain, 5 release. |
| 31 | 4 | ENV 1–4 progress through the stage, 0..1. |
| 35 | 8 | LFO 1–8 value `s` 0..1. |
| 43 | 8 | LFO 1–8 phase 0..1. |
| 51 | 8 | Macro 1–8 value `s`. |
| 59 | 10 | `velocity`, `note`, `modwheel`, `aftertouch`, `mpe.slide`, `mpe.pressure`, `chaos.1`, `chaos.2`, `random`, `bend`, as `s`. |
| 69 | 8 | Peak meters, dBFS (−120 floor), L and R of Main, Bus 1, Bus 2 and the output after `nova.master`. |
| 77 | 1 | Clip sequencer clip playing, 0..3, or −1. |
| 78 | 1 | Clip playhead, quarter notes from the clip start. |
| 79 | 1 | The arpeggiator's pattern step, counted from the pattern start (each lane shows it modulo its length), or −1. |
| 80 | 256 | Route slot 0..255: its current contribution in normalised destination units (§3.1); 0 when unused. |
| 336 | 384 | Oscillators A, B, C: 128 points each of one cycle at the modulated position, peak-normalised −1..1 (Classic: the waveform with PW/sync/warps; Wavetable: the frame with warps; others: the newest grain or analysis frame, or zeros). |
| 720 | 24 | FX uid 1..24: a module-specific live value. Compressor and Multiband: gain reduction in dB (positive). Chorus, Flanger, Phaser, Hyper: modulation phase 0..1. Delay: the time in ms. Shifter: the left shift in Hz. Flyby: the source's distance in metres. Others: output peak, dBFS. |
| 744 | 24 | FX uid 1..24: input peak, dBFS (−120 floor; falls 24 dB/s). −120 for a uid with no module. |
| 768 | 24 | FX uid 1..24: output peak after the slot's mix, dBFS. |
| 792 | 72 | FX uid 1..24 × 3: detail. Multiband: the low, mid and high band gain change in dB (negative pushed down, positive lifted). Others 0. |
| 864 | 1152 | FX uid 1..24 × 48: the slot's output spectrum (after its mix), dBFS, band i centred at `20 × 1000^((i + 0.5) / 48)` Hz; the peak bin in each band, analysed 30 times a second over a Hann window of 1024 frames at 48 kHz (longer at higher rates), falling 36 dB/s. |
| 2016 | 3 | `voice.mod_a`, `voice.mod_b`, `voice.spread` as `s` (0.5 with no voice). |
| 2019 | 8 | Note voices 0..7 (by voice card when `nova.analogue.cards` ≥ 1, else by slot; the newest note on a card): Analogue's pitch drift now, cents; 0 when silent or off. |
| 2027 | 8 | Note voices 0..7, as above: the key playing, or −1. |
| 2035 | 2 | `arp.mod`, `arp.accent` as `s` (0 with no voice). |
| 2037 | 1 | 1 while the arp step playing is on a variation bar, else 0. |

**The panel's use.** Knob value arcs follow the saved value; the violet live dot of each route sits at `value + contribution` (clamped). The wavetable stack highlights the frame at the modulated position and draws the published cycle at the front. Filter curves draw at the modulated cutoff and resonance. The LFO and envelope editors draw an amber playhead at the phase or stage. VOICES and CPU come from offsets 1–3.

**Wavetable stack frames.** Telemetry carries only the current cycle. The other frames in the 3D stack come from `NativeSynthVisuals` once the engine exists: **root** adds a Nova entry with `table_frames(document media slot, frames, points)` and exposes it on the device controller; until then the panel draws a deterministic preview per built-in table name.

## 7. Panel-side behaviour root must know

- **Drop.** Sets `nova.mod.<slot>.amount` to 0.3 (one value edit), then submits the document with the new route (one document edit). Two undo steps today; one combined command can follow later.
- **Remove a route.** Submits the document without the route and resets its amount to 0, so a later route in that slot starts clean.
- **Depth drag.** A value gesture on `nova.mod.<slot>.amount` (begin, update…, commit), like any knob.
- **LFO point drag, FX reorder and every other structural edit** go through `submit_document`, with gestures for drags.
- **Add effect.** Picks the lowest uid not in any chain, writes the module defaults (§5) as one value edit (`set_values`), then appends `{uid, module}` to the selected chain. Changing a slot's module writes the new module's defaults the same way and keeps the uid.
- **Wavetable editor DONE.** Today it commits the oscillator's built-in table choice (media slot) as one document edit. Drawn frames need managed-media writes and are phase 6.
- **Arpeggiator steps** are drawn as bars: drag for velocity, the strip below for gate, ⌒ for tie, double-click for a rest.
- **Drag to arrangement.** The panel emits `sequenceDragged("clip.<n>")`; the host makes the MIDI clip (NOVA-ARP-03). **Root** wires it in `NativeSynthEditorOverlay.qml` once the session command exists.
- **Presets.** `apply_preset` replaces the flat values only. Nova presets also carry a document (`preset_document`), so **root** extends the controller's preset path to submit both when the engine lands.
