# Routed managed-WAV streaming: local evidence

**15 October 2026.** Ubuntu x86-64, Intel Core i7-10700K, GCC 15.2.0. This is a focused routed-player slice, not the 32-track qualification.

## Contract and ownership

`RoutedAudioSource` opens each managed source through `ProjectStore::open_managed_source`, verifies its recorded metadata, and allocates one bounded `PrefetchRing` and callback scratch buffer per clip. The total ring capacity is limited by the caller's frame budget; the player uses 512 × 1024 frames, with a 65,536-frame per-clip ceiling. A single worker owns every audio reader and ring producer. The device callback owns ring consumption, source spans and the transport cursor. Stop and join the output callback before destroying the source; its destructor then stops and joins the worker.

The publisher decides whether a block holds or renders before calling the source-fill function. A hold consumes no frames. On a transport-generation change, the callback publishes the requested sample/generation and freezes the cursor. The worker acknowledges after old writes cease; the callback discards old ring entries; the worker then primes the new generation. The callback polls atomics without waiting. Missing steady-state source frames become silence and increment `stream_missing_frames`; worker failure has a separate status and error. A source-fill failure cannot publish a successor graph or consume source frames.

Audio clips backed by mono/stereo managed sources are supported when the mapped source-range duration exactly equals the project placement. Mismatched rates require the optional source resampler; conversion runs only on the worker in at most 1,024-frame batches. Instrument, stretched and source-changing edits remain explicit unsupported cases. Encoded formats require the optional codec adapter as described below.

## Optional encoded-source extension

The routed worker now uses `AudioReader` inside `SourceRateReader` for verified managed WAV, AIFF, FLAC and MP3. AIFF/FLAC/MP3 require `OSCILLATE_BUILD_CODEC_ADAPTER=ON`; codec-disabled builds reject those encodings with `unsupported` before source preparation. Mismatched rates require `OSCILLATE_BUILD_SOURCE_RESAMPLER=ON` and an exact mapped source-range duration. The worker remains the only decoder/converter owner; the callback, ring generation handoff and source-availability status are unchanged. AudioReader checks format and recorded metadata after `ProjectStore` verifies the exact managed bytes.

The codec-enabled fake-device fixture imports each encoded format, saves and reopens the project, compares first and seeked output frames to a direct managed-source decode, checks zero callback allocations and source underruns, and rejects a tampered managed asset. Codec-disabled tests check an explicit routed-source rejection. Focused GCC and Clang runs, codec-enabled ASan/UBSan and TSan, formatting and tidy passed. The full codec-enabled suite passed 54/54 tests; the GCC coverage build passed 50/50 with 11,225/12,353 executable production lines (90%). A 48 kHz WAV callback regression on the local i7-10700K measured 4.494 µs/block at 64 frames and 7.890 µs/block at 128 frames, with zero callback allocations, render failures and missing frames.

With the codec adapter enabled, `python3 tests/qualification/qualify_live_routed_edit.py` passed on a disposable PipeWire null sink using the WAV regression project: 248,960 captured frames, 34 windows at the original gain and 211 at the edited gain, model/active revision 9, zero render failures and zero missing frames, then revision 9 and gain 1.5 on reopen.

`python3 tests/qualification/qualify_live_routed_encoded.py` generated a five-second 48 kHz FLAC source, imported it through the managed project fixture, and played four seconds through a named disposable PipeWire sink. It captured 202,752 frames with 202 audible 20 ms windows; model and active revision were 6, with zero render failures, missing frames and worker failures. Reopening preserved revision 6 and the saved send. Follow-up integrated-main runs with `--format aiff` and `--format mp3` captured 201,728 and 204,160 frames, with 201 and 200 audible windows respectively. Both reported model/active revision 6, zero render failures, zero missing frames and zero worker failures; the saved send reopened intact. Compressed seek and initial-prime latency remain worker-side and unmeasured under longer media; the callback holds during seek priming without waiting or reading the file.

## Commands and observations

```sh
cmake -S tests/qualification -B build/routed-live-qualification -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/routed-live-qualification --target oscillate_routed_live_fixture oscillate_routed_player
python3 tests/qualification/qualify_live_routed_edit.py
```

The Python command creates and removes a named disposable PipeWire null sink, records its monitor, runs the routed player, then reopens the saved project with `oscillate_routed_live_fixture inspect`. The final run captured 248,576 frames: 36 windows at the original send level followed by 208 windows at the edited level. The final log showed `model=9 active=9 application=active`, `stream_missing_frames=0`, `stream_worker_failed=0`, and `render_failures=0`. The reopened project reported revision 9, no master EQ, and saved send gain 1.5. The deliberately unsupported master-EQ edit and retry reported `model=8 active=7 application=failed` while the old route stayed audible.

```sh
cmake --build build/release --target oscillate_application_routed_device_tests
./build/release/tests/application/oscillate_application_routed_device_tests --bench
```

At 48 kHz, one managed WAV source, group bus and return send, 1,000 paced blocks: 64 frames averaged **3.469 µs/block**, 128 frames **6.465 µs/block**. Both runs reported zero callback allocations, render failures and missing source frames. Fake-device tests additionally cover a paused worker, a visible 64-frame underrun, exact seek resume after generation change, seek during graph hold, and seeking a 31-second clip past the old in-memory cap.
