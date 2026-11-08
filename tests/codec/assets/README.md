The original mono 48 kHz, 20 ms, 1 kHz sine fixtures were generated with
FFmpeg's `sine` source and encoded as AIFF PCM16, FLAC, MP3 and Ogg/Vorbis.
The stereo WAV contains constant left/right 0.25/0.5 float32 channels. The test uses
the mathematical sine amplitude as an independent decode oracle for lossless
formats and a broad peak range for lossy MP3. These are not imported samples
from another musical work.

`sine_no_xing.mp3` is a mono 48 kHz, 0.2 s, 1 kHz sine encoded at 64 kbit/s
with an ID3v2.3 tag and no Xing/Info header
(`ffmpeg -f lavfi -i sine=frequency=1000:sample_rate=48000:duration=0.2 -ac 1
-c:a libmp3lame -b:a 64k -write_xing 0 -id3v2_version 3 -metadata title=sine
-bitexact`), so a decoder has to estimate its length.

SHA-256:

- `sine.aiff`: `b20dfead2878ed5e6c2130fc12f97a2d531ae78458835ad8cb317416a0f35938`
- `sine.flac`: `2adab4cc0289e2a5a3df2791388ac9465845bf8cdd796cec9bf40d862630df61`
- `sine.mp3`: `bf62c32b101636811ad8d05f9e21d87765a0b738d038d5c43f0869e354de1389`
- `sine_no_xing.mp3`: `4a4b432f41aa8a3be365c479bd1acde20741f120abcd360ff07a47b6fec589b7`
- `stereo.wav`: `2ac1307bec077b3b236e2832c28b46fff0af43ee60c36982049aa55d6a95ed46`
- `unsupported.ogg`: `a3d85450286364c57973d87f4a21b4a9e5681767605f6f7639732eb8279022f2`
