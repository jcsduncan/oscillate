# Source-rate worker and managed-audio integration

`media::SourceRateReader` owns a worker-only `AudioReader` opened from a verified managed descriptor. It accepts mono or stereo sources from 8 to 384 kHz and project rates of 44.1, 48, 88.2 or 96 kHz, subject to the bounded rational phase-period check below. `AudioReader` duplicates mono to stereo before conversion; no multichannel downmix policy is implied. The original source remains unchanged. The public `source_rate_frame_count` helper returns the nearest project-frame duration with checked integer arithmetic; it is available even in converter-disabled builds so import, playback and export use one duration rule. The adapter accepts at most 1,024 requested frames per call. Reads at or beyond the rounded endpoint return zero frames. Same-rate reads use `AudioReader` directly, including when the optional converter is disabled; different-rate requests fail explicitly in that build.

Each random read starts at a source-frame anchor on the exact rational source/project grid. The anchor is at least 256 source frames before the requested position where the source permits it. It reads a bounded window with 256 source frames of postroll, using zeros beyond EOF, resets one prepared `SRC_SINC_MEDIUM_QUALITY` converter and returns only the requested project frames. This avoids accumulated phase drift and makes a seek independent of previous requests. To keep this window bounded, `open` rejects a mismatched rate pair when either reduced grid period (`source_rate/gcd` or `project_rate/gcd`) exceeds 4,096 frames. Common 8/44.1/48/88.2/96/384 kHz pairs pass; a coprime 48,001 to 44,100 Hz pair is explicitly unsupported. The worker owns fixed 16,384-frame stereo decode, input and output buffers (about 384 KiB total), plus libsamplerate state. It does no allocation on a read in our code. A stop token is checked before source I/O, after source I/O and after conversion. A codec or libsamplerate call already in progress is not interruptible; the 1,024-frame request bound limits conversion work, but compressed-source seek latency still needs measurement.

Fixtures cover all 24 combinations of source rates 8/44.1/48/88.2/96/384 kHz and the four project rates, duration rounding, repeat seeks, 73-frame versus 1,024-frame chunking, independent 440 Hz sine timing, mono impulse placement, stereo channel separation, EOF and cancellation. Separate one-, three- and seventeen-frame sources verify that missing preroll/postroll at file boundaries is replaced by silence, with no read beyond the source or dropped rounded endpoint. The current consistency tolerance is 3e-5 per float sample across seek/chunk boundaries. These are source-preparation fixtures, not audible playback qualification.

Local preliminary GCC 15.2 runs on an Intel i7-10700K used a five-second 48 to 44.1 kHz stereo sine, five passes for each request size. **Release** medians were 0.559 seconds at 64 frames, 0.304 seconds at 128 frames and 0.087 seconds at 1,024 frames (about 8.9, 16 and 57 times real time respectively); process `VmHWM` was 5.6 MiB. **Debug** medians were 0.747, 0.418 and 0.118 seconds respectively, with 5.6 MiB `VmHWM`. This includes the temporary WAV reader and benchmark process. The benchmark target is `oscillate_source_rate_bench`. The small-request cost makes decode-ahead batching a necessary integration decision for larger track counts. Long compressed-source seek, resampler quality/alias sweeps, per-clip concurrency and reference-hardware headroom remain to be qualified.

## Integrated contract

- `source_rate_supported` checks converter availability and the bounded phase period before a staged import publishes an asset. ProjectSession retains the original source-frame range and places a new clip for `source_rate_frame_count(source.frames, source.rate, project.rate)` project frames. Rounded zero duration and overflow are rejected.
- Routed playback and offline master/stem export convert each source-range endpoint independently with the same helper. The difference must equal the rendered clip duration. Worker reads begin at the converted absolute source-range start plus elapsed clip frames. Stretch and warp remain explicit unsupported cases; current trim commands also reject mismatched-rate clips.
- The routed worker primes and refills rings in at most 1,024-frame reads. The callback consumes only tagged project-rate frames. A seek changes transport generation and re-primes on the absolute project-source grid; stale ring output is discarded. Offline master and stems use the same bounded reader.
- The original encoded source remains the managed asset. No converted cache is persisted. Source or clip edits require a new prepared routed source and graph handoff. A codec call already in progress is not interruptible, and multi-track conversion headroom still needs qualification.

End-to-end fixtures cover 44.1→48 kHz import/save/reopen, routed impulse seek and exact clip end, offline sine pitch/timing, master/stem equality, repeat rendering, converter-disabled rejection and stretched-clip rejection. The optional converter remains disabled by default. A Qt Quick offscreen fake-device fixture also imports a 44.1 kHz WAV into a 48 kHz project with no buses and verifies audible routed playback, final-output metering and stop. The Release batch timings above measure the primitive, not an integrated 32-track workload.

The integration branch reran the Release primitive on the same machine while other builds were active: five seconds of 48→44.1 kHz stereo conversion took median 0.626, 0.320 and 0.104 seconds at 64-, 128- and 1,024-frame requests, with 5.6 MiB process high-water RSS. Converter-ON and OFF focused import/playback/export tests passed; converter-ON ASan/UBSan and TSan passed. Focused line coverage for `SourceRateReader` was 112/121 (92%); the four touched production files combined reached 1,530/1,794 (85%), including existing failure branches in ProjectSession, routed source and offline export. This evidence does not establish 32-track headroom or a real-device underrun threshold.

## Long compressed-source seek qualification

The opt-in [compressed seek rehearsal](../../tests/qualification/qualify_compressed_seek.py) generates separate ten-minute, stereo, 48 kHz FLAC and 192 kb/s MP3 sources outside Git. It checks 1,024-frame reads at quarter, half, three-quarter and near-end positions against one sequential decode, with 30 shuffled passes. It also checks immediate cancellation, the 1,024-frame request bound, subsequent reader usability, and 48→44.1 kHz conversion consistency across 1,024- versus 64-frame partitions. The sources are deterministic, distinct multi-tone signals in each channel. The sequential decode is an independent *access pattern* through the same libsndfile decoder, not an independent codec implementation.

Local Release run, 15 October 2026: Intel Core i7-10700K (8 cores/16 threads), 32 GB RAM, GCC 15.2.0, Linux 7.0.0-38, libsndfile 1.2.2, libsamplerate 0.2.2, FFmpeg 8.0.1. The generated fixtures were on `/tmp` (tmpfs), and the oracle scan warms the page cache first. Timings include the worker reader and codec, but exclude project asset verification, scheduler dispatch, ring fill, and audio callback. “Decoder-cold” below reopens the decoder for each read; it does **not** mean cold storage.

| Source and 1,024-frame operation | Calls | Median | p95 | Maximum |
| --- | ---: | ---: | ---: | ---: |
| FLAC decoder-cold open + start prime | 30 | 0.096 ms | 0.191 ms | 0.280 ms |
| FLAC random same-rate seek | 90 | 0.138 ms | 0.177 ms | 0.204 ms |
| FLAC near-end same-rate seek | 30 | 0.074 ms | 0.083 ms | 0.091 ms |
| FLAC random 48→44.1 kHz seek | 150 | 0.510 ms | 0.644 ms | 0.789 ms |
| MP3 decoder-cold open + start prime | 30 | 0.053 ms | 0.147 ms | 0.196 ms |
| MP3 random same-rate seek | 90 | 0.064 ms | 0.084 ms | 3.708 ms |
| MP3 near-end same-rate seek | 30 | 0.054 ms | 0.065 ms | 3.651 ms |
| MP3 random 48→44.1 kHz seek | 150 | 0.486 ms | 0.566 ms | 4.269 ms |

Both codecs matched the sequential reference exactly for the tested positions (maximum float difference 0). Converted reads matched their 64-frame partitions exactly. Oversized reads and a pre-cancelled stop token were rejected, and the worker reader remained usable afterward. Occasional MP3 seeks were 3.7–4.3 ms despite sub-millisecond medians; the current codec/resampler calls cannot be interrupted once entered. This is a measured tail-latency risk for many simultaneous clip seeks. The result does not set a release threshold or qualify cold SSD reads, multiple concurrent decoder workers, a mixed-plugin 32-track graph, physical-device underruns or all MP3 encoder modes. Keep those as release-performance gates; only a measured failing workload should motivate a bounded decode-ahead or codec change.

The integrated main-branch Release rehearsal reran on the same host with the
locally staged PipeWire/codec development headers. FLAC and MP3 again matched
the sequential oracle exactly, cancellation and bounds passed, and converted
reads matched 64-frame partitions. Same-rate random-seek p95 was 0.212 ms for
FLAC and 0.083 ms for MP3; converted-seek p95 was 0.636 and 0.556 ms. MP3
still had isolated maxima of 3.64–4.15 ms. This confirms the tail-latency
observation without changing its cold-storage and concurrent-worker limits.
