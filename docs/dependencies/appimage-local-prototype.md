# Local Qt Quick AppImage prototype

**Later static-runtime notice gate:** A subsequent local image stages and
verifies the [runtime's full static-dependency notice set](appimage-runtime-static-notices.md),
including mimalloc, which is named in the pinned runtime Makefile but absent
from its top-level licence. The earlier image and hashes below predate that
change. Exact Alpine package/build provenance and static libfuse LGPL
relinking remain open.

**Later notice gate:** The packaging scripts now stage checked-in VST3 and nlohmann notices, and the AppDir stage maps each copied ELF to its exact installed Ubuntu copyright file. The AppImage builder verifies the extracted notice mapping. See the [notice payload gate](package-notice-payload.md). The historical image and hashes below predate that change and do **not** contain these added files.

**Status:** Ubuntu 26.04 x86-64 packaging experiment, 15 October 2026. **Do not distribute.** This image is a reproducible *local* test artifact from one host and configuration. The dependency licences, notices, source/replacement routes and clean-host compatibility have not passed the [distribution review](distribution-sbom-review.md).

## Pinned inputs and provenance

The input [AppDir](appdir-local-prototype.md) was rebuilt at project commit `16397d9a5d9f7d5a5d7fb83504cdcb58d99ed067` with GCC 15.2, Release, `BUILD_TESTING=OFF`, Qt Quick, VST3 scanner, libsndfile codec adapter and export libsamplerate enabled. Legacy Widgets, source resampler and convolution were disabled. It has 122 regular payload files: two project executables, four selected Qt plugins and 111 copied shared libraries. `build/oscillate-prototype.AppDir.inventory.json` records exact bytes, modes, ELF `DT_NEEDED`, source package/version/copyright path and deliberate host-library exclusions. Its content-and-mode tree SHA-256 is `4c886a7f5b99be79bf7c7a38defab7164b257486b60055a8240bc14c4955d067`.

| Build input | Immutable source | GitHub release SHA-256 | Source tag commit |
| --- | --- | --- | --- |
| `appimagetool-x86_64.AppImage` | [1.9.1 release](https://github.com/AppImage/appimagetool/releases/tag/1.9.1) | `ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0` | `8c8c91f762b412a19f4e8d2c4b35afb98f2d7c81` |
| `runtime-x86_64` | [20251108 release](https://github.com/AppImage/type2-runtime/releases/tag/20251108) | `2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d` | `dd6cebedcbddde9c82f89b011e8e1d40b6e43868` |

The release SHA-256 values were checked against GitHub's release asset metadata and against the downloaded files. Both were inspected as x86-64 static PIE ELFs without `DT_NEEDED` or RPATH. The tool reports git version `8c8c91f`, build 296. The [tool's source licence](https://github.com/AppImage/appimagetool/blob/1.9.1/LICENSE) is MIT with an explicit reminder that it does not license image contents. The [runtime licence](https://github.com/AppImage/type2-runtime/blob/20251108/LICENSE) identifies MIT runtime code plus statically linked musl, libfuse, squashfuse, zstd and zlib under their own terms. The binary release digest establishes byte provenance through GitHub/TLS; no independent signed-source or reproducible-upstream-binary attestation was performed. Runtime notices and any LGPL libfuse source/replacement obligations remain under review.

## Offline build and validation

Download the *versioned* assets, verify their hashes, and place them in the ignored `build/appimage-inputs/` directory. `scripts/build-appimage-prototype.py` accepts only those hard-coded hashes, uses `--runtime-file` so the tool cannot fetch a moving runtime, and refuses an existing or out-of-`build/` output. It verifies every AppDir payload file before building and every extracted file after building. `appimagetool` adds a root `.DirIcon` symlink in a temporary **copy** of the AppDir; the source inventory remains unchanged. The embedded runtime matches the pinned runtime except for appimagetool's 16-byte image-MD5 slot at byte offsets `932096..932111`. The script records that observation and checks no other runtime byte changed.

```sh
curl -fL -o build/appimage-inputs/appimagetool-1.9.1-x86_64.AppImage \
  https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
curl -fL -o build/appimage-inputs/runtime-20251108-x86_64 \
  https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64
chmod +x build/appimage-inputs/appimagetool-1.9.1-x86_64.AppImage
python3 scripts/build-appimage-prototype.py \
  --appdir build/oscillate-prototype.AppDir \
  --inventory build/oscillate-prototype.AppDir.inventory.json \
  --appimagetool build/appimage-inputs/appimagetool-1.9.1-x86_64.AppImage \
  --runtime build/appimage-inputs/runtime-20251108-x86_64 \
  --output build/oscillate-prototype.AppImage
```

`SOURCE_DATE_EPOCH=0` and `ARCH=x86_64` are fixed by the script. Two local runs with the same staged input, pinned tool/runtime and different output basenames produced identical 52,582,904-byte AppImages with SHA-256 `035b8e3da177b319a914cf7a3523065eb5a1a8e12056447fe965bdddd9c21b3f`. This proves repeatability on this host, **not** cross-host reproducibility or a stable release build. The sidecar `build/oscillate-prototype.AppImage.inventory.json` records the image hash, AppDir tree hash and input hashes. Neither artifact is committed.

The following local checks passed:

- Image type-2 marker, 944,632-byte runtime offset, exact pinned runtime outside its MD5 slot, and `--appimage-extract` payload comparison with all 122 inventoried files, sizes, hashes and modes. The extracted `.DirIcon` points only to `oscillate-prototype.svg`.
- With no inherited `LD_LIBRARY_PATH` and `QT_QPA_PLATFORM=offscreen`, the image's normal **FUSE-mounted** runtime executed `--create` and `--open` on a disposable project. Both reported `revision 4, 1 tracks`; Qt loaded `libqoffscreen.so` from `/tmp/.mount_.../usr/plugins`, and the `QtQuick` resource import resolved. The same create/reopen checks passed with `APPIMAGE_EXTRACT_AND_RUN=1`; that route loaded `libqoffscreen.so` and the ibus plugin from `/tmp/appimage_extracted_.../usr/plugins`.
- `tests/packaging/smoke_appdir.py` passed against the inventoried source AppDir, checking Qt/PipeWire/codec/resampler/OpenSSL loader resolution and the sibling scanner. A separately extracted image's `usr/bin/oscillate_vst3_scan_helper` returned an `LDV3` response for the synthetic `Valid.vst3` fixture. The fixture is test-only and is **not** in the image.
- A one-byte-corrupted runtime was rejected before output publication. `readelf -d` on the extracted Quick ELF showed no RPATH/RUNPATH.

Both normal FUSE mounting and the extraction fallback passed on this developer host. `appimagetool` warned that AppStream metadata is missing. Its automatically created `.DirIcon` is a symlink and has no separate copied-library obligation.

## Open release gates

This payload still copies 111 host libraries without a file-by-file approved SPDX/notice/source package, plus Qt and codec licence uncertainty already identified in the [AppDir audit](appdir-local-prototype.md) and [MP3/FLAC gate](mp3-flac-redistribution-gate.md). The product licence and starter-sound grant remain unsettled. An extracted AppDir exists, but a **modified LGPL library replacement and execution** test has not been run. The bundled type-2 runtime adds its static dependency obligations; no legal conclusion follows from the local build.

Only one developer host and offscreen Qt were exercised. X11, Wayland, graphics drivers, physical PipeWire/JACK audio, FUSE on other hosts, clean Ubuntu 26.04 installs, third-party VST3s, update/rollback and actual user project workflows remain unqualified. `libc`, `libm`, `libmvec`, `libresolv`, `libproxy` and its backend are intentional host dependencies in this staging experiment. Inclusion/exclusion and package closure must be curated and retested for a release. This local image is useful packaging evidence, not a release candidate.
