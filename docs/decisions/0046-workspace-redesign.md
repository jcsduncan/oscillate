# ADR-0046 — The workspace redesign

**Status:** Accepted and implemented. **Date:** 20 October 2026.
**Builds on:** ADR-0043 (one native insert path) and ADR-0042 (utilities).

## Context

The Quick shell had grown by accretion. An icon rail, a footer of engine status, a tall track list with a meter block and an add row, a 65 px arrangement header, and stand-in "input" and "output" cards in the device chain made it read as a tool for testing the engine rather than for making music. A design mockup of the arrangement, browser and mixer was drawn first; the app was rebuilt to it and checked against it screen by screen.

## Decision

### Layout
- **Top bar:** project identity and plain status, Arrange/Mixer, transport, one Loop button (the range is dragged on the ruler), Position/Tempo/Meter/Key, a master level meter, Export, and a More menu (inspector, master effects, plug-ins, media, versions, cache, shortcuts, engine details).
- **Browser:** docked beside the arrangement, with three tabs (Instruments, Effects, Utilities) and a quieter second row (Drums, Samples, VST3, Favourites).
- **Arrangement:**
  - a `TRACKS · + Track` header;
  - 62 px rows with a colour stripe, name, summary, M/S, a slim meter, and a delete control on hover or selection;
  - the master pinned below;
  - a ruler with bars and small tools only;
  - clips in their track's colour;
  - dashed placeholders in empty lanes;
  - the loop range tinted across the lanes.
- **Device chain:**
  - a header with the path's colour, name and summary, and Chain/Clip/Notes tabs;
  - strips only for real devices, with an instrument heading an instrument track;
  - preset names and live knob values on each strip;
  - reorder and remove controls on hover;
  - a drop zone at the end.
- **Mixer:** strips with one knob row (pan, then one send per return, named for the return).
- **Welcome screen:** the same system. A start panel stands where the browser is, and projects are listed as track-style rows.
- **Screenshots:** `oscillate_quick_screenshot PNG [--view arrange|mixer]` (tests/ui/quick) renders any screen for comparison with its mockup.

### Browser drags
Cards carry `BrowserCatalogue.dragPayload` under `application/x-oscillate-device`:
- on a track row or lane, an instrument replaces an instrument track's sound, and an effect joins the end of the track's chain;
- below the tracks, an instrument makes a new track;
- right-clicking a docked card offers its primary action and replacing the selected track's instrument.

### Model rules the redesign needed
- **Return buses stay at the bottom.** An `AddTrack` with no index places a return last and any other track above the first return. Creation APIs return the new track's ID, and the session reports its row, so nothing assumes the newest track is last.
- **A project key** (schema 1.36): null until the musician chooses one, then a tonic and a mode. It never affects audio. The top bar's KEY readout opens a picker that also offers the key detected in the project's audio.
- **The audible revision.** A snapshot carries the revision of its last change that could be heard:
  - names, colours, markers and the key carry it forward;
  - so do undo, redo or restore between snapshots that sound the same.
  - Playback is current when its graph was built at or after that revision, so cosmetic edits during playback never read as unheard changes.

## Consequences

- **Classic inserts are the exception in the mixer.** Mixer strips open built-in effects in place, but the classic EQ, compressor, reverb and limiter open in the arrangement's device chain until they become engines (ADR-0047).
- **The arrangement viewport is deferred.** It is submitted after layout settles; one window resize used to refresh the whole project per pixel.
