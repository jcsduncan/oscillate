# Waveform summary core (PA-01 slice)

This worker-only component scans a verified `media::AudioReader` once and returns
per-channel minimum, maximum and RMS values over half-open source-frame ranges.
The caller supplies a source cache key (at most 128 bytes) and monotonically
increasing job generation. Both are echoed in the result and **must be checked
again** by the caller before installing it. A cancellation check runs before
each 4,096-frame read and once before result publication. Cancellation or a
decoder/read error returns no partial summary.

The first level uses at least the requested number of frames per bucket. If the
requested resolutions would exceed the total bucket budget, it doubles the
first-level width until all levels fit. Each following level covers four times
as many source frames, capped at the source length. Partial final buckets keep
their exact frame range; aggregate RMS weights them by actual frame count.
Defaults are 256 source frames per initial bucket, 250,000 buckets total and
six levels. Hard limits are one million buckets and 16 levels. Decoder scratch
is a fixed 4,096 stereo frames. Result memory is proportional to the bounded
bucket count; it never grows with source duration beyond that cap. No I/O or
analysis happens on the audio callback.

This is waveform data only. BPM, key and onset suggestions have separate
analyzers. The summary is disposable derived data; source media and user
corrections are untouched. The shared source-analysis cache uses a versioned
semantic key and validates payload lengths, frame ranges and numeric values on
read. Stale worker generations cannot publish into the current project view.

The Quick application requests a finer bounded pyramid: 64 frames per initial
bucket, 250,000 buckets total, and eight levels. For long recordings the
analyzer widens the base bucket only enough to fit that budget. The options are
part of the shared source-analysis cache key, so older coarse 4,096-bucket
entries are not reused. The arrangement projects only the visible source
interval into screen columns and draws a continuous peak envelope; zooming in
selects finer source buckets without loading raw PCM on the UI thread.

The cache panel reports total usage across registered project caches and the
shared source-analysis cache, with shared usage shown as a breakdown. The
global budget applies oldest-first eviction across both stores; the shared
store also retains a 2 GiB safety limit for its own writes. Clear Project
leaves shared analysis intact; Clear All removes reconstructable entries from
both. If the shared store is unavailable, the panel reports this and refuses
to apply a budget that it cannot enforce globally.
