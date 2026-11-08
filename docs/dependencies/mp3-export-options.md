# MP3 export distribution options — decision record for review

**Decision (G8, [#93](https://github.com/jcsduncan/oscillate/issues/93)):**
keep libsndfile with Ubuntu's LAME. The AppImage bundles `libmp3lame.so.0`
with its notice and a written source offer linking the exact source
([release packages](release-packages.md)). The options below are the
alternatives that were weighed. The
[exact-byte codec audit](mp3-flac-redistribution-gate.md) records the
bundled binary.

## Current constraint

The application calls libsndfile's MP3 writer. Its current Ubuntu 26.04
`libsndfile.so.1` has a direct `DT_NEEDED` edge to `libmp3lame.so.0`;
[libsndfile's own build definition](https://github.com/libsndfile/libsndfile/blob/1.2.2/CMakeLists.txt)
links `mp3lame::mp3lame` when MPEG support is enabled. Merely copying a
different encoder beside the application cannot replace this link. The
AppImage bundles the exact [Ubuntu LAME 3.101 source](https://archive.ubuntu.com/ubuntu/pool/main/l/lame/lame_3.101~svn6525+dfsg.orig.tar.gz)
build whose `fft.c` code and GPL-1+ source classification are recorded in
the audit. Its matching [Debian packaging source](https://archive.ubuntu.com/ubuntu/pool/main/l/lame/lame_3.101~svn6525+dfsg-2.debian.tar.xz)
contains the file-level copyright declaration.
The native `.deb` obtains this library through Ubuntu's package manager.

| Route | Full MP3 workflow and technical work | Licence and distribution position | Evidence required before selection |
| --- | --- | --- | --- |
| Rebuild LAME after replacing `fft.c` with independently licensed code, or secure a compatible grant | Preserves libsndfile's writer API and LAME's encoding features **if** the replacement is bitstream and quality compatible. The exact Ubuntu `fft.c` defines `fft_short`, `fft_long`, and `init_fft`; `Makefile.am` compiles it into the shared library. This is a genuine DSP replacement, not a build flag. | Could remove the identified GPL-classified file only after an independently licensed implementation or documented grant. The rest of the exact source, build inputs, LGPL obligations, and resulting binary need a new file-level audit. The original `fft.c` header itself mentions multiple claimed rights, so its informal wish to allow reuse is not a licence grant. | Source provenance; legal review of all LAME files; reproducible build and ELF/source mapping; test vectors, listening tests and CPU comparison for 44.1/48 kHz MP3 outputs from every supported project rate; replacement-library and corresponding-source path. |
| Use [Shine](https://github.com/toots/shine) as a separately integrated encoder | Ubuntu 26.04 offers `libshine3` `3.1.1-3build1`. Its API encodes MP3, but libsndfile does not use it. A new export adapter would have to handle PCM conversion, channel policy, supported rate/bitrate checks, flush, ID3/metadata policy, cancellation and file atomicity. Import could still use libsndfile/mpg123. | Ubuntu's installed `libshine3` copyright assigns library files LGPL-2; `debian/*` is GPL-2+ source packaging, not evidence that GPL code is in the shared library. A copied Shine binary still needs exact source, full applicable notices and a working LGPL replacement route. A separate audit is required before bundling. | File-level binary audit, packaging prototype, supported output matrix, blind listening on representative masters and difficult signals, objective codec diagnostics, repeat-render and decode/reopen tests, CPU benchmarks. |
| Delegate LAME and libsndfile to Ubuntu system packages | The **native `.deb`** already has a practical route: it depends on `libsndfile1`; [Ubuntu 26.04 `libsndfile1` declares `libmp3lame0 (>= 3.100)`](https://packages.ubuntu.com/resolute/libsndfile1). On an AppImage, host delegation requires a host `libsndfile1` and its codec dependencies, plus ABI/version checks. Omitting only LAME while bundling the current libsndfile makes the image depend on a host SONAME that a clean machine need not have. | Avoids *copying* Ubuntu codec bytes into the AppImage. It does not settle the DAW/product licence interpretation or guarantee a standalone AppImage; Ubuntu's LAME library retains the same source question. The host may install a later security-updated build. | Clean Ubuntu 26.04 X11 and Wayland install tests with only declared baseline packages; MP3 and FLAC import/export; diagnostic when absent; supported version/ABI window; project decision whether a system-library requirement is acceptable for AppImage. |

### Quality and maintenance evidence for Shine

[Shine's own README](https://github.com/toots/shine) says it has no
psychoacoustic model and warns that the old encoding code may have bugs. Its
published speed examples use different hardware and 128 kb/s inputs, so they
do **not** establish a speed advantage on the DAW's Ubuntu x86-64 workload.
[FFmpeg's encoder documentation](https://ffmpeg.org/ffmpeg-codecs.html#libshine)
identifies Shine as mono/stereo and constant-bitrate only, with quality below
LAME. These are material limitations for a production master exporter, even
though the PRD does not prescribe VBR. The [upstream repository metadata](https://api.github.com/repos/toots/shine)
reports its last source push as 1 December 2023 as checked on this
date; package availability alone is not evidence of ongoing algorithm work.
It is a *candidate for measured comparison*, not a drop-in equivalent or a
justification to weaken the MP3 export acceptance gate.

### Why there is no code prototype in this change

The Ubuntu `libshine3` package and upstream API establish that an encoder
can be called, but the existing writer is linked through libsndfile to LAME.
A tiny Shine encode test would only duplicate the upstream API example; it
would not test DAW integration, quality, metadata, tails or output correctness.
Changing the adapter before deciding the quality/licence route would introduce
a second export path without review. The next useful prototype is a
**standalone comparative export harness** using representative owned audio
fixtures, matched bitrate/metadata/channel settings, decode/reopen checks,
blind listening and CPU measurement. It should be built only after the team
sets a quality floor and decides whether CBR-only output is acceptable.
