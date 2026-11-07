# ADR-0019 — Persisted automation identity and preparation

**Status:** Accepted; implemented. Automation lanes target channel level, pan and mute, send gains, VST3 parameters, native-synth and native-effect parameters, and macros; see the schema table in [`src/persistence/README.md`](../../src/persistence/README.md).
**Date:** 15 October 2026
**Requirements:** PRD §14, §§8 and 18; FS-06 AM-01–04/07; FS-04 PI-04; ADR-0007/0008.

## Context

`automation::PreparedAutomation` already evaluates validated, absolute musical-tick or
project-sample curves with clip precedence. It is not project state. Schema 1.8 has no
automation fields, mixer gain/pan identity, or native insert identity. VST3 placeholders
do have stable `PluginInstanceId` and numeric parameter IDs, but a missing plugin cannot
provide a fresh parameter descriptor. Saving only a normalized value or a display label
would lose the meaning of a curve or its binding.

## Decision

### Durable identities and values

- Add stable `AutomationLaneId` and `AutomationPointId` to project state. A lane has one
  owner (`TrackId`, or an audio/MIDI `ClipId`), one target, one `automation::Curve`, and a
  nonzero settings generation that increments on edit and is restored by undo. Point
  IDs remain stable through a point edit and change on duplication. Curves keep their
  current ordered, unique
  coordinates and start-point `to_next` interpolation/normalized Bézier handles.
- A target is tagged, never a UI label or insert index: `track_control` (track ID plus
  fixed, versioned control key such as `gain`/`pan` when those controls exist),
  `native_processor` (stable future processor-instance ID plus versioned parameter key),
  or `vst3_parameter` (`PluginInstanceId` plus VST3 `ParamID`). Clip lanes also store
  this target; the target must belong to the clip's owning track. Moving a plugin
  within its chain does not retarget lanes. Unknown native keys and unresolved VST3 IDs
  remain durable, visibly unresolved bindings and are never silently mapped by name.
  A missing plugin retains its exact instance ID, chain position, state and automation
  links. Explicit plugin removal must show the affected lanes and remove those lanes
  atomically with the plugin, unless the user keeps a missing placeholder instead;
  undo restores them. Removing a track/clip removes its owned lanes as part of that
  user's atomic delete; undo restores them.
- Persist a descriptor snapshot with each lane: representation (`physical` for native
  parameters, `vst3_normalized` for VST3), semantic unit/key, finite minimum/maximum,
  default, and discrete/step count metadata. Native curve values are in physical units.
  VST3 curves use the VST3 host transport range [0,1], **not** a guessed physical unit;
  the plugin's plain-text conversion is display only. On plugin discovery, compare
  current metadata to the saved snapshot and flag incompatibility. Do not silently
  rescale or discard a saved curve. The saved descriptor is sufficient to validate and
  display an unresolved placeholder. Changing a descriptor requires an explicit
  migration/rebind command with preview; copy/paste uses semantic-key and domain checks.

### Coordinates and playback

- Track lanes store absolute nonnegative project ticks or project samples as the
  existing evaluator expects. A clip lane stores **nonnegative clip-local** ticks or
  samples in the selected timebase, independently of its clip placement timebase.
  Musical and elapsed anchors default to the clip's musical and rendered-sample start
  respectively. Moving a clip changes its anchor, not its stored envelope points;
  track lanes remain at their project coordinates. Trimming/resizing does not delete
  points beyond the current clip end, so extending can reveal them again. No implicit
  loop/repeat of an envelope occurs for a looping MIDI pattern: one envelope spans the
  clip's arrangement interval. A later loop mode would need an explicit format revision.
- On the control thread, render each clip's half-open active sample interval from the
  candidate project tempo/rate, convert its local point coordinates to absolute
  coordinates with checked addition, and create `PreparedAutomation`. A musical local
  envelope on an elapsed-anchored clip uses the shared tempo map to find the clip-start
  tick; an elapsed local envelope on a musical clip uses its rendered start sample.
  Tempo/rate/placement edits must revalidate affected preparations, including rounding
  collisions and overflow, before graph publication. If preparation fails, retain
  the durable edit but mark its audio application failed under the existing
  model-versus-active-graph contract; reject structurally invalid project commands.
- Only the active clip's matching envelope replaces its track lane on that parameter
  over `[clip_start, clip_end)`. At `clip_end`, evaluate the track lane at the current
  coordinate. No blend or sum. The shared `PreparedAutomation` evaluator and shared
  timeline tick buffer supply both playback and offline export at the same revision;
  a graph adapter selects among a track's clips because the current prepared API
  accepts only one clip envelope at a time. Smoothing follows evaluation and cannot
  shift event positions. Project sample-rate changes preserve musical coordinates;
  elapsed sample coordinates retain their elapsed seconds across a project sample-rate
  change. The rate-change command converts every elapsed track and clip lane point with
  the shared exact-ratio, ties-to-even timing helper, increments each affected lane's
  settings generation, and rejects the whole edit on overflow. Undo restores the prior
  coordinates and generations exactly. Musical coordinates remain unchanged.

### Commands, validation and schema

- Proposed schema **1.9** adds `automation` arrays to each track and each audio/MIDI
  clip. Track lanes are owned by the track; clip lanes by that clip. A lane record has
  `id`, `target`, `domain`, `timebase`, `points`, `settings_generation`; each point has
  `id`, `coordinate`, `value`, `to_next`, and all four normalized Bézier handles.
  The tagged target JSON is `{"kind":"vst3_parameter","plugin_id":"…","parameter_id":7}`
  for VST3, `{"kind":"track_control","track_id":"…","key":"gain"}` for a
  mixer control, or `{"kind":"native_processor","processor_id":"…","key":"wet"}`
  for a native insert. A domain JSON record contains `representation`, `semantic_key`,
  `unit`, `minimum`, `maximum`, `default`, `discrete`, and `step_count`; the native
  processor ID type and acceptance of that target kind wait until persistent native
  insert instances exist. Track-control keys are accepted only when their backing
  persisted control exists.
  The codec must keep strict exact fields, bounded JSON nesting and the 16 MiB manifest
  cap. Schema 1.0–1.8 decode with empty automation arrays; old writers reject 1.9.
  Do not rewrite older manifests merely by opening them. No synthesis from VST3 saved
  parameter references: those are an initial static state, not an automation curve.
- Add `AddAutomationLane`, `ReplaceAutomationLane`, and `RemoveAutomationLane` typed
  commands. `Replace` carries the stable lane ID and complete new lane value; UI point,
  handle, curve, timebase and target edits issue one such command per logical operation,
  using the existing gesture ID for continuous edits. Clip move plus any explicit
  track-point movement is one compound `apply`, never a hidden second revision. The
  default is clip envelope travels and track points stay. If the UI detects an
  ambiguous interaction with track automation, it asks for the FS-06 choice before
  issuing that compound command. Undo/redo restore exact lane data and point IDs.
  `ChangeSet` gains affected lane IDs and an automation dirty flag; it remains a hint.
- Enforce unique lane and point IDs project-wide, one lane per owner/target pair,
  finite descriptor/value/handle numbers, `minimum < maximum`, values in range,
  sorted unique nonnegative coordinates, valid shapes/timebases, and step/hold for
  discrete parameters. Proposed initial bounds: 128 lanes per track, 32 per clip,
  65,536 lanes and 1,000,000 points per project, 4,096 points per lane. The 16 MiB
  manifest limit remains authoritative and may make smaller practical projects hit
  the cap first. Validate incrementally while decoding and validate the full candidate
  snapshot before publication; fail oversized or malformed input without partial
  state. Tighten these limits only with measured real-project evidence.

## First implementation slice and evidence

Implement the 1.9 model/codec/commands and migration together, including a save/reopen
fixture with a missing VST3 placeholder and its automation lane. Consumers that cannot
prepare/apply automation must explicitly reject that revision or show pending/failed
application; neither live playback nor export may silently ignore a saved lane. Then
bind one stable native track control end to end through the same prepared evaluator in
live and offline paths before calling AM-01–04 functional. This is a schema/contract
sequence, not a claim that the current mixer has that control.

Required tests: 1.8 migration; exact 1.9 round trip; duplicate/orphan/oversized and
nonfinite manifest rejection; atomic compound and gesture undo; clip move, trim and
tempo/rate remapping; missing-plugin retention; track/clip handoff at exact sample
boundaries; playback/export trace equality; and failure status when a binding cannot
activate. Save fault injection remains under ADR-0004. Callback allocation/lock tests
apply to the eventual graph adapter, not to the model codec.

## Alternatives and consequences

Absolute persisted clip points would require rewriting every point on a move and
could change shape under rounding. Plugin names or chain positions would retarget
automation after reorder or relink. Persisting only live plugin metadata would make a
missing plugin's curve unverifiable. Full lane replacement is initially simpler and
already fits atomic project revisions; point-level commands can follow if measured
editing cost warrants them. The descriptor snapshot is historical intent and cannot
prove a changed third-party plugin behaves identically; compatibility diagnostics
must remain visible.
