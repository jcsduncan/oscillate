# Analysis cache payload v1/v2

`analysis::encode_waveform_cache` and `encode_tempo_cache` serialize disposable
PA-01 worker results for `media::CacheStore`. The format is canonical little
endian and independent of compiler struct layout. It does not alter managed
source bytes, project state, manual tempo edits, or user warp markers. The
caller supplies the `media::CacheKey::hex()` for the specific analyzer version
and canonical options as `source_key`; using only the source SHA would make
different algorithms or settings collide. The short keys in unit fixtures are
for readability. The project-local cache store owns atomic file publication,
SHA-256 file verification, eviction and its 64 MiB payload cap.

The payload begins with `LDANLYS1`, a `uint16` version (waveform 1, tempo 2), one-byte kind
(waveform 1, tempo 2), a zero flag byte, `uint32` total length, `uint16` key
length, zero `uint16`, zero `uint64` reserved slot, `uint32` sample rate,
`uint32` channel count, `uint64` source frames, then the exact key bytes.
Waveform data follows as `uint32` level count and, for each level, `uint64`
frames per bucket, `uint32` bucket count, then contiguous `uint64` begin/end
frame pairs and `float32` minimum/maximum plus `float64` RMS for each active
channel. Tempo data follows as `float64` BPM/confidence, a canonical byte
truncation flag, `uint32` beat count, and ascending `uint64` source-frame beat
positions, then a canonical onset-truncation byte, `uint32` onset count and
ascending `uint64` source-frame onset positions. Tempo v1 entries are rejected
and rebuilt so a cache hit cannot silently omit the newly exposed onsets.
The last eight bytes are an FNV-1a checksum of all previous bytes.
This inner checksum catches accidental mutation when the codec is used outside
CacheStore; it is not an authentication mechanism.

The producer validates every field before encoding. The reader checks magic,
version, type, exact length, checksum, source key, source metadata, limits and
record structure before trusting counts for allocation. It rejects non-finite
or invalid numeric data, overlaps/gaps in waveform buckets, unordered/out of
range beats or onsets and trailing bytes. Waveform results are capped at 16 levels and
one million total buckets; beat and onset suggestions are each capped at 50,000. Encoders reject a
waveform that exceeds the cache store's 64 MiB payload budget. Decoders take a
bounded byte span, allocate only after checking the relevant record length,
and do not create a second full payload copy.

Job generation is **not** persisted. It is transient: a valid semantic cache
entry must survive reopen and a new worker request. The caller passes its
current nonzero generation to the decoder, which stamps the returned result.
Before installing the result, the control side must still compare the current
cache key, generation, source metadata and cancellation state against its
ticket. A mismatch is discarded as stale. A corrupt or unknown payload is a
cache miss to regenerate, never a reason to change project state.
