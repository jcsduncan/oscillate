# Managed encoded source staging

The optional `OSCILLATE_BUILD_CODEC_ADAPTER=ON` build links the project store to
the worker-only codec adapter (libsndfile, and libmpg123 for MP3). `begin_encoded_stage` duplicates the caller's
readable descriptor and the store's directory capabilities. Its move-only task
copies the original bytes to a private `.stage-` file, checks that the input
did not change during copying, flushes the staged file, hashes the bytes and
uses the pinned staged descriptor to detect AIFF, FLAC or MP3 and inspect
sample rate, channels and frame count. The input filename is never used as a
format authority. Cancellation or failure removes the private stage.

Only the originating writer can publish the prepared asset. Publication checks
the staged inode and file version, retains a verified existing digest asset
when deduplicating, links a new asset as `<sha256>.<aiff|flac|mp3>`, flushes the
assets directory and then issues a store-bound receipt. A project save still
requires that receipt or an identical record from a committed generation.
`open_managed_source` checks the managed filename, no-follow regular-file type,
length, SHA-256, detected format and decoded media metadata before returning a
caller-owned pinned descriptor. The existing narrow WAV staging and verification
path remains in use for WAV sources. An MP3's frame count is its decodable
length: libmpg123 scans every frame on open, since a stream without a Xing/Info
header does not state its length, and the reader then checks where decoding
really ends.

The default build remains WAV-only and returns `unsupported` for encoded
staging and opening. With the optional codec adapter enabled, playback
preparation opens the verified managed descriptor, checks the source record
against decoder metadata, and hands a move-only reader to the prefetch worker.
The worker decodes into reusable bounded scratch buffers; the graph callback
consumes only tagged frames from the existing ring. Clip placement, seek
generation, priming and underrun semantics are identical to WAV. Source and
project sample rates must match; no implicit conversion occurs. The app's
encoded import flow, resampling and encoded export remain separate work.
The fixture suites cover AIFF, FLAC and MP3 save/reopen, duplicate bytes, cancellation,
malformed and unsupported media, path replacement, staged-byte tampering,
managed-asset tampering, symlink replacement, placement, seek/re-prime and
steady playback underruns.
