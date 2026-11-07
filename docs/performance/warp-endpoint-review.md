# Warp endpoint and marker review

**Status:** Experimental worker candidate, 16 October 2026. Production playback
and export remain disabled for processed clips. This review covers the pinned
Signalsmith Stretch 1.4.0 `exact()` path at 48 kHz, zero pitch shift and two
channels. It does not approve the algorithm or its redistribution terms.

## Endpoint probe

A 48,000-frame source contained a 0.9-amplitude left-channel impulse only in
its final frame. The requested output duration varied; the peak offset below
is relative to the last requested output frame. A second run appended 4,096
zero source frames and extended the processor output in the same ratio, then
examined the original requested boundary. Both runs used the same processor
seed and default preset. The temporary probe was compiled with GCC 15.2.0,
`-O2`, and the pinned local Signalsmith Stretch/Linear headers. It lives in the
worktree's ignored `build/dev/scratch/warp_probe.cpp`.

| Output/source ratio | `exact()` peak offset | Padded peak offset | Consequence of trimming padded output |
| ---: | ---: | ---: | --- |
| 0.5× | 0 frames | +1 frame | Peak falls outside requested clip |
| 0.75× | −21 frames | +22 frames | Peak falls outside requested clip |
| 1.25× | −22 frames | −22 frames | Peak remains displaced |
| 2× | −1 frame | −1 frame | Peak remains displaced |
| 3× | −20 frames | −20 frames | Peak remains displaced |

The sign and size of displacement depend on the stretch ratio and the
processor's internal block phase. `exact()` returns the requested number of
frames, but that alone does not establish audible endpoint correctness.
Padding and cropping are therefore not a safe local correction: they can
discard the attack. Likewise, moving the rendered waveform to align one peak
would change the start and reviewed interior-marker mapping.

## Marker seam probe

Two 48,000-frame adjacent segments were processed independently, as the
current candidate does. The source was a continuous 0.3-amplitude sine at
110, 220, 440 or 997 Hz and three start phases. The first segment's output
duration was 24,000, 36,000, 48,000, 60,000 or 96,000 frames; the second
segment's output duration was 48,000 frames. Across these 60 cases, the
largest adjacent-sample source jump at the marker was 0.0390, while the
largest output jump was 0.3554. For 220 Hz, phase 1.3 radians and a 24,000
frame first output, the source jump was 0.00243 and the output seam jump was
0.35403. This is direct evidence that resetting `exact()` at every marker
can introduce a discontinuity in continuous material. The new contract test
checks that the reported `max_seam_jump` equals the actual rendered adjacent
sample jump for that signal; it does not encode the bad jump as an acceptable
quality threshold.

An adjacent-sample difference is only a diagnostic. Listening, short-window
energy, phase continuity and percussive transients need separate qualification.
The measured case is still sufficient to reject the current independent
segment renderer for production.

## Required design before activation

1. Define a marker-spanning worker renderer with one processor lifetime across
   adjacent segments. Schedule the input and output spans around each reviewed
   anchor while accounting for `inputLatency()` and `outputLatency()`; do not
   restart `exact()` at each marker. Specify exact requested output frame
   count and bounded source/output lookahead. Keep cancellation and private
   cache publication semantics.
2. Define endpoint policy using explicit processor drain and a measured tail
   region. A clip render may not silently discard material that crosses its
   requested boundary. If an algorithm cannot satisfy the chosen endpoint
   contract at a given ratio or source length, return an actionable unsupported
   result instead of adding silence detection or an attack-position heuristic.
3. Validate source and output anchors with impulses on both sides of each
   marker, continuous tones, near-end attacks, percussion, vocals and mixed
   music. Include trim and loop joins. Measure timing, seam energy, pitch,
   duration, CPU and memory at relevant sample rates and quality modes.
4. Version the algorithm/cache key if the rendering or endpoint policy changes.
   Do not treat existing experimental cache entries as production audio.

No candidate code or cache format changed in this review. Existing bounded
stream I/O, cancellation and all-or-nothing cache publication remain in force.
