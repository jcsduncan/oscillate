# ADR-0054 — Live controls, granular UI updates and meters on their own clock

**Status:** Accepted and implemented. **Date:** 21 October 2026.

## Context

Dragging a mixer fader while playing was slow and jittery, and the meters jumped. The cause was the shape of the UI plumbing, not the fader:

- **Every drag step was a saved project edit.** A fader, pan or send drag saved an edit about 20 times a second. Each one made an undo revision, offered the engine a new revision and called `ProjectBridge::refresh()`.
- **Every edit rebuilt everything.** `refresh()` re-projected the whole project and announced it with one `projectChanged`, which 15 bridge properties shared. The project included every clip's waveform columns, which were computed and then thrown away.
- **QML had no models.** It repeated plain JavaScript arrays, so every row, strip and clip rebound on every edit. The mixer rebuilt each strip from one array, which also read the master's true peak, so it rebuilt at meter rate during playback.
- **The cost.** On a 24-clip two-deck mix, one refresh cost 13 ms of C++. With the mixer showing, the total was 60–75 ms, more than the gap between drag steps.
- **The meters.** They were one map, refreshed about every 45 ms from the transport poll, with no ballistics, so every meter redrew together.

Earlier fixes to zoom, clip drags and waveforms had each worked around the same root.

## Decision

### Continuous controls are live; the release saves
- While a fader, pan or send knob is being dragged, its value goes straight to the running mixer through a lock-free single-producer, single-consumer mailbox on `RoutedMixerPublisher`. The calls are `push_live_node_gain` and `push_live_edge_gain`, reached through `RoutedPlaybackControl::set_live_channel` and `set_live_send`.
- **On the audio thread,** the callback drains the mailbox at the next block. The channel or send edge glides to the new gain over the mixer's usual gain ramp, with no revision, no hold and no allocation.
- **Automation wins.** A channel or send under automation keeps its automation.
- **The release saves one edit,** which is one undo step. That edit's control overlay then changes nothing audible.
- **Synth and effect knobs work the same way.** A knob drag on a built-in synth or effect offers its value set to the running instance through the shared parameter mailbox as a *live* offer (`ParameterControl::offer_live`):
  - it is heard and glides exactly like a saved offer;
  - it carries no revision, so `applied_revision()` keeps naming the last saved one;
  - it is refused for an effect whose values would change its latency, which needs the saved edit's rebuild.
- **The panel adapter holds the drag's values** (`SessionNativeSynthAdapter`). The release saves one edit; a cancel puts the saved values back through the same live path, with nothing to undo.

### Edits signal only what they changed
- **Keyed models.** `KeyedListModel` is a QML list model of rows keyed by ID. Each update is diffed: rows are inserted, removed and moved, and only the roles that changed signal. A row's fields are its `modelData`. Extra roles change on their own, without touching `modelData`.
- **Track and clip models.** `trackModel` has one row per track, and each row carries its own `clips` model. A clip row has two extra roles:
  - `waveform`: the columns of its part in view, so a zoom repaints only visible clips;
  - `tempo`: its `ClipTempoController` entry.
- **QML.** The arrangement rows and clips, the track list and the mixer strips repeat these models. A mixer strip binds only its own track row and its own insert chain. Only the master strip reads the true peak.
- **Narrow signals.** `projectChanged` means opened, renamed or reformatted. These each signal alone, and only when their value changed:
  - revision;
  - tracks;
  - markers;
  - key;
  - timeline;
  - insert chains;
  - routing;
  - master level.
- **Controllers.** The clip tempo, clip warp and arrangement automation controllers don't signal an unchanged state.
- **The rule for new work:** new UI state gets its own model or signal. Nothing new hangs off a whole-project refresh.

### Meters run on their own clock
- `MeterModel` gives each channel its own `MeterChannel` object, advanced on a 60 Hz display clock that edits and transport polling never touch. It stops once every meter has fallen silent.
- Ballistics run in C++:
  - the peak rises at once and falls at 26 dB/s;
  - the held peak waits 1.2 s, then falls at 18 dB/s;
  - RMS integrates over 300 ms;
  - a clip light stays lit for 1.5 s.
- Each channel signals alone.

## Consequences

- **Measured on the 24-clip mix:**
  - a refresh costs 2.5 ms, down from 13 ms;
  - with the mixer showing, 3–4 ms, down from 60–75 ms;
  - a fader drag step costs only a mailbox push.
- **Clip positions are still fractions of the timeline length,** so moving the timeline's end updates every clip row. Absolute bar positions would remove that and are a follow-up.
- **The raw engine meter windows stay readable from C++** (`master_meter()`, `track_meters()`) for tests.
