# MP3 and FLAC redistribution gate

## Exact Ubuntu LAME package follow-up, 15 October 2026

The current copied AppDir MP3 path is
`oscillate` → `libsndfile.so.1` → `libmp3lame.so.0`, with
`libmp3lame.so.0` also depending on `libmpg123.so.0`. These are direct
`DT_NEEDED` edges in the staged binaries. The app's
[`encoded_writer.cpp`](../../src/export/encoded_writer.cpp) requests the
libsndfile MP3 format; it does not contain a separate MP3 encoder. The
[machine-readable component ledger](mp3-ubuntu-codec-ledger.json) records
the three copied libraries, binary package/source names, exact versions,
ELF SHA-256, notice SHA-256 and the LAME source evidence. The
[release AppImage](release-packages.md) is built without the codec adapter,
and AppDir staging refuses `libmp3lame.so.0` until G8 decides. The native
`.deb` copies no codec; it depends on Ubuntu's system libraries.

The staged `libmp3lame.so.0` SHA-256
`e0676e6702b0095212724f2d298685d189dc3b54845c90cb1db1bd2970c29ed2`
is byte-identical to the installed library and to the extracted exact Ubuntu
[`libmp3lame0` 3.101~svn6525+dfsg-2 binary package](https://archive.ubuntu.com/ubuntu/pool/main/l/lame/libmp3lame0_3.101~svn6525+dfsg-2_amd64.deb)
(package SHA-256 `20ab98afff633ed2c8e2cfb1b69a2c0a0cb86c8328b663c5825e6592dab600b4`).
Its installed and extracted `debian/copyright` files also match byte for
byte, SHA-256 `0e4b0e56bb8e5b72a5d3af0629b617c9d78c2c8fb42a722f4b0a0d9bb3cac202`.
That notice assigns `libmp3lame/fft.c` **GPL-1+**; its comment says no GPL
version is stated in the file itself. The exact [Ubuntu source archive](https://archive.ubuntu.com/ubuntu/pool/main/l/lame/lame_3.101~svn6525+dfsg.orig.tar.gz)
contains `fft.c` (SHA-256 `5148c76c02607dfd285a71da2ffec64cbdae293c450ac84bdd442a032d94eea1`).
`libmp3lame/Makefile.am` places it in `libmp3lame_la_SOURCES`, and the
matching [development package](https://archive.ubuntu.com/ubuntu/pool/main/l/lame/libmp3lame-dev_3.101~svn6525+dfsg-2_amd64.deb)
contains `fft.o` in `libmp3lame.a`, with `fft_short`, `fft_long` and
`init_fft` symbols. The [Debian rules archive](https://archive.ubuntu.com/ubuntu/pool/main/l/lame/lame_3.101~svn6525+dfsg-2.debian.tar.xz)
does not omit the file from the library build. More directly, bytes 67–847
of that exact `fft.o` `.text` section match 781 consecutive bytes at file
offset 33,411 in the copied shared object (segment SHA-256
`605b96ca9ac3f7296007386ce5290f69c02bda547855e6bbb422ad92465b53d1`).
The matching span lies in `fft.o`'s `fht` routine. The packaging gate now
checks this fingerprint in the staged and extracted LAME library. This is
strong binary evidence that the copied encoder contains code from the
**GPL-classified `fft.c` source**. A full shared-object link map and legal
interpretation remain unavailable; the audit does not decide the effect on
the DAW's own code. Treat the copied encoder as a distribution blocker
pending an approved product-licence interpretation and corresponding-source
plan.

The broad [upstream LAME licence guidance](https://lame.sourceforge.io/license.txt)
describes LGPL use and an older internal GPL decoder. This Ubuntu library
has a dynamic `libmpg123.so.0` dependency, but that observation does not
resolve its separate `fft.c` classification. Ubuntu's package index now
also offers a later `-2ubuntu0.1` binary; that is **not** the currently
copied `-2` binary and must be audited separately before substitution.

Possible closure routes, without selecting one for the product:

1. Resolve `fft.c`'s GPL-1+ classification, the full shared-object source
   map and the product/distribution licence with qualified review. Supply the exact
   corresponding source, all notices and a tested shared-library replacement
   path for bundled LGPL components; address any GPL obligations found for
   the copied encoder explicitly.
2. Keep the native `.deb` on Ubuntu system codec packages, document and test
   the supported package versions on clean Ubuntu 26.04, and review whether
   that system-dependency route satisfies the intended product licence. It
   does not make an AppImage containing the same binary cleared.
3. Select or build a compatible MP3 encoder only after file-level licence,
   quality, CPU, ABI, MP3 format and redistribution checks. The PRD requires
   MP3 export, so the package must retain that workflow or return to product
   review before scope changes.

**Status:** Local Ubuntu 26.04 package and codec-source evidence, 15 October 2026. **No distribution sign-off.** Applies to the [`.deb`](release-packages.md) with `OSCILLATE_BUILD_CODEC_ADAPTER=ON` and the proposed AppImage. The product uses libsndfile for FLAC/MP3 import and encoded master export; audio recording is outside v1 scope.

## Artifact and functional observation

The inspected `.deb`, SHA-256 `60b3512a97a7f6dee1d9fb0d2bbead3a82fcbe8f91d987b43bcac92a7ff07495`, contains **only** `oscillate`, a desktop file and a prototype warning. It bundles no codec `.so` and no codec notice/source. Its control file directly declares `libsndfile1 (>= 1.0.20)` and `libsamplerate0 (>= 0.1.7)` among other Ubuntu `Depends`; the FLAC, MP3 and Ogg libraries are installed as transitive dependencies of Ubuntu's libsndfile package rather than named in this `.deb` control file. The development host resolves `libsndfile.so.1` to Ubuntu `libsndfile1` 1.2.2-4. This is a **system-package dependency route**, not a licence exemption or a fixed package-version pin.

An extracted `.deb` executable, with `LD_LIBRARY_PATH` removed and `QT_QPA_PLATFORM=offscreen`, exported a 48 kHz stereo FLAC 24-bit master and an MP3 master from a disposable starter-sound project. `file` identified the FLAC as 24-bit stereo 48 kHz/30,064 samples and the MP3 as MPEG Layer III stereo 48 kHz. This proves local encoder availability and UI/export wiring against installed Ubuntu libraries, not clean-system installation or codec redistribution qualification. The earlier empty starter MIDI fixture could not export because it had no sampler patch; `--smoke-midi-workflow` supplied the usable fixture.

## Current library and notice matrix

`ldd` on the exact Quick ELF shows each SONAME below; `dpkg-query -S` maps the resolved file to the listed installed package. The notice reference is the installed Ubuntu `/usr/share/doc/<package>/copyright`, checked locally. A future AppImage must inventory **its actual copied files** and carry their complete applicable notices and source/replacement path; it cannot inherit the `.deb`'s system-package assumption.

| Resolved shared object | Ubuntu package/version | Relevant licence evidence and AppImage action |
| --- | --- | --- |
| `libsndfile.so.1` | `libsndfile1` 1.2.2-4 | Package copyright marks core `LGPL-2.1+`, with file-specific exceptions. [Upstream libsndfile](https://github.com/libsndfile/libsndfile/blob/1.2.2/docs/index.md) describes an LGPL 2.1 or 3 choice and dynamic linking. If bundled, include its exact source/build and LGPL notice, allow replacement, and audit compiled subcomponents. |
| `libFLAC.so.14` | `libflac14` 1.5.0+ds-5 | Ubuntu copyright maps `src/libFLAC/*` to BSD-3-Clause; [Xiph's library licence](https://xiph.org/flac/license.html) agrees. Its **source package** also contains LGPL and GPL files for other components, so use the binary's source-file set and complete package notice rather than assigning those files to `libFLAC` by name. Include BSD notice if bundled. |
| `libogg.so.0` | `libogg0` 1.3.6-2 | Ubuntu copyright marks library source BSD-3-Clause; [upstream notice](https://github.com/xiph/ogg/blob/master/COPYING). Include notice if bundled. |
| `libvorbis.so.0`, `libvorbisenc.so.2` | `libvorbis0a`, `libvorbisenc2` 1.3.7-3build2 | Both package copyright files mark library source BSD-3-Clause; [upstream notice](https://github.com/xiph/vorbis/blob/master/COPYING). Include notice for both copied files. |
| `libopus.so.0` | `libopus0` 1.6.1-1 | Ubuntu copyright marks the main code BSD-3-Clause and some source files BSD-2-Clause. Preserve the package's applicable file-level notices if bundled. |
| `libmpg123.so.0` | `libmpg123-0t64` 1.33.3-2 | Ubuntu copyright and [upstream mpg123](https://www.mpg123.de/) state LGPL-2.1. It is a decoder dependency of libsndfile **and** the local LAME library. Bundling requires the corresponding source, notice and replacement route. |
| `libmp3lame.so.0` | `libmp3lame0` 3.101~svn6525+dfsg-2 | LAME [generally describes LGPL use](https://lame.sourceforge.io/license.txt), but Ubuntu's exact package copyright marks `libmp3lame/fft.c` **GPL-1+**. The exact Ubuntu [source archive](https://archive.ubuntu.com/ubuntu/pool/main/l/lame/lame_3.101~svn6525+dfsg.orig.tar.gz) lists `fft.c` in `libmp3lame_la_SOURCES`; its [Debian build rules](https://archive.ubuntu.com/ubuntu/pool/main/l/lame/lame_3.101~svn6525+dfsg-2.debian.tar.xz) do not remove that file. This discrepancy must be resolved against the intended product licence and actual binary before shipping or copying it into an AppImage. Do not label this binary unqualifiedly LGPL. |

`readelf -d libsndfile.so.1` confirms direct `DT_NEEDED` entries for all seven codec SONAME groups. `readelf -d libmp3lame.so.0` confirms a dynamic `libmpg123.so.0` dependency. The LAME source's `configure.ac` selects external mpg123 by default when present, so the old internal GPL decoder described in [LAME's licence note](https://lame.sourceforge.io/license.txt) is not shown to be compiled into this Ubuntu library. This does **not** resolve the separate `fft.c` notice. The exact Debian rules archive's `debian/copyright` is byte-identical to the installed `/usr/share/doc/libmp3lame0/copyright` (SHA-256 `0e4b0e56bb8e5b72a5d3af0629b617c9d78c2c8fb42a722f4b0a0d9bb3cac202`). The source tarball was downloaded only under ignored `build/` and hashed `c4c3aae99c495d50caac9a181fbd6f5f7fb5859ad2d27431ec7a0a0010bdec01`; the Debian rules archive hash is `bd2d8fb86ba7e2a19e99decceee84a4df97ff84b77f4976bb7e89a29d0c93331`.

The [LAME project's current patent-information page](https://lame.sourceforge.io/links.php) says relevant MP3 compression patents expired by April 2017 and no licence fees are collected. That is the upstream statement found for this codec; this audit found no current MP3 royalty term to add to the package. Copyright and LGPL/GPL source, notice and replacement conditions remain independent of patent expiry. No recording-device or recorder-specific codec terms are implicated by the current export/import path.

## Decisions needed before a release artifact

1. **Resolve LAME's `fft.c` classification** with a recorded source/file provenance and approved product licence interpretation, or select an MP3 encoder build with a verified compatible file-level licence. The broad upstream LGPL statement and Ubuntu package's GPL-1+ file classification conflict for this specific binary. Do not silently substitute a newer LAME binary; redo quality, ABI and payload checks if changing versions.
2. Choose whether the native `.deb` continues to depend on Ubuntu codec packages or carries any private copies. For the system route, record exact supported Ubuntu package versions and installed copyright/source access; test clean Ubuntu 26.04 install and MP3/FLAC import/export. The current minimum `Depends` versions alone do not pin the tested codec feature set.
3. For an AppImage, first enumerate every included ELF and `DT_NEEDED` closure. Decide which codec libraries are bundled versus host-provided and why; include matching notices, corresponding source/build details and a working LGPL-library replacement path for each bundled LGPL component. Validate on clean X11/Wayland targets and with the installed system codec packages absent where the image claims self-containment.
4. Regenerate this matrix from the **release candidate** and test FLAC 16/24, MP3, decode/reopen, bit depth/rates, and failure paths in the actual package. Capture exact file hashes, package/source versions, dynamic loading, notices and source/relink instructions. Code/content licensing and the wider [distribution SBOM gate](distribution-sbom-review.md) remain open.

Reproduction commands: `dpkg-deb --contents <prototype.deb>`, `dpkg-deb --field <prototype.deb> Depends`, `ldd <quick-elf>`, `readelf -d /usr/lib/x86_64-linux-gnu/{libsndfile.so.1,libmp3lame.so.0}`, `dpkg-query -S <resolved-file>`, `dpkg-query -W <package>`, and `rg '^(Files:|License:)' /usr/share/doc/<package>/copyright`. The functional smoke used `--create ... --smoke --smoke-midi-workflow`, then `--open ... --smoke --smoke-export <target> 3` for FLAC and `5` for MP3.
