# ADR-0035 — Pedals in MIDI clips

**Status:** Accepted and implemented. **Date:** 19 October 2026.
**Amends:** the MIDI clip model, the project schema (the next free minor version: 1.32, since main already uses 1.31 for VST3 controller state), the compiled MIDI plan, the native synth runtime and the native sampler.

## Context

Until now, MIDI clips carried notes only. Native synths also had their bend, wheel and pressure as clip automation lanes on each engine's `*.perf.*` parameters. Those lanes are curves, sample-accurate, and travel with the clip (schema 1.30).

Pedals cannot work that way. A sustain pedal changes what a note-off *means*, so it has to travel with the notes and reach every instrument on the note timeline. A piano (ADR-0036) is unplayable without one.

Requirement: a platform change rather than a per-instrument workaround; the project is greenfield, so no legacy representation needs preserving.

## Decision

**Pedals are part of the MIDI clip.** A clip may hold up to one lane each for sustain (CC64), sostenuto (CC66) and soft/una corda (CC67).

- **Values.** A lane holds a value from 0 to 1 from each point until the next. Sustain can take half-pedal values; instruments without a damper model treat a value of 0.5 or more as down.
- **Placement.** Points sit at musical ticks inside the clip's pattern, and repeat with the pattern when it loops.
- **Clip end.** If a pedal is down when its clip ends, the compiler releases it at the clip's end, as it crops notes.

**Bend, wheel and pressure stay clip automation lanes.** They are continuous curves, and the existing lanes already do that better than stepped controller events would. A piano-roll UI should show pedal lanes and performance automation side by side.

### Pipeline

- **Model.**
  - `MidiClip::pedals` holds `MidiPedalLane{pedal, points}`.
  - Validation: one lane per pedal; positions nonnegative and strictly increasing, points at or after the pattern's end kept but silent (PI-06c); values finite and from 0 to 1; per-clip point limits. Pedal events count towards the project event limit.
  - The command `SetMidiPedalLane{clip, pedal, points}` replaces a lane, and an empty list removes it. Undo works on snapshots, as it does for every edit.
- **Persistence.** The next free schema minor version adds an optional `"pedals"` array to each MIDI clip. Main already uses 1.31 for VST3 controller state, so pedals take 1.32, or the next version free at implementation time. The implementation builds on main's 1.31 codec, so:
  - files with 1.31 controller state load unchanged;
  - 1.32 files carry both the controller state and the pedals, and round-trip both;
  - older files load with no pedals.
- **Compiled plan.**
  - `InstrumentEvents` carries pedal events in their own sorted stream, beside the note events. Consumers that know only notes, like the VST3 provider, are therefore unaffected.
  - `copy_pedal_block` copies a block's pedal events.
  - `pedal_state_at(sample)` gives every pedal's value at a sample, for chasing after a seek or loop.
  - Pedal events have their own density limit.
  - Export lengths include pedal events: releasing the pedal after the last note sets when the release starts.
- **Native synths.**
  - **Ordering.** The runtime takes a pedal span beside notes. At the same frame, pedals act before notes.
  - **Generic pedals.** Engines that don't model pedals get them from the runtime:
    - *sustain* defers note-offs while down and flushes them on release;
    - *sostenuto* defers the note-offs of the notes held when it went down;
    - *soft* scales new notes' velocity by 0.75.
  - **Modelled pedals.** Engines that do model pedals (`SynthEngine::models_pedals`) receive the values through `SynthKernel::pedal`. Concert uses this for its dampers, half-pedal and una corda.
  - **Chasing.** On a transport-generation change, the synth player sends each pedal's state at the playhead. A pedal held across a seek or loop therefore stays down, live and in export alike.
- **Native sampler.** It gets the same generic sustain and sostenuto. *(Superseded by [ADR-0063](0063-crate-chop-sampler.md): the sampler is removed; Crate, a built-in engine, gets the synths' pedals.)*
- **VST3 instruments.** When the runtime is prepared, the host asks the plugin's `IMidiMapping` which parameter it assigns to CC64, CC66 and CC67 (event bus 0, channel 0):
  - Pedal events become sample-accurate changes of those parameters, grouped with automation, and are chased after a seek.
  - Mapped parameters are accepted even when they are hidden or not automatable, as MIDI-learn targets often are.
  - A plugin without a mapping simply gets no pedal.
  - The pedal event budget is reserved only when the track's clips have pedals.

## As built (19 October 2026)

- **Schema 1.32.** Each MIDI clip has an always-present `"pedals"` array of `{pedal: "sustain"|"sostenuto"|"soft", points: [{position, value}]}`. It is decoded only from 1.32, so a 1.31 file carrying pedals is rejected. Main's 1.31 VST3 controller state round-trips beside it.
- **Plan order.** At the same sample, a clip's end release sorts before a new value, so back-to-back clips that both hold the pedal keep it down. The density limit is 256 pedal events per 8,192-sample window.
- **Generic pedals** come from `PedalLatch` (`pedal_latch.hpp`):
  - It has a fixed capacity of 256 and never allocates. When full, a note-off is delivered, never lost.
  - A note re-struck while its earlier note-off is deferred releases that earlier note first, because loop repeats reuse note IDs.
  - The native synth runtime and the native sampler both use it (the sampler is removed by ADR-0063).
- **Chasing** uses `InstrumentEvents::copy_pedals(start, frames, chase, ...)`. The synth player, both live sampler feeders (removed by ADR-0063) and the VST3 provider chase when the transport generation changes and on their first block. Offline export renders from frame 0, so it never needs to chase.
- **Tests:**
  - schema round-trip and rejection;
  - model command, validation and undo;
  - plan loops, releases, ordering, chase, cut and density;
  - latch semantics;
  - every engine sustains under any block split;
  - the release starts at the lift, and sostenuto captures only the notes that were down;
  - the player chases after a fresh start and after a seek;
  - the sampler sustains and resets on seek (removed with the sampler, ADR-0063);
  - a VST3 fixture receives CC64 at its offset, and again after a seek;
  - every engine's export with pedals nulls against live playback (largest difference 0), and the pedals change each export.

## Consequences

- **Every native instrument gains a sustain pedal and a sostenuto pedal**, with live playback and export staying identical.
- **The schema moves to the next free minor version** (1.32 on top of main's 1.31). Tests that pin the version, or that use the next version as the "future" one, are updated. Migration keeps both main's controller state and the pedal data.
- **The Quick piano roll** gains a pedal strip (`PedalStrip.qml`) wired through the project bridge. The session API provides `set_midi_pedal_lane`, `paint_pedal_span`, `remove_pedal_span` and `clear_midi_pedals`, and duplicating a clip copies its pedals.
