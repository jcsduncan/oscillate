# ADR-0112 — Warp markers and the tempo lane on the arrangement

**Status:** Accepted. **Date:** 9 November 2026.

## Context

ADR-0053 put tempo, Follow and the downbeat on every audio clip, and left warp markers in the docked clip editor and the tempo map in an inspector form. Mixing a DJ set needs both where the music is: markers on the clip being placed, and tempo ramps across the transitions.

## Decision

### Warp markers on the selected clip
`WarpMarkerOverlay` lies over the selected audio clip's waveform on the arrangement and over the docked clip editor's detail view, with the same gestures in both:
- **Hover** the band along the waveform's top: a ghost snaps to the nearest transient (from the grid check), else the nearest beat of the clip's grid; Alt places it on any sample. **Click** pins a marker there.
- **Drag** a marker to stretch only the audio between its neighbours. Each side's segment tempo shows live, and the waveform redraws where the audio will play.
- **Shift-drag** keeps the marker on its beat and slides the audio under it (`slide_warp_marker`): a late kick moves onto the grid without changing the tempo around it.
- **Double-click** removes a marker.
- **Right-click** lists:
  - Set downbeat here;
  - Straight from here (`straighten_after_warp_marker`): the markers after it go, and the tempo of the segment ending at it carries on to the clip's end, so the clip grows or shrinks;
  - Set tempo from this segment (`warp_segment_source_bpm`): the track tempo becomes the quarter notes the segment spans per minute of its source;
  - Fix drift from here;
  - Remove marker;
  - Remove all after (`remove_warp_markers_after`).

Each gesture and command is one undo step. The pure algebra is in `application/clip_warp_edit`, and edits go through the session's existing clip processing and warp commands.

Fix drift fits the kicks from the chosen bar in two passes. The second pass numbers its beats from the first pass's fitted beat, so the pinned kicks stay on their own beats.

### The tempo lane
`TempoLane` sits under the ruler and draws the tempo as it plays across the visible bars, sampled through the shared tempo map.
- **Collapsed**, it is a thin line. A click, or the "Project tempo" header in the track list, expands it.
- **Expanded**, it edits:
  - drag across bars on empty lane to draw a ramp to the tempo under the pointer;
  - drag a point to move it;
  - double-click to add or remove a point;
  - right-click a point for the shape to the next one: Straight, S-curve, Ease in, Ease out or Hold.
- Positions snap to bars (Alt: free), and tempi to a tenth of a BPM.
- Hovering a ramp shows "126.00 → 128.00 over 8 bars · S-curve".

`SongService` gains three commands, each one undo step:
- `set_tempo_ramp`: keeps the tempo playing at the ramp's start, removes the points inside, and always holds the end tempo until the next later point. A ramp that starts inside an eased segment ends that segment at the tempo playing there, over the shorter span.
- `move_tempo_event`.
- `ramp_to_clip_tempo`: "Ramp to this track's tempo" in an audio clip's menu. It ramps, with an S-curve, from the bar nearest the clip's start to the bar nearest where the latest audio already playing into it ends, or the clip itself ends if that is sooner. The ramp lasts at least one bar. It ends at the clip's reviewed BPM, so the track plays unstretched once the mix lands.

The last tempo point always holds, since there is no later tempo for it to move towards; removing a ramp's end leaves the point before it holding.

Tempo edits switch live as any tempo change does (ADR-0098, ADR-0101); export renders through the same map.

Hovered, an audio clip that follows the project shows how far the project tempo stretches it, in amber past ±6%.

The inspector's tempo map form is retired. The opening tempo and meter stay in the project controls.

## Consequences
- The tempo ramp needs no schema change: ramps were already tempo map events with shapes.
- The tempo lane's expanded state is view state and is not saved with the project.
- The demo project used for the documentation's screenshots gains a warped Top Loop and a ramp into its Drop.
