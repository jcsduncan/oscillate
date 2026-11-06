# Processing: warp maps, processed sources and the warp render cache

`PreparedWarpMap` is the shared integer-frame segment plan for time-stretch
workers and offline rendering. It requires explicit clip endpoints
and strictly increasing source/output anchors, then maps boundaries in either
direction without cumulative floating-point drift. A segment is bounded to
2^31 frames and a clip to 4,096 anchors; a processor that cannot render a
requested ratio must reject it separately. This map does not change pitch or
render audio. Keep its output-frame endpoints authoritative when connecting a
worker or cache result to the playback ring.

`prepare_clip_frame_plan` resolves a saved clip's source range and placement
into those frame endpoints. Reviewed marker targets are clip-local offsets in
the clip's timebase, so a clip move retains their shape; musical offsets use
the shared tempo map. The default plan reads the clip's saved reviewed markers.

`warp_render_cache` resolves a clip's saved warp request, keys and caches
the per-source warp analysis, and creates the shared warp stream every path
renders through. Its LDW2 rendered-payload path (`render_warp_to_cache`,
`WarpRenderCacheReader`, `fill_from_warp_render_cache`) streams a render into
the project cache in bounded chunks and reads it back fully verified; the
app does not use it yet (live playback renders through `warp_live_reader`).

`warp_stream_map` adapts that frame plan to a `warp_dsp` time map in the
project-rate view of the source. `verified_source_view` reads a managed
source through project verification for workers. `warp_live_reader` is the
prefetch worker's source of a processed clip's frames for live playback, and
`offline_warp_clip` renders one processed clip frame-exactly for export from
the managed source alone. `clay_source` renders a clip's Clay note edits into
a derived source that playback, warp and export then read in its place
(ADR-0081). None of these run on the audio callback.

Consumers link `oscillate::processing`, `oscillate::warp_stream_map`,
`oscillate::verified_source_view`, `oscillate::clay_source` or
`oscillate::warp_render_cache`.
