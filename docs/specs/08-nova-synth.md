# 08 — Nova flagship synth

**Status:** Draft for sign-off. **Decision:** [ADR-0057](../decisions/0057-nova-flagship-synth.md). **Panel design:** [nova-panel-mockup.html](../ui/nova-panel-mockup.html). Open it in a browser; it is interactive.

This spec is written so that a contributor who has read AGENTS.md, the HLD, ADR-0027 and this file can work on Nova without further context. Requirement IDs are stable; tests cite them.

**Quality bar.** Sound first ("Proper modern sounds are huge. This shouldn't be basic."). CPU budgets are soft, but every heavy path has an ECO / HIGH / ULTRA quality setting. Determinism and export parity are hard requirements.

---

## 1. Scope summary

| Area | Nova |
|---|---|
| Oscillators | 3 main (A, B, C). Types: Classic, Wavetable, Sample, Multisample, Granular, Spectral. Plus Sub and Noise. |
| Unison | Up to 16 voices per oscillator, with detune, a detune curve, blend, width, stack modes, phase and position/warp spread. |
| Warps | 2 warp slots per oscillator, each with 16 modes, including cross-oscillator FM, PM, AM and RM. |
| Filters | 2, with 20+ models. Serial, parallel or split routing; each source picks its filter. |
| Modulation | 4 envelopes, 8 LFOs (drawable point-and-curve), 8 macros, velocity, note, mod wheel, aftertouch, MPE (bend, slide, pressure), 2 chaos sources, random, pitch bend. Drag to modulate, with unlimited routes up to 256. |
| FX | A reorderable rack with Main plus 2 buses: 8 slots each, 16 module types (including a 4-band parametric EQ, a frequency shifter and Flyby). |
| Note generation | Arpeggiator, plus a 4-clip sequencer. |
| Editor | Wavetable editor: draw, harmonics, formula, audio resynthesis, import and export. |
| Presets | 150 or more factory presets, at least 10 per category, with tags. |

---

## 2. Panel (build this first, to the mockup)

The mockup is the visual and interaction contract. QML lives in `apps/daw_quick/qml/nova/` and talks only through `NativeSynthPanelAdapter` plus the new document adapter (§9.6). The redesign ([ADR-0076](../decisions/0076-nova-panel-redesign.md), mockup `docs/ui/nova-redesign-mockup.html`) supersedes the original mockup's shell: header, navigation, browser, size and colours. The panel reflows to the area its window gives it, at the zoom the user picks; at or above 1180 × 720 it is never scaled down (a page that needs more room than it gets is laid out at its own minimum and scaled to fit). Its designed size is 1360 × 932.

| ID | Requirement |
|---|---|
| NOVA-UI-01 | The **header** holds the NOVA wordmark; one preset box (◀ name CATEGORY ▶, with EDITED once changed; the name opens the browser); **BROWSE** (the preset drawer) and **✦ CONSTELLATION**; the **page buttons** MAIN / FX / MATRIX / ARP · CLIP / VOICES / GLOBAL; the **keys pill** (green KEYS PLAY NOTES, or amber TYPING IN SEARCH · ESC TO PLAY while a browser field has the keys); and **zoom chips** 85 / 100 / 120 / 140 %. Narrow, the header is compact (✦ alone, the pill as its light). Under it a **slim signal strip** (ADR-0076 revision 2): OSC A › OSC B › OSC C › SUB · NOISE › FILTER 1 › FILTER 2 › FX › VOICES › OUT, each a light (grey while off), its name and a small live thumbnail; a sound module's item shows MAIN, scrolls to its card and flashes it; FX, VOICES and OUT open the FX, VOICES and GLOBAL pages. A **status line** above the keys shows the **TOUCH A CONTROL** readout (the last touched control's name and value, or a route's "Source → Target n%"), voices in use, CPU and the Live badge. A **key strip** along the bottom plays notes on the synth's track: white keys labelled A S D F G H J K L ; and black keys W E T Y U O P from the typing keyboard's octave, held while pressed, lit while held or sounding. The floating window resizes from its corner and edges; the window size and zoom persist. The **preset browser** is a drawer left of the pages that opens with its list focused, so the typing keyboard keeps playing: ↑/↓ audition, "/" or a click searches, Escape returns to the list, Return or Escape in the list closes; its chips are the categories and ★ favourites, with the tags behind #TAGS. |
| NOVA-UI-02 | **Overview** (MAIN, page 0, ADR-0076 revision 2, mockup `docs/ui/nova-overview-mockup.html`): a card per module in a grid, OSC A / OSC B / OSC C / SUB · NOISE, then FILTER 1 / FILTER 2 / ROUTING, then MODULATION / MACROS; two columns below 1050 px, scrolling when short. Each card has a header (on LED, bold mono title, type dropdown, a sub-label in the module's accent, ⤢), its display and every control its module has: the main knobs at full size, the secondary controls as small knobs in groups under small rotated labels. Every parameter is visible on MAIN without opening anything. A card whose module is off fades (0.42) and rises under the pointer. ⤢ or a double-click on a title opens the card large over the overview with a scrim and a bigger display (`focusModule`); Esc, ⤢ or a click on the scrim restores it. |
| NOVA-UI-03 | Each **oscillator's card** has its on LED and type selector in the header and what it plays under its display (table with ‹ › arrows, SPECTRAL and ✎ EDIT; the classic shape; the sample loop and keytrack; the spectral FFT and freeze). Primary knobs: LEVEL, the type's main knobs (POS and WARP 1–2 with their modes beneath; PW, SYNC, DRIFT; START and the loop; SIZE, DENSITY, POS, SPRAY; POS, SHIFT, STRETCH) and PAN. Secondary, small: UNISON (voices, detune, blend, width, position spread for wavetables and granular, curve, stack), the type's extras (WARP spread and classic warp, SAMPLE, GRAIN, SPECTRAL) and PITCH (octave, semitone, fine, phase, random phase). |
| NOVA-UI-04 | The **wavetable display** is a 3D frame stack with the current frame highlighted. It follows the *modulated* position live. The "✎ EDIT" button opens the editor overlay. |
| NOVA-UI-05 | **Knobs:** the value arc is the module's accent (NOVA-UI-16). Each route into a knob draws a violet outer arc spanning its depth, plus a live violet dot at the current modulated value. Dragging changes the value (Shift = fine). Double-click resets. Dragging on the ring, or Alt-drag, changes the newest route's depth. Hovering a modulated knob shows a popover listing its routes. Right-click opens a menu: remove a route, edit it in the matrix, automate, MIDI-learn. |
| NOVA-UI-06 | **Drag to modulate:** every source can be dragged onto any knob: its tab on the MODULATION card (envelopes, LFOs, performance and voice sources, macros) or a ✥ handle (each macro, the matrix, the Voices page). Dropping it creates a route at 30 %, and the readout confirms. A knob under the drag shows a violet ring. While a card is open large, only its knobs take a drop. |
| NOVA-UI-07 | **Modulation sources** are tabs on the MODULATION card, envelopes amber and the rest violet, each with its route count raised; "CLICK TO EDIT · DRAG ONTO ANY KNOB" on the right. A click on ENV 1–4 or LFO 1–8 selects it (`modTab`, ENV 1 · AMP at first): its envelope or drawable LFO shape fills the left of the card and its controls (§6) the right, the stages or timing at full size, the curves and velocity small. A macro's tab shows the MACROS card. |
| NOVA-UI-08 | **Filters**, a card each: the on LED and a model selector grouped Analog / Clean / Character / Morph in the header with the cutoff on the right; the display is the live response curve including modulation, draggable for cutoff and resonance; primary knobs CUTOFF, RESONANCE, DRIVE and FAT, secondary small KEYTRACK, PAN and MIX. |
| NOVA-UI-09 | **Routing**, its own card beside the filters: a SERIAL / PARALLEL / SPLIT selector and a source × {F1, F2, DIRECT} grid, with the master level (OUT). |
| NOVA-UI-10 | The **macros**, a card on MAIN (and a macro's tab jumps to it): 8 knobs with preset-defined names and ✥ handles. A macro can be renamed by double-clicking its name. |
| NOVA-UI-11 | **FX page (Serum 2-style rack, mockup `docs/ui/nova-fx-rack-mockup.html`):** a left column with the bus tabs (MAIN / BUS 1 / BUS 2, each with its count and a live meter), the bus routing (send, pre/post, return), the chain as a top-to-bottom signal-flow list (each row dragged whole to reorder, dropped on a bus tab to move or, with Ctrl, copy; an LED, in/out meter and a ⋯ menu: duplicate, move, copy/paste settings, save preset, reset, remove), "+ Add effect", SPLIT and OVERSAMPLE. On the right every module is a full-width strip in chain order with a live display from telemetry and every parameter as a control, a mode menu, per-module factory and user presets, and collapse. Adding or swapping a module opens a categorised browser with search. |
| NOVA-UI-12 | The **MATRIX page** has one row per route, including routes made by dragging. Columns: source, aux source (multiplier), amount (bipolar bar), curve (editable), UNI/BI, destination, output scale, delete. The page has + ADD ROUTE and a filter. |
| NOVA-UI-13 | The **ARP · CLIP page** has the arpeggiator card, a 16-step pattern (velocity, gate and tie per step), and the clip sequencer: a piano grid with 4 clips, note-range triggers, and drag-to-arrangement, which creates a MIDI clip through existing session commands. |
| NOVA-UI-14 | The **GLOBAL page** has Voicing (poly/mono/legato, polyphony up to 32, portamento with curve, drift), Playing (bend range, velocity curve, MPE on/off and range) and Quality (ECO/HIGH/ULTRA, tuning: 12-TET with A4, or a .scl/.kbm import). |
| NOVA-UI-15 | The **wavetable editor overlay** has DRAW / FORMULA / IMPORT AUDIO → RESYNTH / FROM IMAGE; MORPH (crossfade, spectral); NORMALISE, REMOVE DC and EXPORT .WAV; a large frame editor; a draggable harmonics view; a formula field; and a frame strip. UNDO lives in the editor, and DONE commits one undoable document edit. |
| NOVA-UI-16 | **Visual tokens:** true neutral graphite with no blue in the greys (bg `#0a0a0b`, plate `#0f1011`, rail `#0c0c0d`, cards `#151618` / `#1b1c1f`, well `#08090a`, lines `#232427` / `#303236`, text `#ececee`, dim `#9a9ca3`, faint `#63656c`); fonts Inter and JetBrains Mono. Each module has one accent, matching its spine tile, on its knobs' arcs, toggles, selected segments, display traces and focus glow: oscillators cyan `#46d6ff`, SUB · NOISE green `#5fe3a1`, filters and envelopes amber `#f5b14c`, FX pink `#ff6fb5`, voices, LFOs and the other sources violet `#a98bff` (violet also marks every route), OUT and global settings neutral white. The wordmark and ✦ CONSTELLATION keep the cyan-violet-pink sweep; preset categories have their own colours in the browser. Nova's floating window chrome is the same graphite. |
| NOVA-UI-17 | The panel never blocks the GUI thread on DSP. Displays read telemetry (§10). Animations are clock-driven, per ADR-0054. |

**Acceptance:**
- A Quick UI test (`nova_panel_tests`) screenshots every page against the mockup layout. It drives a drag-to-modulate drop, a ring-depth drag, a type switch and the editor DONE, and asserts the adapter submissions.
- `NOVA_PANEL_SCREENSHOT_DIR` writes the PNGs for human review, the same pattern as the other panels.

---

## 3. Oscillators

The voice runs at 2× internal rate in HIGH and 4× in ULTRA. ECO runs at 1× and halves unison above 8 voices.

| ID | Requirement |
|---|---|
| NOVA-OSC-01 | **Classic:** band-limited sine, triangle, saw, square and pulse. Uses PolyBLEP/BLAMP, or minBLEP if it is better on the oracle. PWM 1–99 %; hard sync ratio 1–8×, band-limited; drift (analog pitch and phase wander, seeded). Reuses `oscillator.hpp`. |
| NOVA-OSC-02 | **Wavetable:** up to 256 frames × 2048 samples. Mip-mapped per octave (one band-limited copy per octave, built at load). Interpolation between frames: linear crossfade, or spectral morph (per-harmonic magnitude and phase interpolation, precomputed). Frame position is modulatable. Builds on `wavetable.hpp`. |
| NOVA-OSC-03 | **Sample:** one sample with root note, start, loop start and end, loop crossfade and direction (fwd / ping-pong / rev). Pitched with a windowed-sinc resampler; keytrack can be turned off. |
| NOVA-OSC-04 | **Multisample:** a key and velocity zone map built from a folder or an SFZ subset (regions, lokey/hikey, lovel/hivel, pitch_keycenter, loop points). Round-robin. |
| NOVA-OSC-05 | **Granular:** source is a sample or table. Grain size 10–500 ms, density, scan position and rate, spray (position jitter), pitch spray, window shape and reverse probability. Up to 64 grains per voice. Reuses `grain_dsp.hpp`. |
| NOVA-OSC-06 | **Spectral:** STFT resynthesis of a sample (FFT 1024–8192). Scan position, spectral warps (shift, stretch, smear, freeze, filter), and partial tracking for pitched sources. |
| NOVA-OSC-07 | **Warps:** 2 slots per oscillator, each a mode and an amount. Modes: Off, Bend +, Bend −, Sync, Window Sync, Mirror, Asym, Quantise, PWM, Flip, Remap (with a drawable curve), Fold, FM ← osc, PM ← osc, AM ← osc, RM ← osc. Cross-oscillator modes choose the source oscillator, Sub or Noise. FM and PM are anti-aliased: the oscillator runs at the internal rate, and the modulation index is limited by the bandwidth estimate. |
| NOVA-OSC-08 | **Pitch:** octave ±4, semitone ±24, fine ±100 ct, coarse pitch modulatable in cents. Each oscillator has a phase start and a random-phase amount. |
| NOVA-OSC-09 | **Unison:** 1–16 voices. Detune up to ±1 octave, with a detune **curve**: linear, exponential, or "super", a non-uniform spread close to a classic supersaw. Blend (centre against outer level), width (stereo spread). Stack modes: off, octave, 2 octaves, fifth, fifth + octave, chord (from the clip sequencer's chord). Position spread and warp spread give each voice an offset across the table or warp. Unison voice phases follow the random-phase setting. Changing the voice count is click-free (voices fade in and out over 5 ms). Total gain is normalised by √N, so loudness is constant to within 1 dB as N varies. |
| NOVA-OSC-10 | **Sub:** sine, triangle, saw or square; −2, −1 or 0 octaves; level; direct-out option (bypasses the filters). |
| NOVA-OSC-11 | **Noise:** a sample-based noise library (procedural white, pink, brown, analog hiss, vinyl, breath, plus imports), with pitch (keytracked or fixed), tone, one-shot or loop, and level. |

**Oracles:**
- **Classic:** alias energy below −90 dBFS for a saw at 5 kHz, 48 kHz, HIGH. Pulse duty is accurate within 0.5 %.
- **Wavetable:** the mip-map has no partial above Nyquist at any played note. Frame interpolation is continuous: the largest step is no bigger than the largest difference between adjacent frames.
- **Tuning:** within 0.1 ct at 44.1, 48 and 96 kHz for every type.
- **Unison:** the measured detune spread matches the curve within 1 %. The √N loudness check holds.
- **FM from B:** at index 5 and 2 kHz, aliasing is at or below −80 dB in HIGH.

---

## 4. Filters

| ID | Requirement |
|---|---|
| NOVA-FLT-01 | **Models.** *Analog:* Ladder 24, Ladder 12, Diode, MG Low, OTA (from `vintage_filters`). *Clean:* SVF LP/BP/HP/Notch/Peak at 12 and 24 dB. *Character:* Formant, Vowel morph (from Snarl's formant), Comb +/−, Phaser, Flanger. *Morph:* LP→BP→HP, Dual Peak, Ring. |
| NOVA-FLT-02 | **Controls:** cutoff 8 Hz–22 kHz (exponential), resonance (self-oscillation allowed on the analog models), drive (pre-filter saturation), FAT (model-specific: low-end compensation or second-stage), keytrack −100…+200 %, pan, mix. |
| NOVA-FLT-03 | **Routing:** serial (F1 → F2), parallel, or split (the source grid chooses F1, F2 or Direct per source). Sub has its own direct-out flag. |
| NOVA-FLT-04 | Filters are oversampled with the voice. Cutoff modulation is applied per sample and smoothed so fast LFOs and envelopes are zipper-free. |

**Oracles:**
- The SVF and ladder responses match their analytic curves within 0.5 dB up to 0.4 × fs.
- Self-oscillation pitch tracks the keyboard within 5 ct.
- A cutoff sweep produces no NaN or denormal, and has no step discontinuity larger than the smoothing bound.

---

## 5. Amp and voice

| ID | Requirement |
|---|---|
| NOVA-VCE-01 | Polyphony 1–32, with poly, mono or legato modes. Stealing takes the oldest released voice, then the quietest, with a 3 ms fade. |
| NOVA-VCE-02 | Portamento has a time, a curve, legato-only or always, and a constant-time or constant-rate mode. |
| NOVA-VCE-03 | ENV 1 is hard-wired to amplitude. Velocity sensitivity sets its depth (the default curve is editable). |
| NOVA-VCE-04 | Voices end when ENV 1 is idle and the tail falls below −120 dBFS. |
| NOVA-VCE-05 | Sustain, sostenuto and soft are honoured through the platform's generic pedals (ADR-0035). |

---

## 6. Modulation

| ID | Requirement |
|---|---|
| NOVA-MOD-01 | **Envelopes (4):** attack, hold, decay, sustain, release. Each segment has a curve (−1…+1, drawn by dragging the segment). Optional loop, legato retrigger, and velocity → amount. |
| NOVA-MOD-02 | **LFOs (8): point-and-curve shapes.** Up to 64 points; each segment has a curve (tension) and a type (curve, step, hold). There is a shape library of 40 or more shapes plus user save. The grid has 1–32 divisions with snap. |
| NOVA-MOD-03 | **LFO behaviour.** Modes: TRIG (retriggers per note), ENV (one-shot), FREE (song-locked: the phase derives from the host transport position, so export equals live). Rate is synced (1/64 to 8 bars, dotted and triplet) or in Hz. Also rise, delay, smoothing, phase, bipolar/unipolar, anchor point, and per-voice or global. |
| NOVA-MOD-04 | **Other sources:** velocity, note (keytrack), mod wheel, aftertouch, MPE bend, MPE slide (CC74), MPE pressure, pitch bend, 2 chaos sources (Lorenz and logistic, with rate and smoothing), random (per note), and 8 macros. |
| NOVA-MOD-05 | **Routes.** A route is source × aux source → destination, with an amount (−1…+1 of the destination's full range), a curve, uni or bi polarity, and an output scale. Destinations are any continuous parameter, including FX parameters, other routes' amounts and LFO rates. There are at most 256 routes. |
| NOVA-MOD-06 | **Evaluation.** Per-voice sources (envelopes, per-voice LFOs, velocity, note, MPE, random) modulate per voice. Global sources (global LFOs, macros, wheel, chaos) modulate once per block, per sample where the destination is audio-rate (pitch, cutoff, FM). Modulation is evaluated at the control rate (every 16 samples at the internal rate) and linearly interpolated, except for pitch, FM, PM and cutoff, which are per sample. |
| NOVA-MOD-07 | Every route amount is a flat parameter `nova.mod.<uid>.amount` and can therefore be automated (ADR-0057 §4). Macros are flat parameters. Envelope and LFO knobs are flat parameters. Shapes and curves are document data. |
| NOVA-MOD-08 | Panel telemetry exposes the current values of all sources (the voice under the most recent note for per-voice sources), so rings and dots animate live. |

**Oracles:**
- An LFO's synced phase at any song position equals the analytic value, so seek and loop give identical results.
- A route at 100 % moves its destination across the whole range.
- Route curves are monotonic as documented.
- Chaos is deterministic for a given seed.
- Matrix evaluation of 256 routes stays allocation-free.

---

## 7. FX rack

| ID | Requirement |
|---|---|
| NOVA-FX-01 | **Buses.** Main plus Bus 1 and Bus 2. Each bus is up to 8 serial slots. Buses take a send from Main (pre or post) and return to Main; optional low/high split (crossover frequency) on Main. |
| NOVA-FX-02 | **Modules:** Distortion (12 types: tube, soft, hard, diode, fold, rectify, sine fold, asym, bit crush, downsample, X-shaper with drawable curve, overdrive), Filter (any NOVA-FLT model), Chorus, Flanger, Phaser, Delay (stereo, ping-pong, tape; sync; HP/LP in feedback), Reverb (hall, plate, room, shimmer; Valhalla-class quality from the native effects platform), Compressor (sidechain from the voice sum), Multiband (OTT-style 3-band up and down), EQ (4 parametric bands: bell, shelves, 12/24 dB cuts, notch, band pass), Dimension, Hyper (unison-style doubler), Utility (gain, width, mono below a set frequency), Convolve (an IR as managed media), Shifter (frequency shifter with feedback, spread and ring mode), Flyby (the Flyby effect on a preset path, ADR-0067). |
| NOVA-FX-03 | Slots can be reordered by drag and moved between buses. A module change or reorder crossfades over 20 ms with no click. Each module has a mix knob and a bypass. |
| NOVA-FX-04 | Oversampling: OFF / 2× / 4×, chosen per bus. Distortion and the filter use it; time-based effects don't need to. |
| NOVA-FX-05 | Reuse comes first: the native effect kernels (contour, forge, orbit, relay, expanse, vista, shape, press) and Snarl's rack modules move into a shared `native_dsp/fx_modules/` and are wrapped. Nothing is duplicated. |

**Oracles:** Each module passes its existing kernel's tests. Reordering keeps the signal continuous (largest sample step no more than 1.5× steady state). Bus sums are latency-aligned.

---

## 8. Arpeggiator and clip sequencer

| ID | Requirement |
|---|---|
| NOVA-ARP-01 | **Arpeggiator.** Modes: up, down, up/down, played, random, chord. Rate (synced), gate, swing, octaves (1–4), velocity source (played or pattern), retrigger, latch and repeats. A 16-step pattern sets velocity, gate and tie per step. Sample-accurate against the host transport. |
| NOVA-ARP-02 | **Clip sequencer:** 4 clips of up to 4 bars of notes (pitch, start, length, velocity), each stored in the document. A clip is triggered by holding its note (C1–D♯1 by default), or it plays with the transport. Transpose follows the played key. |
| NOVA-ARP-03 | **Drag to arrangement:** a clip dragged to the arrangement creates a real MIDI clip through existing session commands. No new project concept is introduced. |
| NOVA-ARP-04 | The arpeggiator and sequencer output feed the voice allocator and stay deterministic across block splits and seeks: a seek recomputes the state from the song position. |

---

## 9. Platform changes (the hard part — do this before DSP)

This section describes the built platform. Items marked *later* are open work, tracked in [issues](https://github.com/jcsduncan/oscillate/issues).

### 9.1 Structured synth document
- **Data.** `NativeInstrumentPatch` (`model.hpp`) has a `NativeSynthDocument document`, which is empty for engines without one. It holds:
  - `routes`: `NativeModRoute{slot, source, aux, destination, bipolar, tension, scale}`. The **amount** is the engine's flat parameter for `slot` (0–255), so it can be automated; `scale` (0..1) is the fixed output level.
  - `curves`: `NativeCurve{id, points}`. Each point is `{x, y, tension, segment: curve | step}`. The first point is at x = 0 and the last at x = 1, with x strictly increasing and y in 0..1.
  - `chains`: `NativeChain{id, modules}`, where each slot is `NativeChainSlot{uid, module}`. The uid names the slot's flat parameters, so reordering moves a module together with its settings and automation. Uids are unique across all chains.
  - `sequences`: `NativeSequence{id, length, notes}`, with notes as `{start, length, key, velocity}` in ticks.
  - `media`: `NativeMediaSlot{id, NativeBuiltinMedia{name} | ManagedAudioReference, frame_length}`.
  - `labels`: `NativeLabel{id, text}`, such as macro names.
- **Validation is split.**
  - *The model* checks structure, limits and media identity. Limits: 256 routes, 64 curves × 64 points, 4 chains × 8 slots, 8 sequences × 512 notes, 32 media slots, 32 labels of up to 64 bytes. A managed slot must match a managed source record exactly, so that source cannot be removed while the slot uses it.
  - *The engine* checks meaning through `SynthEngine::validate_document`. This runs inside `resolve_native_instrument`, so every command and every prepared source checks it.
- **Engine hooks** (`runtime.hpp`): `validate_document`, `prepare_document(document, media, rate)` (which compiles the document into an immutable `SynthDocumentState`), `preset_document(optional preset)`, and `SynthKernel::adopt_document`. Engines without these accept only an empty document.
- **Codec.** The patch payload carries a `"document"` object; every field is required. `ConvolutionIrReference` is now the shared `ManagedAudioReference`.
- **Session.** `native_synth_document(track)` and `set_native_synth_document(track, document, gesture)`: the whole document is replaced as one undo step, and steps with the same gesture coalesce. Value edits keep the document. *Later:* add route, curve and slot helpers in the panel layer, as plain document transforms.

### 9.2 Parameter limit
- The limit is now **1024** (`max_engine_parameters`, `max_native_instrument_parameters`, and the codec checks that use them). The control mailbox was already heap-allocated, and it never allocates on the callback. `max_telemetry_values = 2048` is unchanged.
- **Nova's table:** about 230 core parameters, plus 256 route-amount slots, plus FX slot parameters keyed by slot uid (`fx.<uid>.p0..p7`). That is about 700 in total.

### 9.3 Managed media for synths
- **No new encodings.** Tables, samples and noises are ordinary managed WAV/FLAC/AIFF sources. A media slot's `frame_length` says how the audio is cut into wavetable frames (0 means not cut). A multisample is several slots, plus a zone map in the document (*later*, phase 5).
- **Built-in content** is `NativeBuiltinMedia{name}`. The engine generates or ships it itself and it is never copied into projects.

### 9.4 Media path into kernels
- `source_prep::load_synth_media(store, snapshot, document, stop)` decodes every managed slot from verified sources, sharing one asset between slots that name the same source. It returns `SynthMedia` (slot, sha256, `shared_ptr<const SynthMediaAsset>`).
- `NativeSynthPlayer::prepare(..., media)` and `NativeSynthRuntime::create(engine, values, document, media, rate)` compile the document with its media before the first block.
- **Callers.** Routed playback and offline export, with or without VST3 stages, load the media; nothing is decoded on the callback.
- *Later:* a missing asset should fall back to a sine and raise a problem toast. Today, preparation fails with the reason.

### 9.5 Live edits
- **The live path.** `NativeSynthRuntime::offer_document(document, revision)` compiles on control and offers the result through a lock-free mailbox, *before* the values of the same revision. The callback adopts values first, then the document, at one block start, so `applied_revision()` acknowledges both. Compiled documents are retired on control once the callback has moved past them.
- **Routing.** `RoutedPlaybackControl::try_queue_live_native` offers a changed document. If the edit changes the managed media, `offer_document` declines and the source is rebuilt.
- *Later:* a gap-free media swap (decode on a worker, then swap with a 10 ms crossfade). Oscillator-type crossfades are the engine's own job.

### 9.6 Panel adapter
- `NativeSynthPanelAdapter` has `document()` and `submit_document(document, gesture)`. In the session adapter, a document drag is a series of edits under one gesture: it is heard as it moves, undoes as one step, and is written to disk when it ends. Cancel restores the document from before the drag. The panel tests' in-memory adapter (tests/ui/quick) records document submissions.
- *Later* (phase 3): the bridge exposes the document to QML (routes as a `KeyedListModel`, curves as point arrays, chains). Wavetable thumbnails come from decoded assets.

### 9.7 Schema
- The schema is **1.40**, with no migration. Projects saved at 1.39 do not open.
- *Phase 2:* remove Lumen and Snarl from every fixture.

**Acceptance:**
- *Done:* a codec round-trip of a full document, with malformed documents rejected (`native_instrument_schema_contract`).
- *Done:* model structure, limits and media identity, plus undo (`native_instrument_model_contract`).
- *Done:* session get/set, with an engine without a document rejecting one (`application_native_instrument_session`).
- *Done:* runtime adoption with its revision, retries, retirement, media-change decline, and engines without documents (`native_synth_document_contract`).
- *Phase 2, with Nova:* export equals live for a patch that uses a custom table, a sample, a drawn LFO and an FX reorder; the missing-asset fallback.

---

## 10. Telemetry

Per instance, published at about 60 Hz:
- each oscillator's current frame position (modulated), voice count and peak;
- each filter's modulated cutoff and resonance;
- every source's current value (the newest voice for per-voice sources);
- the LFO and envelope phases;
- voices in use and CPU;
- per-bus meters and the clip sequencer playhead.

Everything goes through the existing telemetry vector, which has spare capacity.

---

## 11. Removal of Lumen and Snarl

Do this in one commit, after the Nova engine is registered so that eight engines still exist.

- **Engine sources and registration:**
  - `src/native_dsp/synth/lumen*`, `snarl*` (but **move** `snarl_dsp` rack modules, formant and vowel code and growl-table generators into shared modules first);
  - `engines.cpp` (array size) and `src/native_dsp/CMakeLists.txt`.
- **App:**
  - `project_bridge_browser.cpp` (EngineLook entries) and `device_strip_catalog.cpp`;
  - `NativeSynthEditorOverlay.qml`, `qml.qrc`, the Lumen and Snarl QML dirs, and `native_synth_visuals.cpp` (LumenVisuals).
- **Tests:**
  - the Quick and native_dsp CMake lists, and `cmake/TestBudgets.cmake`;
  - index-based registry tests (pulse, halo, torque and drift indices shift);
  - tests that use Lumen as a convenient renderer: lens_panel_tests and loudness_panel_tests. Switch them to Prism.
- **Hidden dependencies:**
  - Drift references Lumen's table names (`drift_table.hpp`, `drift_presets.hpp`); give Drift its own copy of the tables it uses, or point it at the shared table module;
  - `tools/demo_song/main.cpp`.
- **Docs:**
  - mark ADR-0044 and ADR-0056 as superseded;
  - remove the Lumen and Snarl performance notes;
  - update the PRD §13 list, the HLD engine list and the implementation plan.

**Acceptance:** `grep -ri "lumen\|snarl"` over src, apps, tests, tools and cmake returns nothing. All native contracts pass for eight engines.

---

## 12. Content and licences

| Source | Licence | Use |
|---|---|---|
| **Procedural tables** (in-house generators: additive series, formant/vowel sweeps, FM ratios, sync sweeps, PWM, fold and bend families, resampled growls from Snarl's generators, noise-band tables, spectral pads) | Ours | The core factory set: 80 or more tables, generated at build time into `share/`, deterministic. |
| **AKWF** (Adventure Kid Waveforms, about 4,300 single cycles) | CC0 public-domain dedication by Kristoffer Ekstrand (stated on adventurekid.se). Credit is appreciated, so add one to the credits. | Imported as single-cycle frames; curated families morphed into about 40 tables. |
| **Noise library** | Generated procedurally, plus our own recordings if any | Noise oscillator. |
| **User import** | The user's own | WAV/FLAC/AIFF; Serum-compatible `clm ` frame-size chunk; folder of single cycles; any audio → resynthesis. |

- **Never bundled:** Serum's factory content, or any pack without an explicit redistribution licence. Most free "Serum wavetable packs" do not grant redistribution.
- **Licence record.** Record each bundled set's licence in `docs/dependencies/`, following the HLD dependency review.

**Factory presets:** 150 or more presets across Bass, Lead, Pad, Pluck, Keys, Arp/Seq, FX, Growl, Reese, Supersaw, Chord and Init. Each has named macros wired to musical controls. They are built in code like the other engines' presets, and checked by ear with rendered audition clips sent to the user before sign-off.

---

## 13. Delivery phases

Each phase is committed on main, tested, and leaves the app working.

1. **Platform** (§9): the document, the 1024-parameter limit, synth media, the kernel resources path, schema 1.40, the panel adapter. No sound change.
2. **Nova core:** Classic and Wavetable oscillators with unison and warps, sub and noise, 2 filters and routing, 4 envelopes and 8 LFOs, macros, the matrix, the amp. A built-in table set. Engine registered. **Remove Lumen and Snarl** (§11).
3. **Panel** (§2), OSC and MATRIX pages first.
4. **FX rack** and the FX page.
5. **Sample, Multisample, Granular and Spectral** oscillators, plus media import.
6. **Wavetable editor.**
7. **Arpeggiator and clip sequencer.**
8. **Content and presets:** AKWF import, procedural sets, 150 presets, audition renders for listening review.
9. **Performance pass:** ECO/HIGH/ULTRA measurements and the performance doc.

**Build discipline ([AGENTS.md](../../AGENTS.md)):** one targeted build at a time with `scripts/build.sh build/quick --target …`; never run two builds in parallel.

## 14. Test inventory

| Test target | Covers |
|---|---|
| `nova_dsp_tests` | NOVA-OSC, NOVA-FLT and NOVA-MOD oracles (§3–6) |
| `nova_engine_tests` | voice allocation, pedals, determinism, block-split invariance, tail, no allocation, goldens |
| `nova_fx_tests` | NOVA-FX (§7) |
| `nova_arp_tests` | NOVA-ARP (§8) |
| `native_synth_document_tests` | §9.1–9.5 (model, codec, session, live edit) |
| `native_synth_contract_tests` | updated to eight engines, plus export parity |
| `nova_panel_tests` (Quick) | NOVA-UI |

Each new test needs an entry in `cmake/TestBudgets.cmake`.
