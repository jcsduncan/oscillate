# ADR-0053 — The clip strip: tempo, Follow and downbeat on every audio clip

**Status:** Accepted; the strip, background analysis, downbeat flag, fractional zoom, bars before bar 1 and the docked clip editor are implemented. Warp markers are edited in the clip editor and the tempo map in the inspector; warp markers on the arrangement and a tempo lane are not built. **Date:** 21 October 2026.

## Context

Mixing a DJ set needs every track at the right tempo, on the project's grid, with its first downbeat on a bar. Until now:
- tempo analysis ran only for the selected clip, and its result was thrown away unless a warp proposal stored it;
- setting a clip's tempo meant inspector forms and reviewed proposals;
- the closest zoom was a whole bar.

The design puts all of this on the clip itself and makes it automatic. Warp markers are a last resort.

## Decision

### Every source is analysed once, and a project is never changed by opening it
- `ClipTempoController` queues every audio clip's source that has no tempo review and analyses it on a background worker, through the shared analysis cache (ADR-0052).
- A result lives in memory until it's needed in the project. It's stored as the source's tempo review:
  - when a clip dropped in this session is set up;
  - before the user's own tempo edit on a clip of that source.
- Opening a project therefore never adds a revision.
- The old selected-clip tempo preview, its inspector panel and its "suggested" grid lines are gone.

### A dropped track sets itself up
When an import finishes, its clips are marked as dropped. Once their source's tempo is known they're set up as one edit each:
- Follow on;
- the first downbeat on the nearest bar;
- Complex mode, the default.

The first track dropped into a project with no other audio and no tempo changes also sets the project's tempo to its own BPM. Clips are named after their file; a store's catalogue-number prefix is dropped and underscores become spaces.

### The strip
Each audio clip's header shows:
- its name;
- **♩ BPM** ("♩ …" while analysing; amber when unsure);
- **FOLLOW / OWN TEMPO**;
- the **stretch mode** ▾.

The BPM chip opens the tempo editor:
- type a BPM;
- ÷2 or ×2;
- TAP: four or more taps, restarting after a two-second gap;
- back to the detected tempo.

The mode chip lists Complex, Complex Pro, Beats, Repitch, Tones and Texture, each with a one-line hint.

### The session's clip-tempo edits (schema 1.38)
Each edit is one undo step:
- **Follow** (`set_audio_clip_follow`). The clip becomes musical. Its length becomes the source range's beats at the source BPM, and reviewed markers scale with it. The first downbeat at or after its start lands on the nearest bar. If that would start the clip before zero, it moves later by whole bars instead; following never adds bars before bar 1 (26 October 2026: a loop starting a fraction of a millisecond early had put a silent bar in front of the song).
- **Own tempo.** The clip becomes elapsed and unstretched, starting where it starts now.
- **A typed BPM** (`set_audio_clip_source_bpm`). This sets the source's correction and re-conforms every following clip of that source, with its downbeat kept where it plays.
- **The downbeat flag** (`set_audio_clip_downbeat`). Drag the "1" on the selected clip; it snaps to the grid's beats, and Alt places it freely. The source's tempo review gains `downbeat_frame`, and a following clip moves so the new downbeat sits on the nearest bar.
- **Without a flag**, the downbeat is the analysis grid's first beat, except for a loop. A source whose length is within a 128th note of a whole number of bars at its tempo is a loop, and its first frame is the downbeat: a loop is cut on its bar lines, while the beat tracker may lock onto off-beats (a top loop's hats).
- **Clip names** (`set_audio_clip_name`). These are presentation only and sound the same.

### Zoom
- The arrangement's view is held in fractional bars, from the whole project down to half a beat.
- The ruler labels beats ("5.2") once they are 40 px apart, and the grid draws sixteenths once they are 12 px apart.
- Ctrl+wheel zooms around the pointer. Shift+wheel, or a trackpad's sideways swipe, scrolls through time.
- Automation snaps to sixteenths when they are visible.

## Consequences
- Tracks land on the grid without warp markers. Markers remain for material that really drifts.
- Tempo cache keys move to algorithm version 3 (key analysis to 2), so results from the old algorithms are recomputed.
- An intro can start before bar 1. The timeline still starts at zero, so playback and export need nothing new; the project's `lead_bars` (schema 1.38) only changes how the ruler and position count.
  - Only dragging a clip left past zero adds them. Follow moves the clip to a later bar instead.
  - The added bars play at the opening tempo and meter, so a ramp keeps its timing. Clips, MIDI, track automation, markers, tempo and meter events move later by the same time, as one edit, and the loop range moves with them.
  - The ruler counts the added bars up to 0.
