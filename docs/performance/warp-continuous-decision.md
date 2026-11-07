# Continuous warp processor decision (17 October 2026)

Status: rejected for the optional worker candidate. Production playback and
export remain gated. The current renderer uses independent `exact()` calls for
reviewed marker intervals and reports the adjacent-sample jump at each seam.

Signalsmith Stretch 1.4.0 has no public operation that both retains the
processor's spectral history across a rate change and binds an arbitrary
source transient to an exact output frame. `exact()` calls `outputSeek()`,
which resets the processor, for every interval. `process()` retains history,
but requires latency-aware input scheduling. The earlier 48 kHz continuous
probes in [warp-timing-spike.md](warp-timing-spike.md) moved marker impulses by
up to 704 frames with fixed cropping and up to 457 frames in a latency-aware
short-segment probe. A smoother measured seam is insufficient when a reviewed
marker moves.

The bounded fallback is explicit rejection of any segment outside the
candidate's 0.5–3× interval range. The streaming worker now checks *every*
segment's structural range before its first source read or output write. A
later unsupported interval therefore cannot expose a successful earlier
interval to a caller's streaming sink. The whole-buffer path already renders
to private pending storage and publishes only after all intervals succeed.
Content-dependent failure, cancellation or a sink error during a streaming
render still may leave partial bytes in the temporary cache writer; the cache
transaction must discard them and never install the entry. This check does
not improve seam quality or approve the algorithm for production.

Independent focused tests cover exact marker impulse placement at 0.5, 1, 2
and 3×, no pre-marker impulse, an identity map's first and last frames, a
tone seam measured from the actual adjacent output samples, and zero reads or
writes for an unsupported *later* interval. The existing long streamed-vs-span
fixture checks equal sample output and seam diagnostics across bounded I/O
chunks. The endpoint impulse and listening-quality gaps measured on the segment
candidate (since removed) remained open.

Next algorithm evaluation needs a marker-aware transient contract with
sample-accurate onset and endpoint tests before seam or CPU comparisons. It
must retain deterministic results across worker partitioning and define a
clip-edge tail policy. No cache key/version changes are needed because accepted
audio bytes are unchanged.

One candidate for a separate offline prototype is Rubber Band's
[`setKeyFrameMap`](https://github.com/breakfastquay/rubberband/blob/default/rubberband/RubberBandStretcher.h):
its API accepts source-to-output frame mappings in offline mode, and its
offline path compensates start and duration. This is an API fit to measure,
not evidence that transients land exactly or seams sound acceptable. Its
[published licensing](https://breakfastquay.com/rubberband/license.html) is
GPL-2.0-or-later or a paid commercial licence, so it is not an approved
production dependency under the current policy. Compare an isolated prototype
against the existing MIT candidate before any dependency decision.
