# Warp Lab

A standalone Qt Quick sandbox for auditioning the warp engines on your own
audio. It is **not** the DAW: nothing here touches projects, the arrangement,
live playback or export, and it is built only on request.

## Build and run

From a Quick-enabled build tree configured with `OSCILLATE_BUILD_QUICK_UI=ON`
and `OSCILLATE_BUILD_BENCHES=ON`:

```sh
mkdir -p build/quick/scratch
TMPDIR="$PWD/build/quick/scratch" cmake --build build/quick --target oscillate_warp_lab
build/quick/tools/listening/warp/oscillate_warp_lab [FILE.wav]
```

## Use

- **Load:** drop a file on the window, use **Open…**, or pass a path.
  - The engines run on 48 kHz stereo. Anything else (MP3, FLAC, OGG, 44.1 kHz, mono…) is converted automatically with ffmpeg into a 48 kHz stereo float copy in the cache.
  - The source line says "converted from … by ffmpeg", so a conversion is never hidden.
- **Analysis:** runs once per file. Detected transients are drawn in amber on the source waveform and used by Beats and Complex; YIN pitch marks are used by Tones.
- **Settings:**
  - Pick a **warp mode**, **speed** (0.25–4×) and **transpose**.
  - A render longer than 10 minutes is rejected before allocating output buffers.
  - Each mode has its own controls: Beats gap fill and envelope, Tones grain, Texture grain and fluctuation, Complex Pro formants and envelope.
  - **Mid-clip tempo change** applies speed, 0.6× speed and 1.5× speed across thirds of the file, to expose seams.
- **Rendering:** happens offline on a worker thread, through the same public `warp_dsp` streams as the audition harness, by default on every change.
  - The stats line shows render CPU time, peak, clipped samples and stream status.
- **Listening:** **▶ Original** / **▶ Warped** play through PipeWire (`pw-play`). Space toggles the render, O the original, and keys 1–6 pick a mode.
- **Save…** copies the current render as a 32-bit float WAV, for the review in `docs/performance/warp-recorded-review.md`.

Renders and conversions live in `~/.cache/Oscillate/WarpLab/`. Converted file
names include a digest of the source path so two files with the same basename
do not overwrite one another. **Save…** replaces the selected output atomically
after the full copy succeeds.

`--screenshot PNG` grabs the window after loading and rendering, then quits. Use it to check the layout headlessly with `QT_QPA_PLATFORM=offscreen`.
