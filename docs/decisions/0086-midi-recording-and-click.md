# ADR-0086: MIDI recording, the metronome and the count-in

- **Status:** Accepted; implemented. **Date:** 3 November 2026.
- **Supersedes:** the deferral of live recording in ADR-0059

## Context

Requirement: play the computer keyboard while the song plays and see the
notes drawn into the track. The chosen design:

- **Input:** the computer keyboard. It already played the selected track's
  instrument live (`ComputerKeyboard`, `ProjectBridge::play_note`).
- **Destination:** the selected track.
- **Overdub:** notes add to what is there, and a looping clip layers each pass.
- **Click and count-in:** a metronome while playing, and a one-bar count-in
  before a take that starts from stop.

## Decision

### Where a press lands

- **Timing.** A note is placed at the project sample the player heard when
  they pressed the key. A note that sounds later, because of queued blocks
  and device delay, still lands where it was played against the song.
- **Audible points.** Each device callback publishes an
  `AudiblePoint {first transport sample, CLOCK_MONOTONIC time it is heard}`
  on the `TransportCursor`. A seqlock makes it readable from any thread.
  - PipeWire: `pw_time.now`, plus `delay` at the graph rate, plus the queued
    and resampler frames ahead of the block.
  - JACK: the callback time plus one period. Port latency is not added.
- **The press.** It reads the point and moves it on by the time since:
  `application::heard_sample()`.
  - A press past the loop end wraps into the loop, as the device does.
  - With no published point (a test output), the last completed block is
    used instead.

### Writing the take

- **The session call.** `ProjectSession::record_notes()` writes finished
  notes onto a track:
  - Each note goes into the MIDI clip whose span holds its start.
  - A looping clip folds the note into its pattern, so passes layer, and
    cuts it at the pattern's end.
  - A repeat on an existing start and key is dropped.
  - Notes in empty space go into the take's own clip. That clip starts at the
    first such note's bar and grows bar by bar, never into the next clip.
- **When notes are written.** The recorder (`RecordingController`) writes
  each note when its key is let go. The whole take uses one model gesture, so
  it is one undo step.
- **Playback.** The edit reaches running playback through the existing
  seamless edit path, so a looping clip plays a recorded note on the next
  pass.
- **Stop.** It ends the take and disarms Record. Held keys end where the
  song stopped.
- **Drawing.** Held notes draw growing in the piano roll (in clip ticks) and
  on the arrangement row (in bars) until they are written.

### Metronome and count-in

- **Placement.** `audio_io::Metronome` hangs off the transport cursor, and
  `render_transport_block()` mixes it in.
  - Only main device playback attaches one. Export and live monitoring never
    do, so the click is never in a render.
- **The click.** Every beat of a published schedule clicks, built from the
  meter and tempo maps by `application::click_schedule()`.
  - The bar's first beat is accented: 1.5 kHz rather than 1 kHz, and louder.
  - Each click is a 30 ms decaying sine, a pure function of the frames since
    its beat. Seeks, loop wraps and block splits all sound the same.
  - The schedule is double-buffered. The control thread waits, without
    locking the callback, for the reader to leave the buffer it rewrites.
- **The count-in.** It holds the transport for one bar
  (`application::count_in_at()` sizes it) and plays only its clicks. The song
  then starts where it was. It sounds even with the click off.
- **Settings.** The Click and Count-in toggles are app settings (QSettings),
  not project state, so the schema is unchanged.

### UI

- **Transport:** a Record button with a blinking dot while counting in, a
  count-in readout, and "♩ Click" and "Count-in" toggles.
- **Shortcut:** `Ctrl+R`.
- **Piano roll:** its key strip records too.

## Consequences

- No schema change.
- **Input sources.** Recording takes the computer keyboard and the piano
  roll's key strip only. Dragging or drawing notes in the piano roll plays
  them but does not record them. A hardware MIDI keyboard needs MIDI input,
  which is still not built.
- **Timing accuracy.** It is bounded by the device's report. On PipeWire it
  is within the clock's precision. On JACK, presses may land late by the
  port latency.
- **Undo.** Each note-off is a model revision. The take stays one undo step
  as long as no other edit lands in between.
