# ADR-0081 — Clay: note editing of audio

**Status:** Accepted; implemented (DSP core, the take in the project, playback, export, session commands and the Notes tab). **Date:** 1 November 2026.
**Builds on:** ADR-0052 (tempo and key analysis), the warp engine's FFT, windows and transient detector (`src/warp_dsp`), ADR-0079 (grooves, for quantising to a feel).
**Panel design:** `docs/ui/clay-mockup.html` (phase 3).
**Contract:** `include/oscillate/clay_dsp/{take,analysis,render,macros,limits}.hpp`; phase 2: `project::ClayTake` and `SetClipClay` (`include/oscillate/project/model.hpp`, schema 1.45), `include/oscillate/processing/clay_source.hpp`, `include/oscillate/application/clay_edit.hpp`, `ProjectSession::set_clip_clay` / `add_clay_midi_clip`, and the Qt `ClayController` (`ProjectBridge.clay`).

## Context

Requirement: Melodyne-class note editing of audio: open an audio clip and see its notes as blobs, then move their pitch, timing, vibrato, drift, formants and level. This covers sung lines and basslines, and also chords in a sampled stab, where one note is shifted and leave the others alone. It runs inside the DAW, so the result plays and exports like any other clip, and the notes can drive our own synths. The hard part is the signal model. Every edit has to sound clean, and audio nobody touched has to stay exactly as it was.

## Decision

### 1. One model for every mode: notes of partials, plus the rest

Analysing a clip gives a **take**: a list of **notes**.
- **Sinusoidal notes** (melodic and polyphonic modes) are partial tracks. Each analysis hop of every partial has a frequency, an amplitude and a phase at the hop's centre.
- **Each note also carries:**
  - a pitch curve (Hz per hop) and an amplitude curve;
  - a **centre**: the amplitude-weighted *median* of its pitch in cents, so the half of a slide inside the note doesn't drag it;
  - **drift**: the pitch averaged over 200 ms (the first null of that average is at 5 Hz), minus the centre;
  - **modulation**: the pitch minus that average, so vibrato;
  - a **spectral envelope** every fourth hop, as natural-log amplitudes on 240 log-spaced bands from 40 Hz to 20 kHz. It runs through the note's own partial levels, linear in log amplitude over log frequency;
  - a **noise share**: how much of its energy the partials don't explain;
  - a **gain per channel**, its pan.
- **Segment notes** (percussive mode) are spans of the original audio.

**What is not a note is the residual:** the original minus every note's resynthesis.

**Rendering.** Output = original − Σ(edited notes as analysed) + Σ(edited notes as edited). This equals residual + Σ(resynthesised notes), but it only computes what changed. So:
- **an unedited note takes the direct path.** It is never resynthesised; its original samples are copied. With no edits the output is bit-for-bit the input. An identity edit is skipped, and so is everything further than a fade and a grain from an edited note. The oracle tests check all three.
- **a resynthesis error stays local.** It only ever lands on notes someone edited.

Each removal and addition uses the same crossfade, a raised cosine 8 ms long, centred on the note's edges. Two notes that meet cross-fade with gains that sum to one, so taking one out leaves exactly the other's share of the boundary.

### 2. Detection

**Melodic (monophonic): probabilistic YIN (Mauch & Dixon 2014).**
- **Frames:** each hop (5.3 ms at 48 kHz) computes the YIN difference function by FFT correlation over a 21 ms integration window centred on the hop. If the window starts at the hop instead, the pitch lags it by half a window, and slide cuts land about 13 ms late. It then takes every local minimum of the cumulative mean normalised difference, refined by a parabola.
- **Candidates:** they get probabilities from 100 thresholds weighted by a Beta(2, 11.33) prior (mean 0.15). The first minimum under each threshold takes that threshold's weight.
- **Path:** a Viterbi over 20-cent pitch bins from 55 Hz to 1,760 Hz, each voiced or unvoiced. Pitch moves by at most 13 bins per hop with triangular weights, and the voicing stays the same with probability 0.99. Frames 60 dB under the loudest are forced unvoiced.
- **The path snaps to the nearest candidate's exact pitch.**
- **Notes:**
  - **Runs:** voiced runs, bridging gaps of 15 ms or less.
  - **Pitch splits:** a run is split where the 200 ms-smoothed pitch leaves its level by more than 70 cents for 60 ms. The cut falls where the smoothed pitch crosses halfway to the new level, so the middle of a slide.
  - **Re-articulations:** a run is also split where the level rises 9 dB out of a dip.
  - **Edges:** a run's outer edges are refined on 1 ms RMS blocks. A note starts where the level first reaches its median −12 dB, and ends where it last holds above its median −24 dB.
- **Partials:**
  - Each hop is a zero-phase Hann window holding four periods of the note's lowest pitch, up to 8,192 samples.
  - Up to 48 harmonics, each taken from the strongest local maximum within ±0.4 f0 of h·f0.
  - The reassigned frequency comes from the derivative window (Auger & Flandrin). The amplitude is corrected by the window's tabulated main lobe at the reassigned offset, and the phase is taken at the window's centre.
- **Envelope:** interpolated through the partials' measured levels, linear in log amplitude over log frequency.
  - **Why not the true envelope:** on the sung-vowel oracle this lands within 1 dB of the resonators between harmonics. The **true envelope** (Röbel & Rodet 2005) misses by up to 3.6 dB there, and at order fs/(2·f0) it ripples at 2·f0 and puts F1 on a single harmonic.
  - **Where the true envelope is still used:** as the measuring instrument (`spectral_envelope` in the Clay DSP tests). It is cepstral smoothing at order fs/(3·f0), iterated until it lies within 2 dB under every bin (at most 64 times).

**Polyphonic: iterative estimation and cancellation (Klapuri 2006).**
- **Spectra:** two STFTs (4,096 and 8,192 samples at 48 kHz, 512-sample hops) with reassigned peaks. Pitches under 200 Hz use the long window.
- **Each hop:**
  - **Salience:** the salience of every 20-cent candidate is a harmonic sum of square-root-whitened peak amplitudes, weighted (f0 + 27)/(h·f0 + 320). Partials match within 35 cents, or 0.6 of a bin, whichever is wider.
  - **Picking:** the best candidate is refined from its first eight harmonics and recorded. Its harmonics are then cancelled from both spectra.
  - **Spectral smoothness:** a partial standing more than 1.5× above its neighbours is shared, so only the neighbours' level is taken from it.
  - **Stopping:** at eight notes, or when the best salience left falls under 0.3 of the hop's first.
- **Tracking:** estimates join a track within 50 cents. A track ends after a 35 ms gap. Tracks shorter than the minimum note length are dropped.
- **Partials:** each note claims the peak *nearest* each harmonic, never a louder neighbour.
  - **Shared partials:** two claims are one partial when their reassigned frequencies agree within 0.6 of a short-window bin, whichever window each came from. A low note reads the long window and a high one the short, so a note an octave under another sees the other's partials in a different list.
  - **Splitting:** a shared partial is **split in proportion to what each note's own unshared neighbouring harmonics lead us to expect**, or equally when neither note has unshared neighbours.
- **Ghosts:**
  - a note with a tenth or less of the salience of a note sounding with it is dropped;
  - so is a note whose partials sit 80 % or more on other notes' peaks (an octave or twelfth made of another note's harmonics);
  - partials are then assigned again without them.
- **Envelope:** a note's envelope is interpolated from its own partial amplitudes, because in a mixture the frame's envelope belongs to no single note.

**Percussive.** A note per transient, using the warp engine's detector. It runs to the next transient or until it falls 50 dB under its peak. It is pitched at the strongest reassigned peak in the first 50 ms, has no partials, and plays back as a segment.

### 3. Edits

A `NoteEdit` is a plain struct, and the model will persist it as it is:
- `pitch_cents`, added to the centre;
- `modulation` and `drift` scales;
- `formant_cents`. At 0 the envelope stays where it is ("preserve"); otherwise the envelope moves by that many cents, independent of pitch;
- `gain_db`;
- an optional new `start` and `end`, between 1/8× and 8× the note's length;
- `duplicate`, which plays a copy of the note with its own edit; the original still plays. This is what Harmonise uses.

`split_note` and `join_notes` change the take itself. The two halves of a split both keep the hop at the split, and envelopes are resampled. A split take still renders bit-identically.

### 4. Resynthesis

- **The edited note.** For each output hop (on the take's hop size, laid over the new placement), we find the fractional source hop.
  - **Pitch:** the new pitch is centre + pitch + drift × scale + modulation × scale. Every partial's frequency is multiplied by the ratio of that to the original pitch at that moment, so inharmonicity and jitter come along.
  - **Level:** each partial's amplitude is multiplied by the gain and by exp(E(f′ · 2^(−formant/1200)) − E(f)). E is the note's envelope at that hop, and the correction is capped at +3 nepers.
  - **Ceiling:** partials at or above 0.45·fs are silent.
- **Phases.** A note whose pitch and timing are untouched keeps its measured phases, so a gain-only or formant-only edit stays phase-exact. Otherwise the phases are integrated from the new frequencies, starting from the note's first measured phase.
- **The bank.** Between hops, each partial follows the McAulay–Quatieri **cubic phase** through the phases and frequencies at both ends, with linear amplitude. Before the first hop and after the last, it holds its edge values under the edge fades.
- **Determinism.** Every sample is a closed-form function of its segment, so **any block split renders the same samples**. Voices are summed in a fixed order.
- **Residuals that move with a note** (melodic mode, a note given a new placement). The note's residual over its span (the original minus every note's partials there) is taken out at its old place and overlap-added at its new place. It is stretched by Hann grains of 24 ms at 50 % overlap, on a fixed grid from the new start. Breaths and consonants travel with the note.
  - **Why not Signalsmith Stretch:** the adapter was a worker-only experiment (since removed) that wasn't linked into the DAW, worked on whole segments, and couldn't be cancelled.
  - **Polyphonic residuals stay put,** because they are shared by every note sounding there.
- **Percussive notes** play back through the same grain mover. A hit is shifted in pitch by reading each grain faster or slower, which keeps its time. With no change, the grain mover reproduces the source.

All rendering is offline (`Renderer::create` plans, `render(first, …)` fills any range) and never runs on the audio thread. Playback reads it through a render cache (phase 4).

### 5. Macros

These are pure functions from (take, edits) to edits. An empty selection means every note.
- `correct_pitch`: centre amount and drift amount, to a `Scale`.
- `quantize_time`: to a `TimeGrid` of steps, with per-step offsets so a `GrooveTemplate` maps straight onto it; keeps the length.
- `level_amplitude`: towards the selection's median level.
- `to_midi`: notes with velocities (linear over 40 dB under the loudest), plus one pitch-bend lane every 10 ms and the range it needs. The bend lane is meaningful for melodic takes.
- `harmonise`: duplicates a number of scale degrees away, placed on the scale.
- `fit_to_key`: degree by degree. With `follow_quality`, a minor third becomes a major third when the modes differ.
- `follow_notes`: to the MIDI note each note overlaps most, in pitch and, by an amount, in time.
- `variation`: seeded with splitmix64. At most ±15 cents, ±12 ms and ±2 dB.

### 6. Oracles

All of them are in `tests/clay_dsp`, one CTest case each, on synthetic signals.
- **Melodic analysis:**
  - **Signal:** a sung /a/ phrase: a band-limited glottal source through three formant resonators, with vibrato, a legato slide and gaps.
  - **Checks:** four notes with onsets within 10 ms (the slide's at its midpoint) and centres within 5 cents; the vibrato's depth is measured; the voice is mostly partials.
- **Polyphonic analysis:** an Am9 voicing (A2 G3 C4 B4) as harmonic tones, and again with string inharmonicity B = 4·10⁻⁵. Every note is found within 30 cents, and nothing else louder than −24 dB.
- **Null:** no edits, or an identity edit, renders the input bit-for-bit, and audio before an edited note is untouched.
- **Melodic shift:** a sung note +300 cents re-analyses at +300 ± 5.
- **Polyphonic shift:** shifting C4 +300 cents in the chord leaves the other notes' first six partials (away from the moved note's) within 1.5 dB. The old C4 falls 20 dB or more, and the E-flat is found within 5 cents.
- **Formants, checked exactly:**
  - **The measuring instrument:** the true envelope of the vowel finds its three formants within 10 %.
  - **Envelope kept:** after +5 semitones, every harmonic up to 3.5 kHz is within 2 dB of the source-filter's exact response at its new frequency.
  - **Envelope moved +5 semitones as well:** every harmonic is within 2 dB of the original's same harmonic.
  - **Why not peak positions:** an earlier version read the formant peaks off the output's envelope. A peak read off a harmonic spectrum lands only within about f0/2 of the resonance, which is 14 % for F1 at f0 = 208 Hz, so that check could not be met by any renderer.
- **Time:** a 1.5× stretch re-analyses within 15 ms of its new length, with its pitch within 5 cents.
- **Determinism:** random block splits are bit-identical, and analysing twice gives the same take.
- **Percussive:** four hits are found within 10 ms at their pitch. A −6 dB edit measures −6 ± 1 dB, and the next hit is untouched.
- **Take editing:** split and join round-trip, and a split take still renders the original.
- **Macros:** correct, quantise, level, variation, to-MIDI, harmonise, fit-to-key and follow.

## Limits

- **Polyphonic quality has known caveats:**
  - Shared partials are split by expectation, not separated. Notes in octaves, or with many shared partials, will bleed into each other when one is moved.
  - Long windows smear onsets, so a moved note's attack may leave a little pre-echo, and polyphonic note starts are only accurate to a hop or two.
  - Dense mixes (full tracks) are out of scope; stems and stabs are the target.
- **A melodic slide is cut at its midpoint.** Re-pitching one side of a slide moves only that half.
- **Percussive pitch shifts** use 24 ms grains, so transients soften.
- **The capped envelope correction** limits extreme formant moves to about 26 dB of rise per partial.

## Performance targets

- **Analysis faster than real time** on a 4-minute stem at 48 kHz, on one worker thread. The melodic Viterbi is about 32 k operations per hop, and the backpointers take about 54 MB for 4 minutes. The polyphonic estimator is about 50 k operations per hop and iteration.
- **Rendering is cached**, and only edited notes cost anything.
- **Memory:** a melodic note keeps 48 partials × 12 bytes per hop, plus an envelope row every fourth hop.

Both targets go into a benchmark in phase 2.

## Phase 2: Clay in the project, playback and export

1. **The take is part of the clip (schema 1.45).** `Clip::clay` is null or a `ClayTake`: the detection mode, the `analysis_version` the note ids refer to, the splits and joins (`ClayStructuralOp`, replayed in order on the analysed take, so the ids `split_note` mints are the same every time), the per-note edits and the key snapping and the key macros work in. An edit is `ClayNoteEdit{note, voice, pitch, modulation, drift, formant, gain, muted, placement}`; voice 0 is the note's own edit and 1.. are its harmonies (`NoteEdit::duplicate`, numbered in the order the DSP lists them). Mute renders at `min_edit_gain_db`. The bounds are `clay_dsp/limits.hpp`, header-only, shared by the DSP and the model, which checks structure and ranges only: whether ids exist is checked when the take renders. A take made by another `analysis_version` is refused; there is no migration.
2. **One command per gesture.** `SetClipClay{clip, take}` replaces the whole take and bumps the clip's settings generation, so a drag (previewed in the controller), a macro (previewed while its slider moves), a split, Harmonise or Fit to key is exactly one undo step. To MIDI is one step too: `add_clay_midi_clip` adds the MIDI clip (and a Nova track when none is chosen) with the bends as clip automation of the engine's `<engine>.perf.bend`, raising the engine's "Bend Range" parameter when the bends need more.
3. **The analysis is not saved.** It is cached in the project's disposable cache (`CacheStore`, a source-analysis entry keyed by the source, `analysis_version` and the mode) and recomputed when missing; analysis is deterministic, so the same notes and ids come back.
4. **A Clay clip plays a render of its source.** `processing::clay::rendered_record(source, take)` names the render: the source's rate, channels and frames, a float WAV whose `sha256` field is the render's cache key (the source, `render_version` and the take's canonical edits; the key and presentation are not in it, so copies of a clip with one take share one render). `open_rendered` verifies the source, then opens `cache/clay/<key>.wav`, or decodes, analyses (cached), renders and publishes it atomically when it is missing or damaged. Every path that reads a clip's audio swaps the source for the render:
   - routed live playback (`RoutedAudioSource`) for an unprocessed clip;
   - the warp, live and export: a `WarpRenderRequest` for a Clay clip carries `clay = {original source, take}` and the render's record as its `source`, so warp cache and analysis keys follow the edit, and `open_request_view` opens the render;
   - master and track export (`offline_master`'s sources are keyed by the audio actually read).

   Clay edits are therefore before warp: the clip's warp, fades and level apply to the render exactly as they did to the source, and export plays what playback plays (tested: a track render equals the Clay render to 1e-6, and an edit is heard through a pitch-shifting warp). The VST3 export paths and the legacy direct playback path refuse Clay clips rather than play the source.
5. **Nothing silently plays the original.** A new graph is prepared on a worker, and preparation renders a missing take there (as it decodes and verifies sources); live structural switching keeps the previous graph sounding until the new one is ready (ADR-0038, ADR-0049). To keep that short, the controller warms the render on a worker after every commit (`renderState`: "rendering", then "ready"); a newer commit supersedes it.
6. **Song-side helpers** (`application::clay`): `ClipTime` maps the clip's source frames to song frames, seconds and ticks through its warp map; `quantize_grid` builds the grid (or a groove pool template's feel) in source frames; `follow_targets` reads an instrument track's MIDI notes under the clip, looping clips included; `midi_clip_out` turns `clay_dsp::to_midi` into a clip-local MIDI clip and bend points.
7. **The controller** (`quick::ClayController`, `ProjectBridge.clay`) holds one clip: detection on a worker with progress, the notes as drawn (keys `"<id>:<voice>"`, song seconds, cents, decimated pitch and level curves), the waveform, selection, begin/update/commit drags for each tool, structural edits, macros with preview, the key, the creative tools, auditions written as WAVs for the browser's preview player (edited, or the original with A/B), the editor's history, and undo/redo through the bridge.

**Not yet:** the Notes tab QML (phase 3); a transport-level A/B (auditions have one); the arrangement's waveform still shows the source, not the render; renders and Clay analyses sit beside, not inside, the cache budget (`cache/clay/` is not evicted by `enforce_budget`).

## Later phases

1. **Done:** the DSP core and oracles.
2. **Done (phase 2 above). Model and persistence:**
   - **Edits** become part of an audio clip, with a schema bump: the analysis mode and options, the list of `NoteEdit`s, and splits and joins, recorded as operations so the take can be rebuilt.
   - **The analysis** is cached by source hash and options, beside the waveform and key caches (`source_analysis_cache_directory`).
   - **Session commands** edit notes as one undo step each.
3. **The Notes tab UI:** from `docs/ui/clay-mockup.html`. Blobs over pitch lines and the scale, the pitch, time, formant and level tools, the macros, Harmonise, and to-MIDI and follow-MIDI with our synths.
4. **Done (phase 2 above), differently:** the render is a source-shaped WAV that the warp reads in the source's place, rather than an LDS2 warp-cache entry, so Clay and warp compose.

## Addendum (1 November 2026): polyphonic detection on real material

On the Notes tab's synthetic Am7 stab, polyphonic detection gave eight notes instead of four, and every note showed a noise share near 100 %. These were the causes and fixes:

- **Duplicate estimates.** Cancelling a note compared each partial with its neighbours' *remaining* weights. Neighbours another note had already taken looked small, so most of the note's own fundamental was left in place, and the note was found again in the same hop. Duplicates then shared all of the real note's partials and pruned it.
  - Cancellation now judges against the neighbours' original weights.
  - A hop gives at most one estimate per pitch.
- **Ghosts made of other notes' partials.** A pitch candidate now needs its own fundamental, within −40 dB of its strongest partial.
  - **Partials split between notes** are explained asymmetrically, in alternating rounds, so a ghost can never prune the real note under it.
    - A lower note explains a partial it has as a higher harmonic, unless the partial stands 3× above that note's neighbouring partials (a real doubling).
    - A higher note explains a partial it has as a lower harmonic (subharmonics).
    - A partial more than 20 cents off a note's harmonic belongs to the note whose harmonic it sits nearer.
  - **Overlapping tracks within 50 cents** are merged.
  - **Tails:** notes 26 dB or more under a note sounding with them, or one that stopped within 0.5 s before, are dropped (dampers, resonance, release tails).
- **Broken notes.** A note the picker skips keeps going while its own partials hold 15 % of the hop's strongest salience and 10 % of its own best. Tracks bridge gaps of up to 80 ms.
- **Noise share** is the residual (the clip minus *every* note's partials) against the clip over the note's span. Before, it was the note against the whole mixture, which counted the other notes as noise.

**New oracles:**
- `clay_dsp.polyphonic_finds_a_struck_stab` (the Notes tab fixture): exactly four unbroken notes, each with a noise share under 0.2.
- `clay_dsp.piano.*`: ten chords played on Concert (Salamander): Am7, Am9 and C in close and open voicings, held and staccato. Each case is skipped, with a message, when the library is missing.

**Superseded the same day by joint estimation (below).** At this point six of the ten piano oracles still failed. The shapes:
- **Weak fundamentals:** a G3 whose fundamental is 30 dB under its second partial loses to the D5 and G4 hypotheses its partials also fit.
- **A real B4 drops out after 0.7 s:** its fundamental falls out of the peak list while its string pair beats.
- **Staccato G4 (close voicing):** lost to its subharmonic.

These need a joint (rather than greedy) choice of note hypotheses.


## Addendum (1 November 2026): joint note sets

**What replaced the greedy per-note pruning:** the polyphonic analysis now chooses note sets jointly.

1. **Per hop** (`NoteSets`):
   - **Score of a note set:**
     - the energy of peaks no chosen note covers;
     - each note's missing partials, each expected at the mean of its neighbours' levels. The first partial is expected at a quarter of the second's level, or half when it has no peak at all;
     - a cost per note, 4 % of the frame's energy.
   - **Free levels:** partial levels are free, so a weak fundamental costs little, and one note that explains another hypothesis's peaks wins by needing fewer notes.
   - **Stretched series:** partials are matched along f_h = h·f0·(1 + s·h²), with s fitted as the series is walked, so stiff strings fit.
   - **Search:** local search (add, remove, swap, and *absorb*: one note in place of every note whose peaks it covers 80 % of). It starts from the iterative estimator's picks, over those picks and their subharmonics.
2. **Segments between onsets** (the warp transient detector) take their notes by a vote. The vote runs over 0.1 s once the long window has passed the attack, and needs half of those hops.
   - **Stopping:** notes the vote leaves out stop at the onset.
   - **Continuing:** a note carries on while its partial series covers more than 0.3 % of its best energy. It stops after 30 ms under that.
   - **Soft entries:** a note entering without a transient must be chosen hop after hop for 0.1 s and must add 5 % of the frame that no sounding note's series covers. A subharmonic of sounding notes adds none.
3. **Partials** are assigned as before, and tails (−26 dB) are dropped.

**Results:**
- All 38 Clay tests pass: the synthetic oracles, the stab, all ten Concert chords (held and staccato), and the application and Quick suites.
- **Cost:** a 30-second piano take (a chord every 2 s) analyses in 5.5 s on one thread (5.4× real time, release build, with the machine in use). To measure it, run `oscillate_clay_piano_tests timing`, which is not a CTest case.
