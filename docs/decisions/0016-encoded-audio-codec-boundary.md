# ADR-0016 — Encoded audio codec boundary

**Status:** Accepted for a worker-only decoder slice; export encoding and resampling remain separate measured decisions.
**Date:** 14 October 2026
**Requirements:** FS-03 PM-01/PM-08/PM-09; PRD §§8, 17, 21.

## Context

The first audible slice reads a bounded subset of PCM WAV directly. The v1 import contract also requires AIFF, FLAC and MP3, while preserving each original encoded file. Extending the project’s WAV parser into several unrelated container and codec implementations would duplicate mature libraries and enlarge the malformed-input surface.

## Decision

Use dynamically linked libsndfile 1.2.2 or later for **off-callback decoding and metadata inspection** of WAV, AIFF, FLAC and MP3. The Ubuntu 26.04 package available on the reference development machine is `libsndfile1` 1.2.2-4. Upstream lists those formats and says MP3 read/write support began in 1.1.0. The library is LGPL 2.1 or 3; its Ubuntu runtime has codec dependencies including FLAC, mpg123 and LAME. Distribution must ship the applicable licence notices and permit replacement of the dynamically linked libraries. Pin and inventory the exact distributed binaries and transitive packages at packaging time.

The codec adapter owns a pinned regular-file descriptor and gives libsndfile virtual I/O backed by `pread`, with an independent bounded cursor. It validates container type, channel count, rate, frame count and decoded output before a source record is committed. Import stores the original bytes and a stable hash; no decoder may rewrite them. Decoding and seeking occur in a media or export worker, never the audio callback or UI thread. A project-format revision will add encoding tags before non-WAV imports are enabled. Existing WAV records and source hashes remain readable.

The existing narrow WAV reader remains a reference/compatibility path until the new adapter has independent AIFF/FLAC/MP3 fixtures, corruption and cancellation tests, and measured worker throughput. The source-rate converter is a separate stage and choice; the BSD-2-Clause libsamplerate candidate requires quality and CPU measurements against the required 44.1–96 kHz rates. FLAC/MP3 **export** also requires format, bit-depth/quality, tail, publication and repeat-render tests before adopting an encoder path. MP3's lossy output will use an explicitly stated consistency tolerance.

## Alternatives considered

- Extend the custom WAV parser to AIFF, FLAC and MP3: excessive codec and malformed-input maintenance cost.
- Use a single-header bundled decoder: package provenance and individual format support vary; it does not remove the source-format and rate-conversion contracts.
- Decode every import into a replacement WAV: violates the requirement to preserve original encoded source media.

## Evidence and open gates

The [upstream format table](https://libsndfile.github.io/libsndfile/formats.html) lists WAV, AIFF and FLAC and documents MP3 support since 1.1.0. The [upstream licence statement](https://libsndfile.github.io/libsndfile/) describes LGPL dynamic-link obligations. The [libsamplerate project](https://github.com/libsndfile/libsamplerate) identifies its BSD-2-Clause terms. The Ubuntu package dependency list was inspected locally. This decision authorizes a codec adapter and test fixtures, not a claim that the full import/export workflow or redistribution review is complete.

The adapter is built in the explicit `codec` CMake preset once `libsndfile1-dev`
is available. Its AIFF, FLAC, MP3, WAV and malformed-input fixtures pass local
GCC, Clang, ASan/UBSan and clang-tidy checks. The focused source line coverage
is 129/142 (90%). Linking still requires a release SBOM and bundled-license
review; this opt-in slice is not yet connected to project imports.

Manifest schema 1.6 records the canonical source encoding tag (`wav`, `aiff`,
`flac`, `mp3`) alongside the existing stable source ID, encoded-byte SHA-256,
byte length and decoded media metadata. Schema 1.0–1.5 remains readable;
1.1–1.5 source records continue to accept only `wav`. Unknown tags and malformed
metadata are rejected, with no format guessing. This is a project-model and
manifest contract. Managed staging, asset verification, playback preparation
and export still accept WAV only until the decoder-backed paths are connected.
