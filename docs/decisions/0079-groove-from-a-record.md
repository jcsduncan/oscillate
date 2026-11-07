# ADR-0079 — Groove from a record

**Status:** Accepted; implemented. **Date:** 31 October 2026.
**Date:** 31 October 2026.

**Builds on:**
- ADR-0059 (the groove pool and its `GrooveTemplate` files);
- ADR-0052 (constant tempo and key analysis);
- ADR-0074 (Mix Check's reference decoding).

**Contract:**
- `include/oscillate/application/groove_capture.hpp` provides `detect_onsets`, `fit_grid`, `downbeat_shift`, `measure`, `describe`, `capture_audio`, `capture_file`, `capture_clip` and `audition`.
- `NoteToolsController::keep_groove`.
- The Qt `GrooveCaptureController` is exposed as `ProjectBridge.grooveCapture`.
- The panel is `apps/daw_quick/qml/GrooveCapturePanel.qml`.

**Design:** `docs/ui/creative-tools-mockup.html#groove`.

## Context

The groove pool had two kinds of groove:
- **built-in swings;**
- **a one-bar groove learned from an audio clip's reviewed onsets.**

The learned groove came from whatever onsets the tempo review had kept, at unit strength, with no notion of which part of the record they came from. So:
- a kick's push and a hat's shuffle were averaged together;
- there were no accents;
- nothing said whether the result could be trusted.

In minimal techno and house the feel lives in particular parts: hats that drag on the off-16ths, percussion that pushes, a kick that stays on the grid. Requirement: lift that feel from a record in the library or from a loop in the project, and apply it to other parts.

## Decision

1. **Bands.** A groove is taken from one band of the record. The user picks it; Hats is the default. The bands are:
   - **All:** everything above 30 Hz.
   - **Kick:** below 150 Hz.
   - **Perc:** 300 Hz to 3 kHz.
   - **Hats:** above 6 kHz.

   Each band filter is two cascaded Butterworth sections per edge, giving 24 dB per octave. The detector can only measure what it hears, so a band keeps the other parts' timing out of the groove. A kick's push on beats 2 and 4 does not leak into the hats, and the tests hold that.

2. **Onsets.** The band runs through an envelope follower with a 0.5 ms attack and a 30 ms release, read every 1 ms.
   - **A hit** is the steepest rise of at least 6 dB over 5 ms. Hits are at least 30 ms apart, which is a 32nd at 250 BPM, and each peaks within 30 dB of the band's loudest.
   - **Its start** is placed at the first sample where the band climbs half the way from the valley before it to its peak, interpolated between samples. The valley is the quietest envelope frame over the rise's 5 ms (the last of equals). Walking back to the first dip instead stopped inside noisy hits, whose envelope wobbles as it climbs, and placed about one hat in eight 2 to 5 ms late.
   - **Its strength** is its peak.
   - **Detector delay.** The band filters and the start threshold make each band's detector late by a different amount. Each band's delay is a named constant, `band_latency_ms`, measured by running the detector design on a reference hit:

     | Band | Delay | Reference hit |
     |---|---|---|
     | Kick | 3.5 ms | a kick falling from 130 Hz to 50 Hz |
     | All | 0.8 ms | the same kick, since All's downbeat hits are a kick's |
     | Perc | 0.6 ms | the mean over 40 noise bursts of 20 ms |
     | Hats | 0.1 ms | the mean over 40 bright noise bursts of 12 ms (spread 0.1 ms, at most 0.5 ms) |

     `detect_onsets` takes the delay off every hit, so the hits of different bands line up with each other.

   Every threshold is a named constant in `groove_capture.cpp`.

3. **The grid.**
   - **A project clip** uses the song's own grid. Each hit is mapped through the clip's warp to song ticks, as playback places it.
   - **A library file** has no song grid, so one is fitted to its kick hits:
     - The tempo is the one between 85 and 175 BPM whose quarter-note grid the hits fit best: the weighted resultant of their phases, scanned in 0.05 BPM steps and refined in 0.001 steps. When half that tempo fits within 98 % as well, the half wins.
     - A tempo in the file name, or one the user types, replaces the search.
     - The downbeat is the beat of the bar whose kicks are strongest.

   The measured window starts at the first downbeat at or after the place the user picked (a library file), or at the first bar line in the clip (a clip). It runs 4 to 32 bars.

4. **The measurement** (`measure`, pure, on hits in song ticks).
   - **Steps.** Each hit belongs to its nearest step of a 16- or 32-step cycle on an 8th, 16th or 32nd grid. The panel offers 16 steps (a bar of 16ths), 32 steps (two bars of 16ths, for parts that change in their second bar) and 32 32nds (one bar).
   - **Measured against the kick.** The groove is each part's feel against the kick. A groove's main content is hats or percussion laid back or pushed against an on-grid kick, so the band's own downbeats are never the reference.
     - **The reference.** `downbeat_shift` takes the kick band's hits in the same window and returns their mean offset at the downbeats, or at the beats when no kick lands on a downbeat. When the kick has no such hits it falls back to All's, then to the chosen band's own.
     - **Applying it.** That one shift moves every hit of the chosen band before it is measured.
     - **A record:** its grid is already fitted to the kicks, so the shift only removes what is left of the fit's error.
     - **A clip:** the shift cancels the clip's own offset from the song's grid.
     - **Hands off:** hits placed against the grid by hand are not shifted.
   - **Per step:**
     - the mean offset as a share of a step, clamped like any groove to ±0.5, and in milliseconds at the record's tempo;
     - its spread (the standard deviation);
     - its level: the mean strength against the mean of all hits, clamped to 0.25..2;
     - its hit count.

     Steps without hits stay straight at level 1.
   - **Confidence** is coverage × tightness:
     - **coverage** is hits against one hit per beat over the bars;
     - **tightness** is 1 − (mean spread ÷ 0.15 of a step), floored at 0.

     Random hits score near 0, and a played part scores near 1.
   - **Summary:**
     - **swing** is 50 % plus half the mean late offset of the odd steps, matching the built-in swings' definition;
     - the offbeat 8ths' mean offset is reported in milliseconds;
     - **accents** are steps at 1.15 × the mean level or more.

     `describe` puts all of this in plain words, and prefixes "Loose" when confidence is under 0.35.

   `measure` is the one implementation; the old `extract_groove(hits, …)` has been removed. The application's `audio_clip_hits`, NoteToolsController's one-bar `extract_groove` and the clip menu's "Extract groove" are removed. The clip menu's "Take its groove…" opens this panel on the clip.

5. **The panel.** It opens from three places: the piano roll's groove pool (RECORD…), an audio clip's menu, and the command palette ("Groove from a record…").
   - **Left: the reference.** You pick the project's audio clips or the sample library's files, or "Choose a record…". Below that are the band, the bars to average over (4, 8, 16 or 32), the steps, and for a file the tempo ("auto" by default). Take groove runs the capture on a worker; Stop cancels it.
   - **Right: the fingerprint.**
     - the first measured bar's waveform with its band's hits marked;
     - each step's offset as a bar (amber late, blue early) with its spread as a whisker, and every hit folded onto its step as a dot;
     - each step's level, with accents bright;
     - the tempo, swing, offbeats, accents, hits and confidence, plus the plain-words summary;
     - hovering a step reads it out.
   - **A/B.** The A/B buttons play four bars of a kick and straight hats (A), then the same hats through the groove's timing and levels (B), at the song's tempo. The audio comes from `audition`, written as two WAV files to the cache and played by the browser's sample preview, which is already beat-synchronised.
   - **Below:**
     - **Save** keeps the groove in the pool under the panel's name, through `NoteToolsController::keep_groove`, and makes it the current groove.
     - **Save and apply** also grooves each selected MIDI clip with the pool's Quantise, Timing and Velocity amounts, one undo step per clip.

6. **Where it works.** A saved groove is an ordinary pool groove. It works wherever a groove does:
   - **MIDI clips:** the piano roll and the clip menu.
   - **Knock:** its patterns are written into MIDI clips.
   - **The Nova arp:** its groove picker reads `NoteToolsController.grooves`.

   **Chop has no groove input.** Its steps have Swing only, and adding per-step timing to an effect's parameter table is a separate change.

## Consequences

- A groove can be taken from a record in the library with no project audio, and from a band rather than the whole mix.
- The capture reads at most 33 bars at 85 BPM, about 93 s of mono audio (18 MB at 48 kHz), on a worker. A song-length file is never decoded whole.
- **Restrictions:**
  - Fitting a tempo needs at least eight kick hits, falling back to all hits. A record with no steady beat is refused with "Too few hits to find the beat".
  - A library file's tempo is assumed constant.
  - The bar is 4/4.
- **A lean is kept.** Hats that sit uniformly 10 ms behind the kick read as 10 ms late, which is the groove. A record whose kick itself drags behind its own beat grid reads as on time, because the kick is the reference.
- **All has one fixed delay.** The All band's delay is fitted to a kick. When All is the chosen band, its hats read about 0.7 ms early against the kick; choose Hats or Perc for those parts.

## Tests

- **`application_groove_capture_contract`:** synthetic records give their feel back:
  - swung hats: 0.3 of a step late within 0.03, 65 % swing, four accents a bar, confidence over 0.7 and plain words;
  - a kick that pushes beats 2 and 4 by 0.15 is measured in the Kick band and absent from the Hats band;
  - 8th hats 10 ms behind an on-grid kick read as +10 ms (within 1.5 ms) on every step and on the offbeats;
  - random hits score under 0.35 and read "Loose";
  - a two-bar cycle keeps its second bar's drag;
  - the grid finds 125 BPM (not half or double) and the downbeat with no tempo given;
  - pure `downbeat_shift` (downbeats, else beats, else none) and `measure` with and without a shift, plus milliseconds, swing and confidence;
  - the A/B audition is whole bars and repeatable;
  - bad requests are refused.
- **`quick_groove_capture_controller_contract`:** a WAV record taken on the worker gives its tempo, swing, steps, waveform, summary and A/B files, and is kept under the panel's name.
- **`quick_note_tools_controller_contract`:** `keep_groove` saves under a file-safe name and makes the groove current, and refuses an empty name.
