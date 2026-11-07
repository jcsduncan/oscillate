# Local package notice payload gate

**MP3 codec drift gate:** AppDir staging and AppImage extraction now compare
the copied libsndfile/LAME/mpg123 chain to a [reviewed exact-byte ledger](mp3-ubuntu-codec-ledger.json).
That ledger is staged inside the AppDir at
`usr/share/doc/oscillate/third-party/codec-source-ledger.json`; extraction
checks its bytes against the reviewed repository copy. A local replay of the
older Release Quick/scanner pair produced a 237-file AppDir tree (SHA-256
`dd45449e3a965e9b0d1e6f55e761e5808608e98acbc02776bfd07558b300befb`)
and a pinned AppImage (SHA-256
`4d9234d7f4fd4444faa028b5d43d6fad889fa490c3b9a9223969d581696d4344`).
Its AppDir offscreen create/reopen and synthetic scanner smoke passed; the
image builder validated the extracted file tree, codec dependency hashes and
LAME `fft.c` code fingerprint. These are
local packaging checks, using an older executable, not release qualification.
The current LAME package's `fft.c` carries a GPL-1+ classification in Ubuntu
copyright metadata. The [codec audit](mp3-flac-redistribution-gate.md) records
why that is a release blocker, not a new product-licence choice. The native
`.deb` uses system libraries and is not pinned to these copied bytes.

**Static-runtime follow-up:** The AppDir now includes seven full notice files
for the pinned runtime's musl, libfuse, squashfuse, zstd, zlib and additionally
observed mimalloc code. Their pinned hashes are checked on staging and in the
extracted AppImage. The [runtime notice audit](appimage-runtime-static-notices.md)
records exact-source evidence and the remaining build-version and LGPL
relinking gaps. The native `.deb` has no AppImage runtime notices.

**15 October runtime-notice follow-up:** The AppDir now stages the exact
[type-2 runtime 20251108 licence](https://github.com/AppImage/type2-runtime/blob/20251108/LICENSE)
under `usr/share/doc/oscillate/third-party/project/type2-runtime.txt`. The
checked-in copy has SHA-256
`aa154fc9070614bbe7921f89db11efd1dba7a1f3a41685958110e2230f9c0ca1`;
staging fails if it changes, and AppImage extraction verifies the notice again.
The runtime licence names static musl, libfuse, squashfuse, zstd and zlib code.
The later static-runtime follow-up above adds their full notices and mimalloc's;
exact compiled-source/build evidence and any LGPL replacement route remain
open. The native `.deb` does not contain the AppImage runtime or its notice.

The notice verifier now rejects duplicate ELF mappings, missing or altered
package-to-file notice associations, non-canonical notice paths, and copied
ELF symlinks. A local replay of the earlier Release Quick/scanner pair staged
117 ELF files (115 copied), 102 Ubuntu package copyright files and four
checked-in notices. This replay is packaging evidence for the changed script,
not a build from the current source revision or a distribution artefact.
With the pinned `appimagetool` 1.9.1 and type-2 runtime 20251108 inputs, the
new 52,906,488-byte local image has SHA-256
`5c40f9be28137ba3d7a78e53b714bd415268ed6429446562483d97edb5f26777`.
The builder compared its extracted 229-file payload to the staged tree
(SHA-256 `bcdd0ec34be634ddf70f5348c82fdb53289ca9c551da6b983cc741c8319f3f34`),
including the runtime notice, and checked the pinned embedded runtime bytes.
Offscreen create/reopen through `APPIMAGE_EXTRACT_AND_RUN=1` passed on this
developer host. A subsequent replay with the seven static dependency notices
produced a 236-file AppDir tree (SHA-256
`8c5f281a2620c7857a48331a399854a49c33c5b8897703f93fba665f7b5b6752`)
and a 52,918,776-byte AppImage (SHA-256
`48ede48e159aee0d8ef9ac6955b42984bb533e9e2e9766e8cfda635feeb24780`).
The builder verified the seven notice bytes in the extracted image; the
staged AppDir passed offscreen create/reopen and scanner smoke, and the new
AppImage passed extracted-run offscreen create/reopen. The sidecar's
`appdir_project_commit` names the **staging
checkout**, not an attested source revision of the earlier compiled ELF.

**QML dependency follow-up:** The native `.deb` now records the 23 QML files
actually embedded by `qml.qrc`, their `QtQuick` import, and the Ubuntu 26.04
[`qml6-module-qtquick`](https://packages.ubuntu.com/resolute/qml6-module-qtquick)
runtime package in `Depends`. The extracted-package smoke rejects a missing
QML runtime dependency. `dpkg-shlibdeps` alone cannot discover this import.
The local replay and still-open clean-host gate are recorded in the
[distribution audit](distribution-sbom-review.md). The AppDir stages no
separate QML module directory; the existing offscreen smoke does not establish
portable QML closure.

**Status:** 15 October 2026 local packaging evidence; **not distribution approval**. This change makes the existing native `.deb` and AppDir/AppImage prototypes carry the notice material already identified in the repository and installed Ubuntu package metadata. It does not decide the project's code or starter-sound licences, a per-binary SPDX expression, corresponding source/offer, or LGPL replacement procedure.

## Payload contract

- Both prototypes stage the exact checked-in `third_party/notices/vst3sdk/LICENSE.txt` and `vst3_pluginterfaces/LICENSE.txt` when the scanner helper is included. The nlohmann/json 3.11.3 development package's complete Ubuntu copyright file is checked in at `third_party/notices/nlohmann-json3-dev/copyright` and staged for both prototypes. Its source is the local extracted Ubuntu package, SHA-256 `a203b78242054dfccc14e558f699e2a9a5fc43e2b4d306ff94ebfccfcc013c38`; the package file includes source-file-specific terms, so the inventory does not compress it to a single MIT label. The release build must still prove it uses this exact header package/version.
- The native `.deb` stages `usr/share/doc/oscillate/third-party/inventory.json`, the project notices above and the `dpkg-shlibdeps` system dependency string. It bundles no Ubuntu shared libraries; their copyright material is supplied by their respective Ubuntu packages and needs release review. The JSON explicitly records that no copied ELF notice mappings exist.
- The AppDir stages each copied shared library and Qt plugin's installed Ubuntu `/usr/share/doc/<binary-package>/copyright` under `usr/share/doc/oscillate/third-party/ubuntu/`. Its inventory maps each copied ELF path and SHA-256 to an exact package/version and staged notice hash. Missing or changed copyright evidence fails staging. The AppImage builder now verifies that mapping again against the **extracted image**, after checking the payload tree.
- The inventory records unresolved product, source, replacement and runtime gates; it is evidence about included files, not a licence approval. The pinned AppImage runtime's top-level licence and seven static dependency notice files are staged. Their exact compiled-source and LGPL replacement review remains open.

## Local replay

Use a Release Qt Quick executable and its sibling scanner helper with no RPATH/RUNPATH. The native package script accepts a CMake cache to enforce the combined feature profile; the AppDir script requires the scanner helper. The exact outputs are ignored local artifacts.

```sh
python3 scripts/package-ubuntu-prototype.py \
  --binary build/ubuntu-package-prototype/apps/daw_quick/oscillate \
  --scanner-helper build/ubuntu-package-prototype/apps/daw_quick/oscillate_vst3_scan_helper \
  --cmake-cache build/ubuntu-package-prototype/CMakeCache.txt \
  --output-dir build/package-notices
python3 tests/packaging/smoke_ubuntu_package.py \
  --package build/package-notices/oscillate-quick-prototype_0.1.0~prototype1_amd64.deb \
  --inventory build/package-notices/oscillate-quick-prototype_0.1.0~prototype1_amd64.deb.inventory.json
python3 scripts/stage-appdir-prototype.py \
  --binary build/ubuntu-package-prototype/apps/daw_quick/oscillate \
  --scanner-helper build/ubuntu-package-prototype/apps/daw_quick/oscillate_vst3_scan_helper \
  --output build/package-notices.AppDir
PYTHONPATH=scripts python3 -m unittest discover -s scripts/packaging -p 'test_*.py' -v
```

On the earlier local combined Release input, extracted `.deb` payload and notice assertions passed. Two builds from the same older scanner-enabled ELF also produced the same `.deb` SHA-256 (`2e0907fb6747607acc85508a60c91ef6df89ac2c5ee6ec60b6658735f1d63daf`). That AppDir mapped **115** copied ELFs to **102** Ubuntu package copyright files and staged three project notices; every mapped hash passed. `tests/packaging/smoke_appdir.py` checked offscreen create/reopen, bundled Qt plugin/QML loading and the synthetic scanner helper. The new runtime-notice replay and AppImage hash are recorded above; neither replay binds its older executable to the current source revision.

The exact project and Ubuntu notice files are useful but insufficient for redistribution. Review source-file reachability for copied libraries, Qt third-party code, LAME's `fft.c` classification, LGPL source/replacement and the runtime's exact static-library closure against the final binary/payload. Perform clean Ubuntu X11/Wayland and audio checks before a release claim.
