# Offline loudness report slice

The worker-only export API accepts an optional `RenderLoudnessReport` output for a WAV master, or an ordered vector of `StemLoudnessReport` outputs for WAV stems. Reports are populated only after output publication. The stem vector follows source-track order and excludes bus-only tracks; a partial bundle or rejected render leaves the caller's report data untouched.

This slice accepts stereo float32 WAV output at 44.1, 48, 88.2 or 96 kHz. The tap follows saved routing and automation, built-in track and master effects, limiter latency removal and output-rate conversion. It receives exactly the float samples handed to the WAV writer at the **output** rate. The analyzer implements [ITU-R BS.1770-5 Annex 1](https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.1770-5-202311-I!!PDF-E.pdf) integrated loudness with a 400 ms window and 100 ms step at each supported rate; it returns no numeric integrated LKFS value for a short render or gated silence. Its default 36,000-block capacity bounds a report to one hour. An over-limit render with a requested report fails before publication. The report also carries the final complete ungated 400 ms momentary and 3 s short-term windows described by [EBU Mode](https://tech.ebu.ch/loudness/). A short render has no 3 s value. Left/right sample peaks and 4× intersample peak estimates cover the complete published file, including the final resampler drain.

The standard publishes 48 kHz K-weighting coefficients and requires other rates to match the same frequency response. The rate-dependent bilinear-transform design follows the equations in [libebur128's filter initialization](https://github.com/jiixyj/libebur128/blob/master/ebur128/ebur128.c#L2609-L2657), without linking or importing that library. It reproduces the published 48 kHz coefficients to floating-point precision. The ring and step sizes are derived from the output sample rate, rather than measuring a 400 ms interval only at 48 kHz.

Other WAV formats continue to render without a report. An explicit report request for another format fails with `unsupported`; an unsupported output rate fails regular export preflight. Encoded FLAC/MP3, real-time metering and UI display are outside this slice. Integer dither and lossy encoding would change the published samples, so this API does not claim those files were measured. The 4× value is an estimator, not a certified true-peak standard result; its independent signal oracle and limits are in [the true-peak core note](../performance/true-peak-core.md).

The native analyzer test uses a published full-scale 997 Hz reference and an independent FFmpeg 8.0.1 `ebur128` oracle. The table gives final integrated LUFS, rounded to 0.1 dB by FFmpeg, for two seconds of full-scale mono float sine:

| Rate | 100 Hz | 997 Hz | 10 kHz |
| --- | ---: | ---: | ---: |
| 44.1 kHz | −4.8 | −3.0 | 0.3 |
| 48 kHz | −4.8 | −3.0 | 0.3 |
| 88.2 kHz | −4.9 | −3.0 | 0.3 |
| 96 kHz | −4.9 | −3.0 | 0.3 |

The export contract test renders a same-rate master and stems at every supported rate, verifies the summed master is 6.02 dB above either isolated stem, then decodes each published float WAV and recomputes integrated loudness, final rolling loudness and both channels' sample/intersample peaks from its samples. Optional output-converter fixtures cover 48→44.1/88.2/96 kHz masters, 48→44.1 kHz stems and a 96→48 kHz master. Unsupported requests and interrupted stem bundles preserve the caller's previous report. These fixtures verify rate/window selection and report placement; they do not replace a full loudness or true-peak conformance corpus or listening test.
