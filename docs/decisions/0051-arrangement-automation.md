# ADR-0051 — Automation drawn on the arrangement

**Status:** Accepted and implemented. The lanes are the only automation editor. **Date:** 21 October 2026.

## Context

Mixing a 60–90 minute DJ set means many automation moves (level, filters, sends, effect parameters) placed against the music. Until now every lane was edited in inspector forms, typing a bar and a value into each point. That is far slower than drawing on the timeline, which is the standard set by other DAWs.

The session API added points only at whole bars and edited one point at a time.

## Decision

### Engine: tick-precise lane edits
`ProjectSession` edits any track automation target at any tick. Each call is one undo step:
- `automation_domain(track, target)`: what a target's normalised value means.
- `add_automation_point`: creates the lane when needed.
- `move_automation_points`: moves several points as one gesture; points that would coincide are refused.
- `remove_automation_points`: removing the last point erases the lane.
- `draw_automation`: replaces the points in a range with a painted stroke.

Values are normalised along each parameter's own feel:
- dB fader law for level and sends (−70 to +12 dB);
- balance for pan;
- on/off for mute;
- the engine's scale for a built-in synth or effect parameter;
- the plug-in's own normalised value for VST3.

### Quick: `ArrangementAutomationController`
The controller projects every track's lanes and the targets it can automate: Mixer, Sends, engine groups and plug-ins. It uses keys `level`, `pan`, `mute`, `send:<id>`, `fx:<slot>:<param>`, `synth:<param>` and `vst3:<plugin>:<param>`, with positions in fractional bars.

Its `curve()` samples the curve exactly as it plays, using the engine's shapes in physical space. What is drawn is therefore what is heard.

### The overlay is the editing surface
- **A** (or the track's A button) shows automation on a track. The chosen parameter's envelope lies **over the clip row**: the waveform or notes show through, dimmed, and the pointer edits the curve, not the clips. A picker in the track header chooses the parameter, defaulting to the lane the track already has, or Level.
- **Extra lanes** beneath the track are optional, for watching more parameters at once. "Show another lane below" adds one, and × hides it.
- **Gestures:**
  - Click adds a point, snapped to the beat (Alt places it freely).
  - Dragging a point, or the selection, moves it.
  - Dragging a line between points moves that segment up or down.
  - Shift-drag box-selects.
  - Double-click or Delete removes.
  - Right-click picks a point's curve: Line, Ease, Exp, Slow or Step, and takes its exact value, typed in the parameter's own units ("−6 dB", "L 20", "1.2 kHz", a choice's name).
  - Right-click off the points switches the lane's timebase (Musical follows tempo changes; Elapsed keeps clock time, points keeping their times) or clears it.
  - **B** (or ✎ Draw) paints the envelope.
- A gesture previews locally and commits once on release, so each gesture is exactly one undo step.
- Only the visible bars are drawn, keeping a 3000-bar set responsive.

## Consequences
- Automation is edited where the music is, with one undo step per gesture.
- The inspector and device-editor automation forms are gone. A device editor's Automate shows that parameter's lane over its track's clips; a clip's level envelope carries its own tools for an exact level, the timebase and Clear.
- Which lanes are shown is view state, held per session in `ArrangementWorkspace` and not saved with the project.
