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

`vbr_no_xing.mp3` is a mono 48 kHz, 2 s variable-bitrate MP3 with no
Xing/Info header and no ID3 tag: 0.1 s of white noise, encoded at a high
bitrate, then 1.9 s of silence at a low one, so a length estimated from the
file size and the first frame's bitrate falls far short
(`ffmpeg -f lavfi -i anoisesrc=color=white:sample_rate=48000:amplitude=0.5:duration=0.1:seed=1
-f lavfi -i anullsrc=r=48000:cl=mono -filter_complex
"[0:a]aformat=channel_layouts=mono[a];[1:a]atrim=duration=1.9[b];[a][b]concat=n=2:v=0:a=1"
-ac 1 -c:a libmp3lame -q:a 0 -write_xing 0 -id3v2_version 0 -bitexact`).

SHA-256:

- `sine.aiff`: `b20dfead2878ed5e6c2130fc12f97a2d531ae78458835ad8cb317416a0f35938`
- `sine.flac`: `2adab4cc0289e2a5a3df2791388ac9465845bf8cdd796cec9bf40d862630df61`
- `sine.mp3`: `bf62c32b101636811ad8d05f9e21d87765a0b738d038d5c43f0869e354de1389`
- `sine_no_xing.mp3`: `4a4b432f41aa8a3be365c479bd1acde20741f120abcd360ff07a47b6fec589b7`
- `vbr_no_xing.mp3`: `0f05d9c57fff1707700ada6acf00a95d4366e193bfe1d030b0a67cacb59f18e1`
- `stereo.wav`: `2ac1307bec077b3b236e2832c28b46fff0af43ee60c36982049aa55d6a95ed46`
- `unsupported.ogg`: `a3d85450286364c57973d87f4a21b4a9e5681767605f6f7639732eb8279022f2`
