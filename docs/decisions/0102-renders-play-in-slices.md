# ADR-0102 — A track render plays in slices that keep its clips' channel lanes

**Status:** Accepted; implemented. **Date:** 8 November 2026.
**Supersedes in part:** [ADR-0059](0059-play-and-sparkle.md), where it renders a track's clip lanes into its render and plays a freeze or a bounce as one clip; [ADR-0099](0099-bounce-moves-mute-lanes.md), where it says a clip's mute lane is already in the render.

## Context

A clip's channel lane (level, pan or mute) takes precedence over the track's lane while the clip plays. `render_track` dropped the track's channel lanes but kept the clips' lanes, so a render already held each clip's envelope. The bounced copy, the flattened track and the frozen view then applied the track's lane over the whole render, including where the clip's envelope had overridden it. The track lane and the clip envelope were applied together ([#289](https://github.com/jcsduncan/oscillate/issues/289)). A key is tapped before the fader, so a keyed effect also heard the clip's level envelope through the render.

## Decision

- A render is wholly before the channel. `render_track` leaves out every lane that plays after the render (`project::plays_after_render`: channel controls and send gains), on the track and on its clips.
- `project::render_slices` turns a render into the clips that play it. Each clip with such lanes becomes a slice at that clip's own placement, carrying those lanes, and plain slices fill the gaps. A slice's lane evaluates exactly as the source clip's did, and it takes precedence over the track's lane in the same frames.
- **Bounce and Flatten** add the slices to the audio track, with the clip lanes' send gains following the copied sends. **The frozen view** plays a frozen track's render as its slices.
- A whole-track render covers every such clip, even where the track is silent. A range render (Bounce the loop) that cuts such a clip is refused, as are a freeze or bounce whose clips with these lanes overlap.

## Consequences

- A bounce, a flatten and a freeze sound exactly as the source did, live and in export, with clip envelopes. Their keys hear the track before its channel, as the source's did.
- A bounced or frozen track with clip envelopes plays several clips of one render.
