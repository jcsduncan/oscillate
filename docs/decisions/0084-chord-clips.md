# ADR-0084 — Chords are notes in MIDI clips

**Status:** Accepted; implemented. **Date:** 1 November 2026.

**Date:** 1 November 2026.

**Supersedes in part:** ADR-0083. The Chord track (`Snapshot::chords`, `SetChordTrack`, schema 1.46) and Comp's Chord-track mode are gone. The harmony library, the Progression Lab and Comp's rhythm engine stay.

**Design:** [ui/chord-clips-mockup.html](../ui/chord-clips-mockup.html).

**Contract:**
- `include/oscillate/harmony/harmony.hpp`: `ChordQuality`, `ChordSymbol`, `ChordDevice` and `ChordRegion` now live here, as the harmony library's working form of chords. They are no longer project data.
- `include/oscillate/harmony/clip.hpp`: recognising chords in a clip's notes and rewriting the notes for a harmony edit.
- `include/oscillate/native_dsp/note_effects/note_effect.hpp`: `AheadEvent`, `note_lookahead_ticks`, `copy_ahead` and `NoteParameters::ahead()`.
- `include/oscillate/application/comp_patterns.hpp`: `render_notes(parameters, held, tempo, sample_rate, clip_start, start, end)`.
- `include/oscillate/application/project_session.hpp`: `comp_rhythm_notes`, `apply_comp_rhythm`, and a `gesture` on `replace_midi_clip_notes`.

## Context

In use the Chord track proved disjointed:
- the chords and the part that played them lived in different places;
- the chords could not be seen or edited as notes in a MIDI clip.

On the Chord track, the chords lived on a lane of their own, and Comp played them on another track. The notes that sounded were nowhere to be seen or edited.

## Decision

1. **Chords are ordinary MIDI notes in MIDI clips.** There is no song-level chord lane.
   - A chord strip over the clip recognises the chords in its notes (`harmony/clip`) and names them.
   - Every harmony edit (a Lab suggestion, a substitution, a device, a voicing) rewrites the clip's notes. A mood-pad drag that rewrites them many times passes one gesture to `replace_midi_clip_notes`, so the whole drag is one undo step.
2. **Schema 1.47 drops `"chords"`.** The codec neither writes nor reads it. There is no migration in the DAW (greenfield). Existing projects are converted once, outside the DAW, by a script that also removes Comp's dropped parameters.
3. **Comp follows the held notes only.**
   - `comp.follow`, `comp.voicing`, `comp.top_line` and `comp.register` are gone: the held notes are the voicing. `comp.octave` stays.
   - The low lane plays the lowest held note (the bass), the top lane the highest, and the middle lane the notes between. Two notes are a bass and a top; one note is the bass alone.
   - Held notes are Comp's input and do not sound themselves. A note-off for a note Comp does not hold passes through.
4. **Pushes see the next chord coming.** The player hands every note effect the track's own notes for a dotted 8th (Comp's longest push) past each block: `copy_ahead` reads them from the compiled MIDI plan, allocation-free, into a buffer the player allocates once.
   - Each event carries the tick of the frame before it. A note joins the first 16th step whose tick passes that tick, exactly as it would when it arrives as input in its own block, so any block split gives the same notes.
   - A change is a step where new notes arrive and the held keys differ afterwards. A chord struck again is not a change.
   - A change at step *C* is pushed when the hash of (seed, *C*'s tick) is below the push chance. From *C* − push, Comp plays the chord as it will be held at *C*, adds a hit at *C* − push and drops the hit at *C*.
   - The hit at *C* is dropped only if the push actually sounded. Notes played live are not in the plan, so their changes are never pushed and never dropped.
   - **Caveat:** the player does not know where a transport loop wraps, so near a loop end the lookahead sees the notes after the loop end, not those at the loop start.
5. **Rhythm Apply is offline Comp.** `apply_comp_rhythm(clip, parameters, held, start, end)` runs the same kernel, clock and lookahead over `held`, normally the chords the caller wrote, from the clip's place in the song.
   - The notes of the clip starting in [start, end) are replaced by Comp's output. Notes starting outside the range stay, and a note held across `start` is cut there. The result is one undo step.
   - `comp_rhythm_notes` returns the same notes without applying them, for previews.
   - The output equals what a Comp on the track plays live over the same notes, played from where the range's notes begin. A push that would land before that point is not seen.
   - Render to clip, which made a new clip and switched the Comp off, is gone.

## Consequences

- The chords are visible and editable in the piano roll like any other notes, and every tool that reads notes reads them.
- The Chord-track live-edit path (`ChordPlan`, `try_queue_live_chords`) is gone. Edits to the notes go through the existing MIDI plan rebuild. Edits to Comp's settings stay gap-free through the automation plan.
- `native_dsp` no longer depends on `harmony`.

## Addendum: the Lab speaks melodic house (2026-11-01)

The chords the Lab wrote did not suit the target genres: every chord had a 7th or 9th,
v became a V7 or V7♭9, and passing chords were diminished. Melodic techno,
Anjunadeep, progressive house and trance use the minor scale's own chords
with triad, add9 and sus2 colour, and no dominant pull. Their usual shapes
are i–VI–III–VII, i–VI, VI–VII–i and i–iv–VII–III. The comparison is in
[ui/deep-chords-mockup.html](../ui/deep-chords-mockup.html).

- **Colour.** The Lab now offers Triads, Add9 (the default), Sus2, 7ths and
  Lush. Two qualities are new: `minor_add9` and `sus4`.
- **No diminished chords in the triad colours.** A diminished chord borrows
  its degree from the nearest mode, so A Dorian's i–VI is Am–F.
- **Restlessness.** A restless v becomes a major V triad, the lift trance
  uses (a V7 only in the 7ths and Lush colours). A still v is suspended.
  The 7♭9 is gone.
- **Pull home and Passing.** Pull home plays a major V. A passing chord is
  the scale's chord a step below, not a °7.
- **Shapes.** Vamp is i–VI. Turnaround and Float become Anthem (VI–VII–i–i)
  and Bittersweet (VI–III–VII–i).
- **Voicing.** An add9 chord keeps its ninth off the bottom of the hand.
- **Recognition.** A missing extension costs more, so a pedal under a
  complete chord (Cadd9/A) reads as that chord, not as Am11 without its
  ninth.

The research behind this was general production writing, not
transcriptions. The next step is to tune the Lab against chords transcribed
from reference tracks.

## Addendum: rhythms from the genres (2026-11-01)

The first Chord Comp patterns (Organ house, Garage skank, Rhodes comp,
Dub stab, Detroit push) did not fit this music. Eight
patterns now replace them, taken from published production guides for
melodic house, progressive house, melodic techno and trance. They play the
chord's upper voices only; the bass is the bass line's job, and a chord
clip keeps its bass held underneath.

- **Anjuna offbeats:** offbeat 8ths with a 16th tuck-in before the bar
  (`..x...x...x...xx`).
- **Driving pluck:** progressive trance (`x.x.x..xx.x.x..x`), with the
  beats accented.
- **Rolling pluck:** `x..xx.x.x..xx.x.`.
- **Three-three-two:** hits on 1, the "and" of 2, and 4, each held to the
  next hit.
- **Dotted pulse:** a hit every dotted 8th against the kick.
- **Techno stabs:** on the "and" of 2 and 4, with a chance of arriving a
  16th early.
- **Trance gate:** a 16th gate over the held chord, with the beats accented.
- **Pads & swells:** held for a bar.

Patterns can now set note lengths per step. Comp's lengths are whole 16ths,
so a pluck's roughly 45% gate is a 16th note into a short synth envelope.

## Addendum: Motion (2026-11-01)

A looped rhythm needs small changes to keep it moving. Chord Comp gains five
live parameters. Phrases are measured from the song's start, and every
choice hashes the seed with the song position, so live playback and Apply
agree under any block split.

- **`comp.variation` (0–1):** the share of bars that vary. In a varied bar
  a hit may drop out (never the bar's first) or a ghost 16th may follow a
  hit. At full variation, 60% of bars vary, a hit drops with 35% chance and
  a ghost fills with 25%.
- **`comp.phrase` (4, 8 or 16 bars):** the length every other Motion
  change is measured in.
- **`comp.turnaround`:** in each phrase's last bar, one of three changes,
  chosen per phrase:
  - a ghost 16th tucked in before the next bar;
  - beat 3 left silent, plus the tuck-in;
  - the next phrase's first chord pushed early for certain.
- **`comp.drift`:** for each phrase's second half, the lowest upper voice
  goes up an octave, so another chord tone is on top.
- **`comp.swell` (0–1):** velocity dips by up to 45% at the phrase's turn
  and peaks in its middle (a raised cosine).

Motion leaves the bass lane alone, and the engine defaults are off. The
Rhythm tab starts at Variation 30%, an 8-bar phrase, Turnarounds and Top
drift on, Swell 30%, and a top cycle of one bar (the alternatives are
polymeters of 12 and 7 16ths, through `comp.top_length`). Apply on a clip
shorter than the phrase grows the clip to the phrase, with its chords
repeated, so the variations are written out and editable. Choosing a
pattern now also loads that pattern's own push and groove.

## Addendum: Complexity (2026-11-01)

There are two Complexity knobs, each one control over the detailed ones,
which stay available for fine-tuning.

- **Rhythm Complexity (0–1)** sets:
  - **Density:** the new `comp.density`, equal to the knob. Below the
    middle, the chord's 16ths between the 8ths drop out, and below a
    quarter the ghosts go too. Above the middle, ghost 16ths follow hits,
    up to 60% of the time. Density is the same in every bar.
  - **Variation and Swell:** each 0.6 × the knob.
  - **Turnarounds:** on from 0.25.
  - **Top drift:** on from 0.4.
  - **Top cycle:** 12 16ths from 0.7, 7 from 0.9.
  - **Pushes:** above 0.6, up to 0.5 is added to the pattern's own push
    chance.

  Its middle reproduces the Rhythm tab's defaults.
- **Harmony Complexity (0–1)** sets:
  - **Colour:** Triads, then Add9 from 0.25, 7ths from 0.6, Lush from 0.85.
  - **Pace:** two bars, then a bar from 0.3, then half a bar from 0.75.
  - **Devices:** Top drone from 0.3, Suspension from 0.55, Walking bass
    from 0.7, Passing from 0.8, Borrowed from 0.9. Pedal tone, Pull home and
    Slide stay the musician's choice.

  It starts at 0.4, which is the Lab's defaults. Crossing a band rewrites
  the suggestion, and a drag is one undo step.

## Addendum: what the records do (2026-11-01)

The reference is PML's "12 Iconic Chord Progressions" video and its free
MIDI pack (52 progressions). Measured across the pack:

- **The usual shape over the bass** is octave, octave + fifth, two octaves,
  then the third: (12, 19, 24, 27/28). Next most common is (7, 12, 15/16).
- **Register:** the bass's median is MIDI 41, the chord's lowest note sits
  around 51, and the top note around 64, rarely above 72.
- **Chord lengths:** besides one- and two-bar chords, chords of 3, 6, 14
  and 26 16ths are common. These are the next chord anticipated by a
  dotted 8th or an 8th.
- **Basses:** mostly i, then VI, iv, VII, III and v.

Our recognition reads the pack's progressions correctly (a regression
reference), apart from root–fifth dyads, hence the new `power` quality ("5").

From this:

- **Open voicing (`VoicingStyle::open`, the Chord tab's default).** Over the
  bass, the fifth, the root and the third, with the colour on top.
  Candidates also double the root under the body or drop the top voice an
  octave for a held common tone.
  - Register: bass from MIDI 33, chord 47–72.
  - A minor add9 may rub its ninth a semitone under the third (Ben Böhmer's
    Em(add9)).
  - Clips now split bass from chord at `bass_lowest + 12`, taken from the
    voicing, instead of a fixed E3.
- **No records in the panel.** The 19 progressions read from the records
  (`LabOptions::record`) were removed on 2026-11-02:
  the Write tab offers the Lab's own shapes only. The records still informed
  the vocabulary and voicing above.
- **Comp.** `comp.lift` doubles the top voice an octave up for each phrase's
  second half. There are two new patterns: Anticipated (Sphinx) and Piano
  eighths (Silhouettes, Epikur).

## Addendum: making simple progressions interesting (2026-11-01)

These genres make simple progressions interesting by
articulation, inversions, layering, voice spreading, rhythmizing, running
tonics and rhythmic supporters.

- **Articulation:** `comp.articulation` scales every hit, from 10% of its
  written length (never below a 32nd) up to the full length.
- **Inversions:** Root, 1st and 2nd inversion buttons on the Chord tab put
  the root, third or fifth in the bass.
- **Voice spreading:** the Open voicing.
- **Layers:** `harmony::pad_layer`, `pluck_layer`, `running_tonic` and
  `bass_line`. The Rhythm tab's Layers buttons add each as a new track
  under the chords' track, with a clip in the same place:
  - **Pad:** Open, an octave up, without the bass.
  - **Pluck:** the chords close and high, played by a Chord Comp in the
    chosen rhythm. This is the rhythmic supporter.
  - **Running tonic:** the key's tonic pulsing in 8ths, 16ths or dotted 8ths
    through every chord, each beat accented.
  - **Bass:** held, off-beat 8ths, or rolling 16ths between the beats.

## Addendum: held chords (2026-11-01)

The re-struck rhythms sounded dated. In PML's pack, 49 of 52 chord
parts never re-strike a chord. The median is one chord onset per bar, and
125 of 569 changes land off the beat. Melodic house holds its chords and
moves them in other ways.

- **The Rhythm tab now leads with Held,** which is the default, and with
  **Push and land**.
  - **Held:** the chords are struck once and held.
  - **Changes arrive early by:** None, a 16th, an 8th, a dotted 8th, or
    Varied (the default). Varied cycles a dotted 8th, an 8th, a 16th and an
    8th (`harmony::anticipate`).
  - **Push and land:** before each change, the next chord is touched once,
    for an 8th, softer, a dotted 8th early, then lands on the change
    (`harmony::push_and_land`).
- **Movement on this track** adds an effect to the end of the chord track's
  chain, where the movement belongs:
  - **Pump:** Pump's Classic Pump preset.
  - **Filter:** Contour's new Dotted Breath preset, the cutoff breathing on
    a dotted 8th.
  - **Delay:** Relay's Dotted Eighth preset.
- **Chord Comp's re-struck patterns** and their controls (push, groove,
  Motion, Complexity) move under a collapsed "More · re-struck patterns".

## Addendum: the bass switch (2026-11-02)

The Write tab has a **Bass** switch (On/Off):
- `VoicingOptions::bass`. With it off, nothing sounds in the bass's register.
  The chord's own bass pitch class (its root, or an inversion's note) becomes
  its lowest note, so recognition still reads the same chords in every
  voicing style.
- `rewrite()` also rewrites a region whose bass disagrees with the switch, in
  its rhythm. Turning the bass off or on is one undo step, and new writes
  follow it. With the bass on, a region played without one now always gets
  it held, not only slash chords.
- The switch starts from the clip: off when its chords have no bass notes.

## Addendum: seven more shapes (2026-11-02)

Added:
- Pendulum (VI–VII)
- Shuffle (i–VII–VI–VII)
- Sway (i–iv)
- Hope (i–III–v–iv, whose iv is major in Dorian)
- Odd loop (i–VI–VII)
- Long phrase (i–VI–III–VII–i–VI–iv–v)
- Rise and fall (iv–VI–VII–i–VI–v–iv–i)

Shapes now hold up to eight chords (`max_shape_chords`). A progression that
doesn't fill the clip evenly grows it to the two lengths' common multiple,
up to 16 bars, so Odd loop's three bars run against four in twelve. When
that multiple is longer, the clip grows to whole passes of the progression.

## Addendum: refit to the records (2026-11-03)

The generated chords still fell short of the records. A corpus of every Lab setting was written and measured against PML's Iconic Chord Progressions: 52 progressions and 718 chords of three notes or more.

**What the old output did wrong:**
- **Clusters low down.** 23 % of chords had three voices within a major third, and 11 % had a second under A3. The Open body stacked the root, ninth and third in close position around A3, e.g. F2 C3 F3 G3 A3.
- **Semitone rubs.** 11 % of chords had a semitone between neighbouring voices. The records do this in 0.2 %.
- **Colour outside the mode.** 13 % of add9 chords did this: Phrygian's i got B♮, and Aeolian's v got F♯.
- **Uniform colour.** Every chord was add9, whereas the records are 63 % plain triads with only 8 add9 chords in 430.
- **Unstable voicing.** It ran from four to seven voices and changed shape from bar to bar.
- **A low bass.** It went down to A1.
- **A drone on by default.** Only 1 of the 52 records holds its top note through.

**How the records voice:**
- **Low register:** octaves and fifths only. The first voice above the bass is its octave 51 % of the time and its fifth 36 %.
- **The third** sits a tenth or more above the bass, and is on top in 43 % of chords.
- **Colour** sits above the doubled root, near the top.
- **Seconds** appear only from about C4 up.
- **Size and register:** 3.7 voices over a bass near F2.

**Changes:**
- **Open voicing (`voicing.cpp`) is a search, not a body.**
  - Hard rules:
    - octaves and fifths only below E3;
    - thirds and colour a ninth or more above a root bass;
    - sevenths a seventh or more above it;
    - over a pedal or an inversion, only the E3 floor applies;
    - no semitone between neighbours;
    - one second at most, and only from C4 up;
    - no minor ninth, except in a 7♭9;
    - the first voice a fifth to a twelfth above the bass.
  - Costs: the register (the records' medians), the first voice's interval, the role on top, the voice count, the movement from the chord before, and the top line.
  - Every chord tone sounds:
    - The fifth may be dropped only from a chord with a seventh or more.
    - A sixth chord, a sus chord or a triad keeps its fifth, and so does an altered fifth. Without it the chord reads as another chord: C6 becomes Am/C, Csus4 becomes F/C, m7♭5 becomes m7.
    - When nothing fits in range, a second pass may leave colour out, at a cost per tone left out.
  - Colour may sit three semitones above the range's top, so Cm9's D clears its E♭.
- **The drone** rings from C4 at one pitch. Outside the chord it sits on top.
- **Bass off (Open):** the voicing the chord has over its bass, with the bass's octave as its lowest voice.
- **The bass** is now from C2 (`open_bass_lowest` 36).
- **Colour stays in the mode** (`in_mode`). A ninth, eleventh or thirteenth the mode lacks is dropped, and the chord keeps its seventh. The major V is a triad or a V7.
- **Natural colour** (`Colour::natural`) is the new default:
  - The records' colours by degree: i sus2, iv m7, v sus4, VI maj7, VII 6, plus the major-mode equivalents.
  - Each is drawn from the seed, on one or two chords a loop, never on neighbours alike.
  - Harmony Complexity's bands become Triads, Natural, 7ths and Lush. The Top drone joins only from 0.65.
- **Comp's applied rhythm** keeps the bass held below the voicing's own split (`bass_split`), not a fixed E3.

**Result over the corpus, records / old / new:**

| Measure | Records | Old | New |
|---|---|---|---|
| Semitone between neighbours | 0.2 % | 10.8 % | 0 % |
| Three voices within a major third | 1.2 % | 23 % | 0 % |
| A second under A3 | 1.4 % | 11 % | 0 % |
| Voices over the bass | 3.7 | 4.1 | 4.0 |
| Bass | MIDI 40.8 | 38.3 | 41.8 |
| First voice above the bass | 9.8 | 15.0 | 9.6 |
| Roughness (Plomp–Levelt) | 2.76 | 3.36 | 2.92 |
| Natural colour: plain triads | 63 % | — | 68 % |

**Tests:**
- `open_voicing_sits_like_the_records`: every quality on every root.
- `lab_colour_stays_in_the_mode`, against the modes from theory.
- `natural_colour_is_mostly_plain`.
- `written_chords_sit_like_the_records`: the corpus statistics within the records' bounds.
- `the_drone_rings_at_one_pitch`.
- The round trip now writes every Lab setting in both Open and Drop-2 and reads it back.
