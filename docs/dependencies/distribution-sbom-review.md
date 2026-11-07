# Distribution dependency and licence review

**MP3 source gate:** The exact local AppDir's copied LAME binary and notice
match Ubuntu's `libmp3lame0` 3.101~svn6525+dfsg-2 package. That package
classifies `libmp3lame/fft.c` GPL-1+, and its matching development archive
contains `fft.o`. A [codec source ledger and packaging drift gate](mp3-flac-redistribution-gate.md)
now retain this evidence. Distribution remains blocked on the exact compiled
source map and a product/licence decision; the native `.deb` has a distinct
system-package route and no copied LAME binary.

**Static-runtime notice checkpoint:** The local AppDir/AppImage prototype now
stages and verifies [full notice files for musl, libfuse, squashfuse, zstd,
zlib and mimalloc](appimage-runtime-static-notices.md). The pinned runtime's
Makefile names mimalloc although its top-level licence does not. Floating
Alpine build dependencies prevent exact APK/source-version attestation; static
libfuse LGPL source/relinking and any further link-map dependencies remain
release gates. This is notice evidence, not G8 closure.

**Runtime notice checkpoint (15 October 2026):** Local AppDir staging now
includes a hash-pinned copy of the [type-2 runtime 20251108 licence](https://github.com/AppImage/type2-runtime/blob/20251108/LICENSE),
and the extracted-image verifier requires it. The licence names statically
linked musl, libfuse, squashfuse, zstd and zlib; staging that top-level notice
does not discharge those components' notice, source or LGPL obligations. The
native `.deb` remains a system-library package route. The [payload note](package-notice-payload.md)
records the changed verifier and the local replay. G8 remains open.

## 15 October 2026 package audit update

The current native packaging script now inventories all 23 `.qml` files named by
`apps/daw_quick/qml.qrc`, rather than scanning the empty `apps/daw_quick/*.qml`
location. Their sole direct import is `QtQuick`. Ubuntu 26.04 publishes its
runtime import as [`qml6-module-qtquick` 6.10.2+dfsg-3](https://packages.ubuntu.com/resolute/qml6-module-qtquick),
which depends on `qml6-module-qtqml`. Qt explains why ELF dependency scanning
alone misses [QML runtime imports](https://doc.qt.io/qt-6/qt-deploy-qml-imports.html).
The `.deb` prototype therefore appends `qml6-module-qtquick` to its
`dpkg-shlibdeps` result and fails staging if a future direct import has no
reviewed package mapping. The extracted-package smoke checks that the declared
QML package is in `Depends`. This is a dependency declaration, not clean-host
installation evidence; Qt's QML registration in this local build also resolves
some imports from library resources, as the [runtime trace](qt-quick-runtime.md)
records.

A local replay using the earlier `build/ubuntu-package-agent` Release Quick ELF
and sibling scanner helper, **without** the optional CMake-cache feature lock,
produced a `.deb` with both executables, 23 QML source hashes, the corrected
dependency and three project notice files. The extracted payload/notice smoke
passed. Its SHA-256 was
`a4a6aa84065840af4783d9b42fd6152c28d163961f0746f4d7595f1ec37a0f8e`.
The same inputs staged an AppDir with 117 ELF files (two project executables,
four Qt plugins and 111 copied shared libraries), 102 installed Ubuntu
copyright files and three project notices. Offscreen create/reopen and the
synthetic scanner smoke passed. This AppDir has **no separate QML module
directory**; the local Qt resource imports do not establish clean-host
portability. Neither artifact is a release candidate, and the replay does not
prove the current complete feature profile or a new AppImage runtime payload.

For the release gate, close these items against one final build and its exact
hashes:

| Component group | Package evidence to freeze | Unresolved closure action |
| --- | --- | --- |
| Qt Core/Gui/Qml/Quick/Widgets and QML runtime | `.deb` `Depends` includes `qml6-module-qtquick`; AppDir copies Qt shared objects and four platform/input plugins, while `QtQuick` currently resolves from Qt resources. Qt's [module licences and third-party SBOM](https://doc.qt.io/qt-6/licensing.html) and [LGPL obligations](https://www.qt.io/development/open-source-lgpl-obligations) remain authoritative inputs. | Freeze Qt binary/source versions, inspect actual QML/platform closure on clean Ubuntu X11 and Wayland hosts, supply full notices and source/offer where applicable, and demonstrate modified-library replacement and execution for both package forms. |
| VST3 SDK and user plug-ins | The helper is packaged; pinned SDK interface headers are compiled into the app/helper; both pinned MIT notices are staged. Steinberg's [SDK licence and trademark guidance](https://github.com/steinbergmedia/vst3sdk#license--usage-guidelines) treats code and optional logo/name use separately. | Recheck pinned source hashes and notices against the final binaries; review any VST trademark/logo use. User-installed plug-ins are outside the payload unless deliberately bundled. |
| PipeWire and JACK | The local Quick ELF depends on `libpipewire-0.3.so.0`; it has no `libjack.so.0` dependency. PipeWire's [source licence inventory](https://github.com/PipeWire/pipewire/blob/master/LICENSE) identifies file-specific exceptions. | Freeze the backend configuration and provider. If JACK is linked in a release build, resolve the exact JACK2 or PipeWire JACK binary and its licence separately; keep the audio server outside the app payload ledger unless shipped. |
| libsndfile, FLAC, MP3 and libsamplerate | The local `.deb` uses system libsndfile/libsamplerate; the AppDir copies them and FLAC/Ogg/Vorbis/Opus/mpg123/LAME from Ubuntu. [libsndfile's LGPL choices](https://github.com/libsndfile/libsndfile/blob/1.2.2/docs/index.md) and the [codec file audit](mp3-flac-redistribution-gate.md) apply. | Resolve the exact `libmp3lame` `fft.c` GPL-1+ classification, freeze encoder availability and file-level notices/source, and prove shared-library replacement for bundled LGPL codecs. Do not classify the entire LAME binary as LGPL from an upstream summary. |
| Project code, nlohmann/json, starter assets, AppImage runtime | The header and VST3 notices are staged; four generated WAVs are embedded in QRC. The pinned [AppImage type-2 runtime licence](https://github.com/AppImage/type2-runtime/blob/20251108/LICENSE) and seven static-dependency notice files now stage with the AppDir and pass extracted-image verification. | Publish the product code licence and explicit starter-sound grant; verify the actual nlohmann header version/hash; bind the runtime's exact compiled source/archive set to its binary, supply corresponding source/relinking evidence (especially static libfuse), and approve a component-level SPDX/source ledger for every bundled ELF before distribution. |

The precise product-code licence, sound-content grant, LAME interpretation and
Qt/AppImage LGPL fulfilment are still product/release choices. This audit does
not select them or constitute legal sign-off.

**Later packaging evidence:** The local package scripts now embed checked-in VST3 and nlohmann notice material, and the AppDir stages installed Ubuntu copyright files for its copied ELF closure. See the [notice payload gate](package-notice-payload.md). These files do not resolve the release licence, source/offer or replacement gates below.

**Status:** Evidence inventory, **not distribution sign-off**. Reviewed
15 October 2026 for Ubuntu 26.04 x86-64. This document combines current
CMake declarations, one local development ELF inspection, and the linked
licence sources. It does not identify the contents of a future `.deb` or
AppImage release candidate. A [local-only `.deb` prototype](ubuntu-package-prototype.md)
has since been built and inspected, followed by a scanner-enabled variant
that includes the sibling VST3 helper. Neither is a release candidate. A newer [combined Quick, scanner and codec audit](quick-scanner-codec-combined-audit.md)
inspects one `BUILD_TESTING=OFF` package with all three paths and export
resampling enabled; it remains a local prototype and does not close the
licence gate. A [staged AppDir experiment](appdir-local-prototype.md) records
copied ELF and Qt plugin files and passes offscreen smoke. A later [local
AppImage experiment](appimage-local-prototype.md) pins the runtime/tool and
verifies an extracted image, but is not an approved distribution payload. The
[copied-ELF licence ledger](appdir-copied-elf-licence-review.md) now maps its
111 shared libraries and four Qt plugins to exact installed binary/source
packages and copyright hashes; notice assembly and per-binary licence review
remain open.

## Build configurations and observed dependency closure

The release configuration is not fixed yet. Root `CMakeLists.txt` enables
the Qt Widgets desktop target by default, but the selected Qt Quick shell,
VST3 scanner, libsndfile codec adapter and native convolution core are opt-in.
`src/export` and `src/media` have separate opt-in libsamplerate switches.
`src/audio_io` requires PipeWire and
adds JACK only when its development package is found. `BUILD_TESTING=ON`
adds `Qt6::Test` to the **Quick executable itself**, not only to a separate
test target. These switches must be fixed in a release preset before a
package inventory can be meaningful.

On 15 October, `readelf -d` on the local
`build/quick/apps/daw_quick/oscillate` development executable reported
direct `DT_NEEDED` entries for Qt Core/Gui/Qml/Quick/Widgets/**Test**,
PipeWire, libsamplerate, libcrypto, libsndfile and C++/C runtimes. `ldd`
also resolved Qt QmlMeta/QmlModels/QmlWorkerScript/Network/OpenGL/DBus and,
through libsndfile, libFLAC, libvorbis, libvorbisenc, libopus, libogg,
libmpg123 and libmp3lame. This particular ELF had no `libjack` dependency.
Its `RUNPATH` contains absolute paths under the developer's home cache and `/tmp`.
It is **not** a redistribution artifact. `ldd` alone does not show QML or
Qt platform plugins loaded at runtime, and system-resolved libraries are
not automatically AppImage payload files. See the earlier
[Quick runtime inventory](qt-quick-runtime.md).

The examined `build/quick/CMakeCache.txt` had `BUILD_TESTING`, Quick UI,
codec adapter and export resampler **ON**, and the VST3 scanner **OFF**.
Reproduce the ELF observation with `readelf -d <quick-executable>` and
`ldd <quick-executable>` on that same build; repeat from the installed
release artifacts rather than treating this development path as a lockfile.

The later scanner-enabled local `.deb` uses `BUILD_TESTING=OFF` and includes
`/usr/bin/oscillate_vst3_scan_helper` beside the Quick executable. Its
`dpkg-shlibdeps` input includes both ELFs; the sidecar records the helper's
hash and dependency list. This proves that the helper is in this **prototype**
payload, but it does not settle the SDK notice, plugin isolation or clean-host
runtime gates. Its codec and resampler switches are off.

## Component ledger

“Confirmed” below means the cited source establishes the stated upstream
terms for the identified component. It does **not** mean the final package
has met the terms. For a native package, dependencies may be supplied by
Ubuntu; for an AppImage, every bundled binary needs its own inventory and
notice handling.

| Component / build trigger | Confirmed source terms and current use | Distribution action still needed |
| --- | --- | --- |
| Qt 6.10 Core, Gui, Qml, Quick, Widgets; Quick UI | These five direct modules offer the LGPLv3 route; the [Qt 6.10 module list](https://doc.qt.io/qt-6.10/licensing.html) also names GPL-only modules that must not be assumed safe because another Qt module is LGPL. Current QML imports only `QtQuick`. The [Qt obligations page](https://www.qt.io/development/open-source-lgpl-obligations) requires licence/use notice, corresponding library source or controlled written offer, and user replacement/relinking and running of an LGPL library. | Lock the Qt version and actual module/plugin closure. Keep shared linking; prove replacement using the installed `.deb` and mounted/extracted AppImage, including launcher paths and updates. Include Qt third-party notices and source/offer. A bare upstream download URL is not treated as a verified offer. |
| Qt Test; `BUILD_TESTING=ON` | `apps/daw_quick/CMakeLists.txt` links `Qt6::Test` into the executable under the testing switch; the inspected development ELF has `libQt6Test.so.6`. [Qt Test documentation](https://doc.qt.io/qt-6.10/qttest-index.html) lists an LGPLv3 option. | Decide release `BUILD_TESTING`. If left on, include Test and its transitive files in SBOM and package review. Do not copy the earlier five-module inventory as complete for this configuration. |
| libsndfile; `OSCILLATE_BUILD_CODEC_ADAPTER=ON` | `src/codec` and `src/export` link shared libsndfile. [Upstream licensing](https://github.com/libsndfile/libsndfile/blob/1.2.2/docs/index.md) offers LGPL 2.1 or 3 and describes dynamic linking. The local Ubuntu library is `libsndfile1` 1.2.2-4. | Preserve applicable LGPL text, notice, corresponding source path and shared-library replacement route for any bundled copy. Confirm the exact Ubuntu/runtime build's embedded subcomponents and copyright file; do not infer a single licence from the library name. |
| FLAC, Ogg/Vorbis, Opus, mpg123, LAME; transitive codec closure | The local `libsndfile.so.1` directly needs all listed shared libraries. [FLAC upstream](https://github.com/xiph/flac) calls `libFLAC` BSD-like; [mpg123 upstream](https://www.mpg123.de/) says LGPL 2.1; [LAME upstream](https://lame.sourceforge.io/) says LGPL. [libsndfile's build](https://github.com/libsndfile/libsndfile/blob/1.2.2/CMakeLists.txt) identifies the optional codec links. These are **local binary observations**, not a fixed cross-distribution dependency set. | For the chosen Ubuntu packages or AppImage payload, resolve each ELF to exact package/version and copyright file. Verify LAME's `libmp3lame` file-specific terms and the versions of Ogg/Vorbis/Opus; include their notices if bundled. Test FLAC/MP3 in the packaged build rather than assuming the local encoder features are present. |
| VST3 SDK; `OSCILLATE_BUILD_VST3_SCANNER=ON` | Pinned `v3.8.1_build_84` [root MIT licence](https://github.com/steinbergmedia/vst3sdk/blob/v3.8.1_build_84/LICENSE.txt). The [source inventory](vst3-sdk-source.md) records the pinned `pluginterfaces`, `base`, and `public.sdk` submodule licences. CMake verifies the release manifest, interface headers and notices; the full root and `pluginterfaces` MIT notices are checked in under `third_party/notices/`. Steinberg's [current README](https://github.com/steinbergmedia/vst3sdk/blob/master/README.md) separates MIT code use from optional VST trademark/logo rules. | Include the checked-in notices in release packages and verify the package payload. Decide marketing/logo use and review current trademark guidance separately. Third-party VST3 plug-ins discovered from user folders are **not** DAW package contents unless deliberately bundled. |
| PFFFT v1.1.0; warp DSP and opt-in convolution core | [Pinned licence](https://github.com/marton78/pffft/blob/v1.1.0/LICENSE.txt) permits source/binary redistribution with preserved notice/disclaimer and no endorsement. Vendored in `third_party/pffft` with [a local allocation patch](../../third_party/pffft/patches/allocation-failure.patch) already applied. The warp DSP always links it; `OSCILLATE_BUILD_CONVOLUTION_DSP=ON` adds the convolution core. | Include it in the runtime SBOM. Record the peeled upstream commit, patch SHA-256 and modified-source status, ship the full upstream notice, and validate actual package contents and ABI. See [PFFFT inventory](pffft-convolution.md). |
| nlohmann/json 3.11.3 minimum; persistence | `src/persistence` requires `nlohmann_json` and compiles its header implementation into project objects. The [v3.11.3 MIT licence](https://github.com/nlohmann/json/blob/v3.11.3/LICENSE.MIT) requires copyright and permission notice with substantial copies. | Pin the *actual* header package/version used for release and include its MIT notice with the distributed binary/source materials. A header-only dependency will not appear in `DT_NEEDED`; inspect compile inputs/package lock. |
| Generated starter WAVs; Quick QRC and Widgets QRC | Four committed WAVs are generated from this repository's standard-library Python script, with hashes and reproducibility in the [sound manifest](../../assets/starter/manifest.json) and [asset note](../assets/generated-starter-sounds.md). No external recording or sample pack is identified. | Product owner must explicitly choose and publish a sound-content licence or usage grant, attribution text and allowed redistribution terms. Generation provenance alone does not give recipients a licence. Perform listening review and verify exact QRC/package bytes. |
| PipeWire, optional JACK; audio I/O | `src/audio_io` requires `libpipewire-0.3`; JACK is conditional. [PipeWire's licence inventory](https://github.com/PipeWire/pipewire/blob/master/LICENSE) identifies its usual MIT terms and exceptions for other components. | Record whether the release links JACK and which provider supplies `libjack.so.0` (JACK2 or PipeWire JACK). Resolve the actual library and file-specific notices. Separate the DAW client library from server/plugins that a package may depend upon but does not ship. |
| OpenSSL Crypto; media, persistence, optional scanner | CMake requests OpenSSL 3 Crypto; the local Quick ELF needs `libcrypto.so.3`. [OpenSSL 3 licence](https://github.com/openssl/openssl/blob/master/LICENSE.txt) is Apache-2.0. | Record the exact Ubuntu package or AppImage binary and provide applicable Apache text/NOTICE and attribution for any bundled copy. |
| libsamplerate; export/source converter options | The local Quick ELF needs `libsamplerate.so.0` when `OSCILLATE_BUILD_EXPORT_RESAMPLER=ON` or `OSCILLATE_BUILD_SOURCE_RESAMPLER=ON`; [upstream licence](https://github.com/libsndfile/libsamplerate/blob/master/COPYING) is BSD-2-Clause. | Fix enabled/disabled output and source-rate behavior, package version, notice and actual binary closure. See [output](export-resampler.md) and [source-rate](source-resampler.md) inventories. |

## Decisions and evidence needed to close the release gate

1. Record the product's own code licence and an explicit grant for the four
   starter sounds. There is no repository root `LICENSE` or `COPYING` today.
2. Define one reproducible Ubuntu 26.04 release build configuration:
   Qt Quick versus the legacy Widgets executable, `BUILD_TESTING`, VST3,
   codec, resampler, JACK and any future convolution switch. A build with
scanner/codec/resampler off cannot claim those product capabilities.
3. Build release-candidate `.deb` and AppImage artifacts. Inventory every installed/bundled file,
   executable and `.so` with source package, exact version, checksum,
   SPDX licence expression, copyright/notice location and reason for
   inclusion. Preserve Ubuntu package dependency metadata for the `.deb`;
   identify the full shared-object and runtime plugin/QML closure on a clean
   target. Qt's [third-party-code inventory and SPDX SBOM](https://doc.qt.io/qt-6.10/licensing.html)
   are inputs, not substitutes for the payload inventory.
4. For LGPL libraries supplied in the AppImage, publish applicable full
   licence texts, source/offer and working replacement instructions. Test
   replacement and execution for Qt and any bundled libsndfile/mpg123/LAME
   libraries. Test native-package replacement against its declared Ubuntu
   dependencies. Capture commands and results, including Wayland and X11
   Qt platform plugins and QML imports.
5. Review notices and source distribution as the final artifacts are built,
   then record approval against those exact hashes. Until then, licence
   compatibility is a candidate assessment and the distribution gate stays
   open.

This is an engineering evidence review, not legal advice or a release
authorization.
