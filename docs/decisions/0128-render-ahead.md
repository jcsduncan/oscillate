# ADR-0128 — Render-ahead: playback rendered ahead of the device on a spare core

**Status:** Accepted and implemented. **Date:** 11 November 2026.
**Builds on:** ADR-0048 (audio helpers), ADR-0049 and ADR-0050 (live graph switching), ADR-0086 (audible points), ADR-0094 (primed source switching), ADR-0127 (the sub-block wavefront).
**Contract:** `audio_io::RenderAhead` and `render_device_block` (`include/oscillate/audio_io/render_ahead.hpp`); `StereoRenderSource::ahead`; `TransportCursor::discontinuities`; `RoutedNodeMeters::heard` and `history`, `RoutedMixer::node_meter_heard`, `RoutedMixerPublisher::active_node_meter_heard`, `RoutedPlaybackControl::heard_track_meter`; `RoutedPlayback::set_render_ahead`, `set_live_input` and `render_ahead_status`; the audio settings' Render ahead choice (`audioRenderAheadMs` in QSettings); the reference player's `--ahead-ms`, `--spike-every` and `--spike-us`. No schema change.

## Context

Every block was rendered in the callback that plays it, so any block that took longer than its period was a click: a heavy track, a device's occasional heavy hop, a page fault, a scheduler hiccup. ADR-0127 shortened the typical block; it cannot absorb a spike. Playback that nobody plays live does not have to be rendered at the last moment.

The user's rule for edits while playing: an edit may be heard after the look-ahead window, but it must never glitch, and it must take effect while playback continues, never needing Stop and Play.

## Decision

1. **A producer renders ahead; the callback copies.** `RenderAhead` wraps the render source and the transport cursor. A producer thread, started by the output at the callback's real-time priority beside the audio helpers, renders the transport ahead of the device into a ring of device-sized blocks and dispatches the helpers as the callback did. The callback copies the next blocks out. When the window is empty it renders just in time, as before. One thread renders at a time: the producer, or the callback; a callback that finds the producer mid-block waits for that block, which is the one it needs.
2. **The window.** It starts at eight device blocks, shrinks by one after 2048 blocks in which the callback never found it empty, down to two, and grows by one each time the callback finds it empty after serving the block before. It never holds more than the limit, 50 ms by default; the audio settings offer Off, 25, 50, 100 and 200 ms.
3. **Edits land at the end of the window.** The producer owns the transport while it renders: graph switches, overlays, live gains, source switches, synth plans, automation and planned loop wraps all reach the engine at the producer's position, through the engine's own click-free switching (ADR-0049, ADR-0050, ADR-0094). Nothing is invalidated or rendered again. An edit is heard at most the window plus one device block after it reaches the engine: 50 ms + one block by default (71 ms at 1024 frames and 48 kHz, 55 ms at 256). The setting's text states the bound for the current buffer.
4. **Seeks drop the window.** A seek or in-place restart (`TransportCursor::discontinuities`) makes the producer render from the new position at once and the callback skip what was rendered before it. A paused seek clears the window. A pause keeps it, so resuming plays on exactly. A source that holds its block (priming after a seek) is rendered just in time until it lets go.
5. **Live input renders just in time.** While the computer keyboard is on or recording is armed, nothing renders ahead: the window drains and the callback renders every block, so keys play at once. The master depends on the played track, so the whole graph renders just in time; with ADR-0127 that is today's cost.
6. **What is heard is what is shown.** The playhead is the device's position (`RenderAhead::audible_position`), not the cursor's rendered one. Track meters keep their last 32 windows and show the newest one the ear has reached (`RoutedNodeMeters::heard`). The master meter, spectrum, true-peak monitor and the mono check observe the blocks the device plays (`RenderAhead::Tap`). Recording still places notes by the audible point, which the callback publishes for each block it plays.
7. **Stop fades.** With a window, Stop fades the device's output over 256 frames and then renders nothing more, instead of draining the graph behind the window.

## Measured

i7-10700K (8 cores, 16 threads), GCC 15.2, Release, seven helpers, paced fake callback, 48 kHz, 20 s a run, with ADR-0127's wavefront. The heavy reference with the heavy master; callback p99, blocks over their period, whole-process CPU (% of one core):

| Frames | Just in time: p99, late, CPU | 50 ms window: p99, late, CPU |
| ---: | ---: | ---: |
| 256 | 2230 µs, 0, 190 | 12 µs, 0, 201 |
| 512 | 4287 µs, 0, 205 | 14 µs, 0, 202 |
| 1024 | 7388 µs, 0, 196 | 16 µs, 0, 195 |

With a load spike, every 200th render 25 ms slower (`--spike-every 200 --spike-us 25000`):

| Frames | Just in time: p99, late | 50 ms window: p99, late |
| ---: | ---: | ---: |
| 256 | 2505 µs, 19 | 11 µs, 0 |
| 512 | 4874 µs, 9 | 4 µs, 0 |
| 1024 | 8959 µs, 5 | 7 µs, 0 |

The light reference with the heavy master: p99 821 and 2314 µs at 256 and 1024 frames just in time, 11 and 12 µs with the window, at 36 → 34% and 31 → 30% of a core. The producer does the same rendering the callback did; its waits cost no measurable CPU.

**Edit to hearing.** Just in time, an edit reaches the next block, heard one block later (5 ms at 256 frames, 21 ms at 1024). With the 50 ms window it is heard at most the window plus one block later: the window holds whole blocks, eight at 256 frames (43 ms) and two at 1024 (43 ms), so 48 ms and 64 ms; the setting's text quotes the limit plus one block (55 and 71 ms). Off keeps today's immediacy.

## Consequences

- **Edits wait for the window.** Default 50 ms plus a block; Off makes every edit immediate and gives up the protection.
- **A spare core.** The producer is one more real-time thread; while the window is full it sleeps until the next device block.
- **Device panels lead slightly.** A built-in device's own telemetry (Lens, a compressor's reduction, a synth's scope) is produced as it renders, up to the window ahead of what is heard. Track and master meters follow the ear.
- **A routing change of a track's main output while playing** is refused by the engine as before (`route ID cannot be reused`), with or without render-ahead.
