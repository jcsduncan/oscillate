# ADR-0107 — MP3 import decodes with libmpg123, scanning the whole stream

**Status:** Accepted; implemented. **Date:** 8 November 2026.
**Supersedes in part:** [ADR-0016](0016-encoded-audio-codec-boundary.md), where libsndfile decodes MP3.

## Context

An MP3 without a Xing/Info header does not state its length. libsndfile estimates it from the file size and the first frame's bitrate and stops reading at that estimate. A variable-bitrate stream whose first frames are dense imports cut short; one 2 s test file reported 0.32 s ([#255](https://github.com/jcsduncan/oscillate/issues/255)). libsndfile has no option to scan the stream, and its MP3 decoding is libmpg123.

## Decision

- The codec adapter (`src/codec`) decodes MP3 with libmpg123 1.33 or later directly, through the same pinned-file virtual I/O. It asks for 32-bit float output at the stream's own rate and channel count and calls `mpg123_scan` on open, so the length counts every MPEG frame.
- libsndfile still recognises the container, so the accepted formats do not change, and it still decodes WAV, AIFF and FLAC.
- The reported length stays the first frame that does not decode ([#253](https://github.com/jcsduncan/oscillate/issues/253)), so the length and the decoded stream agree.

## Consequences

- Headerless and variable-bitrate MP3s import, play and export at their full length. Opening an MP3 reads the whole file once.
- The adapter links `libmpg123.so.0`: the `.deb` depends on `libmpg123-0t64`, and building the codec adapter needs `libmpg123-dev`. The AppImage already bundled libmpg123 with its notice and source-offer entry, as a libsndfile dependency.
