# VST3 offline master output formats

**Status:** Experimental vertical slice, 18 October 2026. Requirement: AM-06
(partial), AM-07 failure preservation.

The stopped VST3 effect, VST3 instrument and mixed instrument-plus-audio
exporters no longer write their own project-rate float32 WAV. After the routed
plugin graph has produced its aligned, tail-inclusive project-rate stream, they
hand it to the same master output stage as the native offline master
(`include/oscillate/export/master_output.hpp`, `src/export/master_output.cpp`). That stage:

- writes WAV float32, PCM24 or PCM16, with seeded TPDF dither for integer WAV;
- in a codec build, writes FLAC16, FLAC24 or MP3 through the existing in-process
  libsndfile `EncodedWriter`; no external encoder or `ffmpeg` process is used;
- in a libsamplerate export build, converts to 44.1, 48, 88.2 or 96 kHz with the
  existing `SRC_SINC_BEST_QUALITY` worker converter, using the native master's
  rounded duration, one-frame terminal pad and wrong-duration rejection;
- publishes through the writer's private temporary file and atomic rename, so a
  rejected or failed render leaves an existing target unchanged.

The native master now uses this stage too, keeping its loudness tap as an
observer on the emitted output samples. Its previous resampler/writer loop was
moved, not reimplemented.

New APIs take `MasterOutputOptions`, a variant of `MasterRenderOptions` and
`EncodedMasterOptions`: `render_vst3_effect_master`,
`render_vst3_instrument_master` and `render_vst3_mixed_master`. The existing
`*_wav_master` and `*_wav_stem` functions accept every WAV format and output
rate. The Qt Quick Export Master action passes the selected format and output
rate to these paths; its existing rate validation still rejects MP3 above
48 kHz and conversion in a build without libsamplerate before dispatch.

## Constraints

- Plugins always process at the project rate with 128-frame blocks. Output-rate
  conversion is applied after the full master, including plugin latency
  removal and reported tails. The synthetic fixtures reject any other
  processing rate, so the tests also prove that the plugin rate is unchanged.
- VST3 paths do not produce loudness reports; requesting one fails before
  output is opened. Report support remains the native float32 WAV slice.
- VST3 stems are single-file WAV APIs (any WAV format/rate); there is no VST3
  stem-directory bundle or encoded stem, and Quick does not expose VST3 stems.
- The JACK real-time master remains project-rate float32 WAV.
- An MP3 above 48 kHz, an unsupported output rate, a missing codec or a missing
  converter fails before publication. Plugin process faults fail mid-render and
  discard the temporary file.
- AM-08 export metadata recording, other chain shapes and third-party plugin
  compatibility remain open; these are synthetic-fixture results.

## Evidence

`tests/application/vst3/vst3_effect_export_tests.cpp` (`output_format_export_checks`)
uses a unity-gain, seven-frame-latency effect whose PDC-aligned master must
equal the independent native master. It checks PCM24/PCM16 headers, frame count
and ±2 LSB samples against the float reference, seeded-dither repeat bytes,
44.1/88.2/96 kHz durations and native-conversion parity, converted PCM24, an
eight-frame plugin tail retained at each converted rate, decoded FLAC16/24
(±2 LSB) and MP3 (RMS) at 48, 44.1 and 96 kHz, deterministic encoded repeats,
96 kHz MP3 rejection, and old FLAC/PCM16 preservation after a plugin process
fault or an unsupported 22.05 kHz request with no leftover temporary files.

`tests/application/vst3/vst3_instrument_export_tests.cpp` checks the VST3 instrument's
265-frame tail-inclusive master as PCM24/PCM16, its sustained 0.75 level and
release tail at each converted rate, converted processed-stem equality, FLAC at
48 and 96 kHz, MP3 duration, and preservation of existing MP3/FLAC/PCM24
targets after rejected or faulted renders.

`tests/ui/quick/vst3_export_tests.cpp` dispatches a 44.1 kHz PCM16 instrument
master (exact tail duration), a FLAC instrument master, a rejected 96 kHz MP3
and a 96 kHz PCM24 effect master (converted plateau level) from Quick.
`export_offline_master` remains the regression oracle for the moved native
output stage.
