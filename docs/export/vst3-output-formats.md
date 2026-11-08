# VST3 offline master output formats

**Status:** Experimental vertical slice, 18 October 2026. Requirement: AM-06
(partial), AM-07 failure preservation.

VST3 export writes no output of its own: a project with VST3 instruments or
effects renders through the native offline master itself
([VST3 offline export](vst3-offline-export.md)), which hands its aligned,
tail-inclusive project-rate stream to the master output stage
(`include/oscillate/export/master_output.hpp`, `src/export/master_output.cpp`). That stage:

- writes WAV float32, PCM24 or PCM16, with seeded TPDF dither for integer WAV;
- in a codec build, writes FLAC16, FLAC24 or MP3 through the existing in-process
  libsndfile `EncodedWriter`; no external encoder or `ffmpeg` process is used;
- in a libsamplerate export build, converts to 44.1, 48, 88.2 or 96 kHz with the
  existing `SRC_SINC_BEST_QUALITY` worker converter, using the native master's
  rounded duration, one-frame terminal pad and wrong-duration rejection;
- publishes through the writer's private temporary file and atomic rename, so a
  rejected or failed render leaves an existing target unchanged.

The native master uses this stage too, keeping its loudness tap as an
observer on the emitted output samples.

`render_vst3_master` takes `MasterOutputOptions`, a variant of
`MasterRenderOptions` and `EncodedMasterOptions`. `render_vst3_stem` and
`render_vst3_stems` accept every WAV format and output rate. The Qt Quick Export Master action passes the selected format and output
rate to these paths; its existing rate validation still rejects MP3 above
48 kHz and conversion in a build without libsamplerate before dispatch.

## Constraints

- Plugins always process at the project rate with 128-frame blocks. Output-rate
  conversion is applied after the full master, including plugin latency
  removal and reported tails. The synthetic fixtures reject any other
  processing rate, so the tests also prove that the plugin rate is unchanged.
- A project with VST3 stages gets the native master's loudness report and
  tail trimming.
- VST3 stems are WAV only (any WAV format and rate), as a bundle
  (`render_vst3_stems`) or one track (`render_vst3_stem`); there is no
  encoded stem.
- The JACK real-time master remains project-rate float32 WAV.
- An MP3 above 48 kHz, an unsupported output rate, a missing codec or a missing
  converter fails before publication. Plugin process faults fail mid-render and
  discard the temporary file.
- AM-08 export metadata recording, other chain shapes and third-party plugin
  compatibility remain open; these are synthetic-fixture results.

## Evidence

`tests/application/vst3/vst3_effect_export_tests.cpp` (`output_format_export_checks`)
uses a unity-gain, seven-frame-latency effect whose PDC-aligned master must
equal the native master without it. It checks PCM24/PCM16 headers, frame count
and ±2 LSB samples against the float reference, seeded-dither repeat bytes,
44.1/88.2/96 kHz durations and native-conversion parity, converted PCM24, an
eight-frame plugin tail retained at each converted rate, decoded FLAC16/24
(±2 LSB) and MP3 (RMS) at 48, 44.1 and 96 kHz, deterministic encoded repeats,
96 kHz MP3 rejection, and old FLAC/PCM16 preservation after a plugin process
fault or an unsupported 22.05 kHz request with no leftover temporary files.
`export_checks` checks a loudness report on a VST3 effect master.

`tests/application/vst3/vst3_instrument_export_tests.cpp` checks the VST3 instrument's
264-frame tail-inclusive master as PCM24/PCM16, its sustained 0.75 level and
release tail at each converted rate, converted processed-stem equality, FLAC at
48 and 96 kHz, MP3 duration, and preservation of existing MP3/FLAC/PCM24
targets after rejected or faulted renders.

`tests/ui/quick/vst3_export_tests.cpp` dispatches a 44.1 kHz PCM16 instrument
master (exact tail duration), a FLAC instrument master, a rejected 96 kHz MP3
and a 96 kHz PCM24 effect master (converted plateau level) from Quick.
`export_offline_master` remains the regression oracle for the moved native
output stage.
