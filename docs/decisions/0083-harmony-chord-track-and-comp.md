# ADR-0083 — Harmony: the Chord track, the Progression Lab and Comp

**Superseded in part by [ADR-0084](0084-chord-clips.md):** chords are notes in MIDI clips. The Chord track (`Snapshot::chords`, `SetChordTrack`, schema 1.46), Comp's Chord-track mode, its voicing parameters and Render to clip are gone. The harmony library, the Progression Lab and Comp's rhythm engine stay.

**Status:** Superseded by ADR-0084: the Chord track was removed; the harmony library, the Progression Lab and Comp's rhythm engine carry over to chord clips.

**Date:** 1 November 2026.

**Builds on:**
- ADR-0059 (note effects, one-step edits);
- ADR-0072 (Kick & Bass Lock);
- ADR-0078 (Arp v2 lanes and patterns);
- ADR-0079 (grooves);
- ADR-0081 (Clay chord detection).

**Design:** [ui/harmony-lab-mockup.html](../ui/harmony-lab-mockup.html).

**Contract:**
- `include/oscillate/project/model.hpp`: `ChordQuality`, `ChordSymbol`, `ChordDevice`, `ChordRegion`, `Snapshot::chords`, `SetChordTrack`.
- `include/oscillate/harmony/harmony.hpp`: the harmony library.
- The `comp` note effect.
- The Qt `HarmonyController`, as `ProjectBridge.harmony`.

## Context

Requirement: creative MIDI tools for chord progressions, grooves that follow them, and melody lines.

Today the harmony tools are scattered:
- the Chord note effect, the Scale and Random effects, and the Arp each keep their own root and scale;
- the piano roll has chord stamps;
- the song key (ADR-0036) is presentation only.

Nothing knows what chord the song is on.

The design settles three choices:
- **Browse by mood, shape and devices, not genres.** A progression comes from:
  - a mood pad: dark to bright picks the mode, still to restless sets the pull;
  - a shape (a pattern of bass motion);
  - colour and pace;
  - devices: pedal tone, top drone, walking bass, suspension, passing chord, borrowed chord, pull home and chromatic slide. Each device carries a one-line hint.
- **The inspector suggests devices that suit the selected chord** ("Try here"), beside the substitutions.
- **Build Wave 1 now.** Waves 2 and 3 come later:
  - Wave 2: bass from chords, the Motif engine, contour drawing and variations;
  - Wave 3: humming to MIDI, counter-melody, living clips and morph.

## Decision

1. **The Chord track is project data (schema 1.46).**
   - `Snapshot::chords` is a sorted list of non-overlapping `ChordRegion{position, length_ticks, chord{root, quality, bass?}, top?, device}`. The `top` field is a drone pitch class held above the voicing. `device` only records which Lab device placed the region, for the label.
   - **Validation:**
     - at most `max_chord_regions` regions;
     - positions are 0 or later;
     - lengths are positive;
     - regions are sorted and do not overlap;
     - every pitch class is below 12;
     - qualities and devices are inside their enums.
   - **One command, `SetChordTrack{chords}`, replaces the whole lane**, as one undo step. Every Lab action, drag, substitution and hint is computed in the harmony library as "the lane after the edit", so each is one undo step and the library stays pure.
   - **It is audible:** Comp plays the Chord track. The command advances `audible_revision`.
   - The codec writes and reads `"chords"` as a required array. There is no migration in the DAW (greenfield). Existing projects are converted once, outside the DAW, as with 1.45.

2. **The harmony library** (`src/harmony`, `oscillate_harmony`, depending only on project and timeline) holds the harmony logic that every tool shares:
   - **The chord table, names and roman numerals.** Names prefer the key's spelling.
   - **Tension and role:** home, lift, pull or colour.
   - **Voicing:**
     - close, drop-2, spread or rootless;
     - top line free, hold, fall or rise;
     - a register offset;
     - least total movement from the previous voicing;
     - a drone top note added above.

     `voice` is callback safe: fixed arrays, no allocation.
   - **The Progression Lab, `suggest`.** It returns four variants:
     - as the shape;
     - one chord swapped for its functional twin;
     - rotated to start on a different chord;
     - one chord borrowed from the neighbouring mode.

     Every random choice hashes `seed` with SplitMix64, as in ADR-0080, so a given seed gives the same result on every machine. The mood maps as follows:
     - **Brightness picks the mode on the tonic:** Phrygian, Aeolian, Dorian, Mixolydian, Ionian or Lydian.
     - **Below 0.22 restlessness**, the v chord becomes 7sus4.
     - **Above 0.55**, it becomes a major V7.
     - **Above 0.8**, it becomes 7♭9.
   - **The devices** are applied in this order:
     1. borrowed;
     2. suspension;
     3. passing chord;
     4. pull home;
     5. slide;
     6. walking bass;
     7. pedal tone;
     8. top drone.

     A device that has no room leaves the lane as it was.
   - **`tile` and `replace_range`** write a progression into the loop range or a selection.
   - **`next_candidates`:**
     - root-motion weights, with a falling fifth strongest;
     - an outside-the-key penalty and a shared-tone bonus;
     - boosts for resolutions: V7 to i, a °7 rising by a semitone, 7sus4 to 7.
   - **`substitutions`:** richer, simpler, borrowed, sus, tritone sub, lead-in dominant, passing chord, and pedal bass on or off.
   - **`hints`:** up to four devices suggested for the selected chord, each with a sentence saying why:
     - pedal tone;
     - top drone, when a note is shared with both neighbours;
     - passing chord, after a jump of a minor third or more;
     - suspension, on the V or the last chord;
     - pull home, on the last chord;
     - borrowed, on a chord that isn't home;
     - inversion, when the bass could step.

3. **Comp is a note effect** (engine `comp`, version 1, ADR-0059). It plays the Chord track in a rhythm on any built-in instrument.
   - **Follow:**
     - **Chord track** (the default): it plays whenever the transport runs. Input notes pass through untouched.
     - **Held notes:** it voices the held notes, like Arp's Chord mode.
   - **Parameters:**
     - **Lanes:** `low.1..16`, `mid.1..16` and `top.1..16` each choose off, ghost or on, for the lowest voice, the inner voices and the top voice.
     - **Lengths:** `length.1..16` in 16ths, from 1 to 32.
     - **Polymeter:** `top_length` from 1 to 16, so the top voice can run against the bar.
     - **Pushes:** `push_chance` from 0 to 1, and `push_amount` of a 16th, an 8th or a dotted 8th.
     - **Groove:** straight, 56, 58, 62 or 66 % swing.
     - **Feel:** `humanise` and `seed`, plus `velocity` and `ghost_velocity`.
     - **Voicing:** `voicing` style, `top_line`, `register` (−12..12) and `octave`.
   - **Pushes:**
     - A chord change at tick *C* is pushed when the hash of (seed, *C*) is below the push chance.
     - Steps within the push amount before *C* then use the next chord.
     - A hit is added at *C* − amount if the lanes have none there, lasting its length plus the amount.
     - The hit at *C* itself is dropped.
   - **Determinism.** All timing is in project ticks through `NoteClock`, and everything chosen by chance hashes the seed with the step's tick. Any block split, live playback and export therefore give the same notes. Humanising only delays notes, never moves them earlier.
   - **The chord plan.** `NoteEffectChain` receives an immutable, shared chord plan built on control from `Snapshot::chords`: the regions in ticks. Comp voices each region on the callback with `harmony::voice`.
     - A Chord track edit while playing swaps the plan through the live-edit path. The change is gap-free and does not rebuild the source (seamless live edits).
     - A region the transport is inside keeps sounding until Comp's next step.
   - **Patterns.** Six patterns are a table in `application/comp_patterns` that the panel applies as one settings edit: Organ house, Garage skank, Rhodes comp, Dub stab, Pads & swells and Detroit push.
   - **Render to clip** runs the same kernel offline over a range through `NoteClock`, so the clip equals what played. The result is one undo step: a new MIDI clip on the track, with Comp switched off.

4. **The UI.**
   - **The Chord lane** sits under the ruler in the arrangement and can be hidden.
     - Regions show their name, roman numeral and device tag.
     - Click selects a region; dragging its right edge resizes it, snapping to beats; right-click opens the substitutions.
   - **The Harmony panel** (Ctrl+K "Harmony", or the lane header):
     - **Left:** the mood pad, shapes, colour and pace, devices with hints and placement, the four suggestions, and More.
     - **Centre:** the inspector, with the name and roman numeral, role and tension, voices, length, split, duplicate and delete, Try here, substitutions, and the voicing preview.
     - **Right:** what comes next, as the radial graph and the list, with Replace next or Insert after.

     A Lab choice writes into the loop range, or to bars 1–4 when there is no loop, and plays the first chord. Each change is one undo step.
   - **The Comp panel:**
     - the pattern grid with low, middle and top lanes and the length lane;
     - pattern chips, push, groove, humanise and voicing;
     - a preview roll of the notes it will play, with pushes marked;
     - Render to clip.

## Consequences

- The Arp, the Chord, Scale and Random effects, Clay's "fit to key" and Kick & Bass Lock can now read the Chord track (Wave 2), instead of keeping their own root and scale.
- **Take chords from a record**, using Clay's polyphonic detection with the existing key analysis, comes after Wave 1. It writes through `SetChordTrack` like everything else.
- VST3 instruments get Comp's notes as they get other note effects' notes: they don't. ADR-0059 refuses note effects on VST3 tracks.
