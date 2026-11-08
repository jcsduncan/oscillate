# ADR-0099 — A bounce moves its source's mute lanes to the bounce

**Status:** Accepted; implemented. **Date:** 8 November 2026.
**Supersedes in part:** [ADR-0097](0097-mute-lanes-gate-keys.md) and [ADR-0059](0059-play-and-sparkle.md), where they say a bounced source keeps its mute lane and its keys and the bounced copy takes no keys.

## Context

Bounce into a new track mutes the source and gives the copy the source's channel lanes. A channel-mute lane drives its track's mute in place of the saved mute (ADR-0097), so a source with a mute lane ignored the mute the bounce saved. The source and the copy both played under the same lane, and the track sounded twice ([#280](https://github.com/jcsduncan/oscillate/issues/280)).

The saved mute could instead win over a lane. A lane takes its default from the saved mute, so a track muted before its lane was drawn would then stay silent whatever the lane says.

## Decision

- **Bounce** (`new_track`) removes every channel-mute lane from the source, on the track and on its clips, in the same undo step that mutes it. The saved mute then silences the source, live and in export, and a muted source keys nothing.
- The copy takes the source's track mute lane with its other channel lanes, and all of the source's sends, sidechain keys included. Its keys follow its lane as ADR-0097 describes. A clip's mute lane is already in the render, which is taken before the channel.
- **Flatten** is unchanged: the copy replaces the source.

## Consequences

- A bounced track sounds and keys once, under its lane, from the copy.
- Unmuting a bounced source plays it without its mute lanes. Undoing the bounce restores them.
