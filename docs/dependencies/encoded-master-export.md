# Encoded offline master output slice

The optional codec build exposes `render_encoded_master` for FLAC 16-bit PCM,
FLAC 24-bit PCM and MP3. It reuses the offline master mixer, source verification,
timing, native processing/tails and live-revision or stopped-transport checks used
by WAV. The existing `render_wav_master` contract and WAV writer remain unchanged.
The API requires an explicit encoded format and matching `.flac` or `.mp3` target;
this slice does not yet wire those choices into the desktop export panel.

The export worker writes to an exclusive temporary file in the destination
directory. After libsndfile closes the encoder, it reopens the file to verify
format, channel count, sample rate, declared frame count and readable first/last
frames; only then does it sync and atomically rename over the target. A failed
render removes the temporary file and keeps any prior target. Directory-sync
failure after rename reports `durability_uncertain`, as with WAV.

FLAC input to libsndfile is integer PCM. Reduction to 16 or 24 bits uses the
same seeded TPDF quantizer as the WAV writer, so a repeated render with the
same snapshot, seed and library build is byte-identical. FLAC supports project
rates 44.1, 48, 88.2 and 96 kHz. MP3 supports 44.1 and 48 kHz in this slice;
its normalized float input is encoded using libsndfile's MPEG Layer III path.
`compression_level` is passed to libsndfile in the inclusive range 0–1. The
actual bitrate is encoder selected and not promised by this API. A repeated
render is byte-identical with the pinned local libsndfile/LAME runtime, but
lossy output has an audio tolerance rather than sample-exact fidelity.
Sample-rate conversion and stem routing are outside this slice.

The local Ubuntu 26.04 runtime is libsndfile 1.2.2-4. The installed API and
runtime were probed for FLAC 16/24 and MPEG Layer III writes before enabling
the paths. Dynamic libsndfile linking and the transitive FLAC/LAME encoder
binaries need exact package pinning and licence notices in the release SBOM;
see [ADR 0016](../decisions/0016-encoded-audio-codec-boundary.md). This is an
implementation gate for packaging, not evidence that distribution review is
complete.

Focused tests independently decode outputs, check format/rate/channel/frame
metadata, FLAC sample placement/dither tolerances, MP3 signal error, repeat
renders, stale revisions, unresolved media, interrupted writes, temporary-file
cleanup and unchanged previous targets.
