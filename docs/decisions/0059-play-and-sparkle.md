# ADR-0059 — Play and sparkle: note effects, macros and freeze

**Status:** Accepted; implemented (schema 1.43 contract, engines, playback and panels). Superseded in part by [ADR-0099](0099-bounce-moves-mute-lanes.md), where it says a bounced source under a mute lane keeps its keys: a bounce moves the source's mute lanes and keys to the copy. **Date:** 25 October 2026.
**Builds on:** ADR-0034 (stepped native automation), ADR-0040 (native effect platform), ADR-0043 (one native insert path), ADR-0051 (arrangement automation), ADR-0058 (effect documents and links).

## Context

A review on 25 October 2026 listed what the DAW lacked for playing and finishing music; this programme covers that list except live playing and recording. Three of those items need new saved state:

- MIDI effects that play before a track's instrument (arpeggiator, chord, note echo, randomiser, scale);
- eight macro knobs per track, which drive parameters across the chain and can be automated;
- freezing a heavy track to a render.

This ADR fixes that persisted contract (schema 1.43) before the engines, playback and panels are built against it. Later sections record the rest of the programme.

## Decision: the schema 1.43 contract

### 1. Note effects

- `Track::note_effects` is an ordered list of up to eight `NoteEffectInsert{id, enabled, engine, engine_version, parameters}`, allowed only on instrument tracks.
- Parameters are sparse plain values by the engine's stable ids, like a built-in effect's. The model checks structure only; the engine resolves ids and ranges, so new note effects need no schema.
- Slot ids share the effect slots' allocator (`next_insert_slot_serial`, `command_insert_slot_id`) and namespace: one id names one note effect or one insert across the project.
- Commands: `AddNoteEffect` (the model allocates the id), `SetNoteEffectSettings`, `MoveNoteEffect`, `SetNoteEffectEnabled` and `RemoveNoteEffect`.
- Removing a note effect drops macro mappings and lanes that name its slot.
- A note effect parameter is addressed as `NativeEffectParameterTarget{slot, parameter}`, the same form as an insert's.

### 2. Macros

- `Track::macros` holds up to eight `Macro{name, value 0..1, mappings}`, indexed by position. An empty name shows as "Macro n".
- A `MacroMapping{destination, domain, minimum, maximum}` names one parameter on the macro's own track:
  - the built-in instrument's (`NativeParameterTarget`, naming the track);
  - or a built-in effect's or note effect's (`NativeEffectParameterTarget`, whose slot is on the track).
- The mapping's domain is the destination's automation domain, copied from its engine when the mapping is made. Preparation and the session therefore need no engine lookup to map a value.
- Minimum may exceed maximum, which inverts the mapping. Both lie inside the domain.
- **Mapping rule** (`automation_project::macro_parameter_value`): a macro at `v` gives the blend from minimum to maximum at `v`, in the domain's scale (linear, logarithmic or decibel, as automation segments blend). It is rounded onto the domain's steps when discrete, and clamped to the domain.
- **Static values.** Turning a macro writes every mapped parameter's saved value in the same command (one undo step, gestures coalesce). Engines, players and stages therefore see plain parameter values and need no macro awareness.
- **Validation:**
  - a parameter has at most one macro;
  - a mapped parameter has no automation lane of its own (a lane on the macro drives it);
  - removing a parameter's owner (its synth engine, effect slot or note effect slot) removes the mapping.
- **Automation.** `MacroTarget{track, index}` joins `AutomationTarget`. Its domain is fixed (`macro_automation_domain`: physical, key `macro`, unit `normalized`, 0..1, linear, continuous). `prepare_project_automation` expands a macro lane into one prepared lane per mapping, targeting the mapped parameter, with the mapping applied after evaluation. No prepared lane targets a macro, so consumers bind lanes exactly as before. Removing a macro (shrinking the list) removes its lanes.
- Duplicating a track copies macros, their mappings (renamed to the copy's slots) and macro lanes.

### 3. Freeze

- `Track::frozen` is an optional `FrozenTrack{audio, first_frame, rendered_revision}`, allowed on audio and instrument tracks.
- The render is a stereo managed WAV at the project rate, whose identity must match a source record exactly, as a convolution IR's does. Its frame 0 sounds at project frame `first_frame`.
- The render replaces the track's instrument, note effects and inserts. The fader, pan, mute, solo, sends and routing, and lanes on them, still play live.
- **Edits are refused while frozen, not unfrozen implicitly.** A command that changes what a frozen track rendered is refused with "unfreeze it first". That covers the instrument, note effects, inserts, clips, notes, macros and parameter automation; and the tempo or sample rate while any track stays frozen. Freezing and unfreezing themselves, and edits in the same command that freezes, are allowed.
- **Why refuse:** an implicit unfreeze would silently swap a cheap render back for a heavy live chain, and a stale render must never play.
- **Session:**
  - `begin_track_freeze` stages the render under the next source id against the current revision.
  - `finish_track_freeze` commits `AddSource` (unless the same bytes are already a source) and `SetTrackFrozen` in one undo step, then saves. It refuses a render if anything audible changed since it began (`audible_revision` passed the origin revision), so a fader move during the render also makes it stale.
  - `unfreeze_track` keeps the render's source record.
- Duplicating a frozen track freezes the copy to the same render.

### Session API (`project_session_track_devices.cpp`)

- **Note effects:** `add_note_effect`, `remove_note_effect`, `move_note_effect`, `set_note_effect_enabled` and `set_note_effect_parameters` (with a gesture).
- **Macros:** `set_macro_value` (with a gesture; writes the mapped parameters), `set_macro_name`, `add_macro_mapping` (copies the domain and sets the parameter from the macro at once) and `remove_macro_mapping`. Touching a macro index beyond the track's list adds default macros up to it. Mapping a note effect's parameter is refused until note effect engines can report domains.
- **Freeze:** `begin_track_freeze`, `finish_track_freeze` and `unfreeze_track`.

## Decision: freeze, bounce and consolidate

1. **One renderer.** `exporting::render_track` renders one track through the export path (`render_master_impl` soloed), so a render is exactly what export plays.
   - **What it renders:** the track's instrument, note effects and inserts, with their automation, keyed by the project's sidechains. It renders before the fader, pan and sends: the track is set to unity, centred, unmuted, unsoloed and sent straight to a master with no chain.
   - **Length:** the track's own content sets it. A key's track does not lengthen the render. The file starts at the first frame the track sounds (`first_frame`) and runs to the end of its tail, or covers exactly a requested range (Bounce the loop).
   - **Progress and cancel:** progress is offered every 16 blocks, and false cancels without publishing.
   - **Session:** `begin_track_render` pins the snapshot and a store reader. `TrackRenderTask::run` renders on a worker into a private file, already unlinked, whose descriptor `TrackRender` owns. `begin_track_render_stage` stages it as the next managed source.
2. **The frozen view.** `project::frozen_playback_view` is the project as it sounds. Each frozen track becomes an audio track with the same id, playing its render from `first_frame`. It keeps its channel, sends, routing, and the channel and send lanes. Its instrument, note effects, inserts, macros, clips and other lanes are dropped, and so are other tracks' keys into its inserts (the render heard them).
   - **Where it applies:** export (`render_master_impl`, `render_stems_impl`) and the Quick playback run (`RoutedPlayback::prepare_run`, `begin_revision`) build from this view. Neither ever runs a frozen track's devices, and a freeze or unfreeze reaches a playing graph as a source edit.
   - **Monitor runs:** they drop `frozen` along with every clip, so a frozen track's devices still answer live notes.
   - **Direct path:** the direct (unrouted) path never plays a frozen project.
   - **Tested:** a project with a frozen key and a frozen keyed track exports what it exported unfrozen, to 1e-6.
3. **Bounce and Flatten.** `finish_track_bounce(staged, origin_revision, track, first_frame, mode)` is one undo step, then a save. A render is refused as stale once anything audible has changed since it began.
   - **Bounce** (`new_track`) adds an audio track below the source playing the render, with the source's channel, routing, sends and channel lanes, and mutes the source. The copy takes the source's keys too, since a muted track keys nothing; a source whose mute a lane drives keeps its keys, and the copy takes none.
   - **Flatten** puts that audio track in the source's place with all of its sends, mute and solo, removes other tracks' keys into the source's inserts, and removes the source.
   - **Tested:** both export what the source played.
4. **Consolidate.** `consolidate_midi_clips(track, start, end)` turns a track's bar-placed MIDI clips overlapping the range into one unlooped clip spanning them all, as one undo step.
   - **How the notes are written:** every repeat is written out as compiled playback reads it. A note past its pattern stays silent, one crossing the pattern or the clip end is cut, and a pedal down at a clip end is let go there.
   - **Overlaps:** a note repeated by overlapping clips is kept once, the longest and loudest.
   - **Refused:** clips with clip automation, and a single unlooped clip.
5. **Quick.** `FreezeController` (`bridge.freezeCommands`) runs one job at a time.
   - **Workers:** the render and the staging each run on a worker, with progress and Cancel, and commit on the UI thread.
   - **Track menu:** Freeze / Unfreeze, Bounce to audio track, Bounce the loop (when the loop is set) and Flatten.
   - **MIDI clip menu:** Consolidate the loop.
   - **Frozen tracks:** an icy wash and a ❄ FROZEN badge, with a progress pill and ✕ while rendering. The device chain is covered with Unfreeze. Edits to a frozen track's devices are refused with "unfreeze it first".

## Decision: the macro strip and racks

1. **The macro strip** (`MacroStrip.qml`, `MacroController` as `bridge.macros`) heads every track's device chain: eight knobs in a fixed palette, each named (double-click to rename), with its value and its count of mappings. It collapses to a narrow tab. The selected macro lists its mappings, each with a range to drag at either end and a remove button.
2. **Mapping.**
   - *MAP* arms a macro (shared by every device panel through one `MacroMapState`); touching any built-in synth or effect knob then maps it instead of turning it. Escape or MAP again ends it, and a click on another macro re-arms.
   - *Right-click* on any kit knob (or the Menu key) opens "Map to Macro 1–8" and "Remove from Macro N".
   - A new mapping runs from the knob's current value to the top of its travel (to the bottom when it sits at the top), so a macro at zero leaves the sound unchanged. Mapping a knob to another macro moves it.
   - Knobs a macro drives wear its colour as a ring and a numbered badge; in MAP mode every mappable knob is outlined in the armed macro's colour. The master's effects belong to no track and cannot be mapped.
   - `ProjectSession::set_macro_mapping_range` edits a range in plain units; the parameter follows at once, and steps with one gesture coalesce.
3. **Turning live.** A drag is heard as it moves with no saved revision per step: the controller resolves each mapped device's saved dense values once, sets the mapped ones with `macro_parameter_value`, and offers them through the same live mailboxes as panel drags (`set_live_synth_values`, `set_live_effect_values`). Open panels for those devices follow through `SessionNativeSynthAdapter::show_macro_values`. The release saves the macro and every mapped parameter with `set_macro_value`, one revision and one undo step; a cancel (or a refused release) offers the saved values again. Note-effect parameters have no live mailbox and follow on release.
4. **Automation.** The arrangement's lane picker offers a "Macros" group (the track's macros) and leaves out parameters a macro drives, which may not have lanes of their own. Prepared macro lanes expand into their mapped parameters as decided above.
5. **Racks.** A rack is a user file, `$XDG_DATA_HOME/oscillate/racks/<name>.json` (kind `oscillate.rack`, version 1, at most 8 MiB), written with the project codec's own encoders (`persistence::encode_rack` / `decode_rack`).
   - It holds the built-in instrument, the note effects, the built-in effects (with their on/off state and documents) and the macros. Slots carry the rack's own ids (`rack_slot_id(1…)`, note effects first) and a synth mapping names `rack_track_id()`; links and generations are dropped. `validate_rack` checks limits, distinct ids and that every mapping names the rack's own synth or slots.
   - `capture_track_rack` leaves out VST3 and convolution effects and a sampler or VST3 instrument (the sampler is removed by [ADR-0063](0063-crate-chop-sampler.md)), and counts them; the panel's status says how many were left out.
   - `load_track_rack` is one revision and one undo step. The engines resolve every device first, so a bad rack changes nothing. The track's note effects, built-in effects and macros are replaced; the rack's instrument replaces a built-in or empty one; VST3 and convolution effects stay, after the rack's. New slots come from the project's high-water mark and the macros are retargeted to them; a lane on a synth parameter the rack's macros now drive is removed.
   - `RackLibraryController` (`bridge.racks`) saves from the strip's SAVE RACK, lists, loads, deletes, exports and imports (an import is checked, then kept). Racks appear in the browser as a "Racks" category (and so in the Ctrl+K palette), loading onto the selected track.

## Decision: the note effect engines

1. **A platform beside the synths'.** `native_dsp::note_effects` (its own library, linked by the synth engines) holds a registry of `NoteEffectEngine{descriptor, create_kernel, tail_beats}`. Each descriptor is the synths' versioned `EngineDescriptor`, so validation, sparse saving, presets, automation domains (stepped and choice parameters automate in whole steps) and the device kit's knobs are shared. `resolve_note_effect` refuses unknown engines, newer versions, unknown ids and out-of-range values, as `resolve_native_instrument` does.
2. **Kernels see project time, not blocks.** A kernel turns a block's sorted note events into new ones. Everything is timed in project ticks through the shared tempo map (`NoteClock`): a frame's tick is the map's exact conversion of its frame, and an event at tick *t* lands on the first frame whose tick reaches *t*. A step, a repeat or a late start therefore lands on the same frame under any block split, and export renders what live playback plays. Kernels allocate nothing on the callback, keep room for the note-offs of every note they sound (a crowded block drops a new note, never strands an old one), and forget everything on a new transport generation, as the instrument does.
3. **The five engines (version 1).**
   - **Arp** (`arp`): Up, Down, Up-Down, Down-Up, As Played, Random (seeded by the step's index on the song grid) and Chord; rate 1/32T to a whole note (straight, dotted, triplet); gate 5–200 %; 1–4 octaves; swing 50–75 % (every second step of a pair late); Steps (restart after *n*); Accent Every *n*; velocity as played or fixed; Latch. Steps sit on the song's grid of the rate from tick zero; the first note after silence plays at once and the grid carries on. The held notes are its input and are not heard.
   - **Chord** (`chord`): up to six semitone shifts, or a shape (triad, seventh, sus2, sus4, sixth, add9, ninth) built on scale degrees of a chosen root and scale, with inversions and a velocity for the added tones. The played note keeps its ids; the added tones end with it.
   - **Note Echo** (`echo`): 1–16 repeats at a synced time, each quieter by Decay and moved by Pitch; a repeat lasts as long as the played note. Its tail (repeats × time) extends the instrument's.
   - **Random** (`random`): a chance to move each note up to Range semitones (snapped into a chosen scale), velocity spread and up to a sixteenth late. Every choice hashes the seed, the key and the note's tick, so it repeats exactly on every pass, in export as live.
   - **Scale** (`scale`): snap to a root and scale (nearest, ties up; up; down), then move by scale steps and octaves.
   Chord, Random and Scale carry their own root and scale rather than following the song key, which is presentation only and never changes audio.
4. **In the synth player.** `NativeSynthPlayer` builds a `NoteEffectChain` from `Track::note_effects` (disabled ones too) and runs it on each block's notes (the project's and live ones) before the synth, so every built-in engine, live playback and export share it. Switching a stage off releases what it was sounding. Its values, switches and prepared lanes on its parameters (macro lanes) travel in the player's automation plan, so `RoutedPlaybackControl::try_queue_live_note_effects` offers an edit of values or switches (with a macro turned in the same edit) to the running player without a rebuild; adding, removing or moving a note effect rebuilds the source. The sampler and VST3 instruments take the project's notes as they are: the session refuses note effects on them. *(Superseded by [ADR-0063](0063-crate-chop-sampler.md): the sampler is removed; Crate is a built-in engine and takes note effects; only VST3 instruments are refused.)*
5. **Session.** `add_note_effect` and `set_note_effect_parameters` resolve through the engine and save the sparse form; `add_macro_mapping` takes a note effect parameter's domain from the engine.
6. **UI.** The device chain shows a track's MIDI effects between its macros and its synth (`NoteEffectStrip.qml`): a card per effect with an on switch, its preset with steppers, a drawing of what it does to a phrase, the device kit's knobs (main and more), move and remove, and "+ MIDI FX". `NoteEffectsController` (`bridge.noteEffects`) lists the engines, edits the chain (each edit one undo step) and gives each card a `NativeSynthDeviceController` over a `NoteEffectAdapter`, so presets, gestures and macro mapping behave as on synths. The browser has a "MIDI Effects" category; a card adds to the selected synth track, or drops on a track row.

## Decision: the rest of the programme

Built alongside, with no schema change:
- **Piano roll:** scale lock, chord stamps (with a hover ghost), strum, humanise, quantise with strength and swing, and a groove pool (built-ins, grooves learned from an audio clip's onsets, user files under `$XDG_DATA_HOME/oscillate/grooves/`). The pure logic is `application/midi_note_tools`; edits are one revision through `ProjectSession::apply_note_edit`.
- **Shell:** an undo history panel (Ctrl+H) whose labels are derived by diffing each step's snapshots (`application/history_labels`); a Ctrl+K command palette (the computer keyboard moved to Ctrl+Shift+K); a master section with momentary, short-term and integrated LUFS (four hours), true peak, spectrum, stereo correlation and mono listen (device output only, never export).
- **Browser:** synth preset phrases when a card is selected (never on hover), a sample folder previewed in the song's tempo from its next beat, a key-match offer on dropped clips, and "Duck the rest" on kick-like tracks.
- **Motion and tooltips:** `qml/shell/` holds the `Motion` timings (off offscreen and with `ui/reducedMotion`) and the one `Tips` tooltip layer; the device kit uses both.

## Consequences

- Playback and export play frozen tracks' renders (`project::audible_snapshot`).
- Note effects play into the built-in synths only; their parameters are automated through macros, not lanes of their own.
- Live playing (MIDI keyboard input, MIDI learn, a metronome) and recording are out of this programme; the HLD's "no v1 MIDI device input" still holds.
- Only 1.43 loads (greenfield).
