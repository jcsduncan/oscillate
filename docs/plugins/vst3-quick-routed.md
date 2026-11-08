# Qt Quick routed VST3 playback slice

**Scope:** PI-03/04/08, one saved effect insert on one audio track. This is a
control-side connection to the existing verified routed graph node, not a
plugin browser, editor or compatibility claim.

When built with `OSCILLATE_BUILD_VST3_SCANNER=ON`, Quick's routed preparation
worker rescans the saved bundle in the helper process. It compares the fresh
SHA-256 with the saved binary identity, then hands the receipt to
`Vst3TrackInsertProvider`. The provider checks class identity, binary receipt
and component state, prepares the runtime outside the callback and gives it to
`RoutedPlaybackControl`. Quick owns the provider until the control is retired.
The helper is looked up only beside the executable. Local VST3 builds copy it
there; a missing packaged helper fails visibly. Distribution packaging must
ship the helper next to the app and recheck its sandbox/permission policy.

The initial VST3 graph is bounded to 128 frames so a requested 64- or
128-frame device block fits the prepared host. A larger observed block stops
the Quick run with a format/block-limit explanation. A failed scan, changed
binary, rejected state or missing provider prevents playback publication and
keeps the saved project intact. A processing fault mutes the affected track;
Quick shows that fault separately from the active graph revision. Builds with
the scanner option off reject saved inserts visibly. Any saved insert selects
the routed path, even when no bus, send or limiter is present.

The fake-output contract saves and reopens a project with the synthetic gain
effect, then checks its audible first block at both requested buffer sizes. It
also checks missing/changed/class-mismatched inserts, a processing fault and
the scanner-off path. The existing routed controller fixture covers runtime
reuse through compatible live graph edits and callback allocation. Quick
currently applies later project edits by Stop/Play, so it does not yet publish
live plugin graph edits. Physical PipeWire/plugin compatibility and offline
VST3 export remain separate qualification work.
