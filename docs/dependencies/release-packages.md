# Release packages

Each release ships two Ubuntu 26.04 x86-64 packages, built by the
[packaging workflow](../../.github/workflows/packaging.yml) in an
`ubuntu:26.04` container and attached to the GitHub release:

| File | Form |
| --- | --- |
| `oscillate_<version>_amd64.deb` | Native package. Installs `/usr/bin/oscillate` and depends on Ubuntu's shared libraries. |
| `Oscillate-<version>-x86_64.AppImage` | Image with Qt and its other libraries; glibc and `libproxy` (`libproxy1v5`, on every Ubuntu desktop through glib-networking) come from the host. |
| `oscillate_<version>_amd64.deb.inventory.json` | `.deb` sidecar: package hash, features, `Depends` and payload. |
| `Oscillate-<version>-x86_64.AppImage.inventory.json` | AppImage sidecar: image hash, features and pinned tool and runtime. |
| `Oscillate.AppDir.inventory.json` | The AppImage's payload: every file with hash and mode, and each copied ELF's Ubuntu package. |
| `SHA256SUMS` | Checksums of everything above. |

Release Please dispatches the workflow with the new tag. A manual run with a
`tag` input rebuilds that release's packages and replaces them. Pull requests
run it when a packaging input changes (`packaging` in
[`changes.py`](../../scripts/ci/changes.py)). It is not a required check.

## Features in each package

Both packages are static Release builds of the Qt Quick app with
`BUILD_TESTING=OFF` and no `RPATH`. The feature switches are fixed by the
`package-deb` and `package-appimage` CMake presets and enforced by
[`package_profiles.py`](../../scripts/package_profiles.py); a package
cannot be made from a build configured differently.

| Feature | `.deb` | AppImage |
| --- | --- | --- |
| Qt Quick app, built-in instruments and effects, PipeWire output, WAV import and export | yes | yes |
| VST3 instruments and effects (pinned SDK `v3.8.1_build_84`, out-of-process scanner) | yes | yes |
| JACK output and real-time export | yes | yes |
| Source and export sample-rate conversion (libsamplerate) | yes | yes |
| AIFF, FLAC and MP3 import and export (libsndfile) | yes | **no** |
| Convolution reverb | **no** | **no** |

- **Codec adapter, AppImage only.** The AppImage would have to bundle
  Ubuntu's libsndfile with its `libmp3lame.so.0`, and Ubuntu classifies that
  library's `fft.c` GPL-1+ ([MP3 and FLAC gate](mp3-flac-redistribution-gate.md)).
  Redistributing it is undecided under G8
  ([#93](https://github.com/jcsduncan/oscillate/issues/93)), so the AppImage
  is built without the codec adapter and
  [`stage-appdir.py`](../../scripts/stage-appdir.py) refuses to stage
  `libmp3lame.so.0`. The `.deb` copies no codec: it depends on Ubuntu's
  `libsndfile1`, which brings its own codec packages.
- **Convolution reverb.** [ADR-0020](../decisions/0020-convolution-reverb.md)
  keeps the convolution core out of release builds until its graph admission
  and multi-instance policy are qualified.
- **JACK.** The `.deb` links Ubuntu's `libjack.so.0` through its
  `libjack-jackd2-0 | libjack-0.125` dependency, so PipeWire's JACK
  replacement works where it is installed. The AppImage bundles JACK2's client
  library, which reaches a running `jackd`; with only PipeWire running, use
  the PipeWire output.
- **PipeWire.** The AppImage bundles the PipeWire client library, which loads
  its SPA support plugins from the host's PipeWire installation.

Each package's `README` lists the same features and exclusions.

## Contents

| Path | `.deb` | AppImage |
| --- | --- | --- |
| App and VST3 scanner helper | `/usr/bin/` | `usr/bin/`, started by `AppRun` |
| Desktop entry and icon | `/usr/share/applications/`, `/usr/share/icons/hicolor/scalable/apps/` | image root |
| Qt platform plugins (offscreen, xcb, wayland, ibus) | from Ubuntu | `usr/plugins/` |
| Shared libraries | from Ubuntu, named in `Depends` | `usr/lib/`, the observed `ldd` closure except glibc and `libproxy` |
| Licence | `/usr/share/doc/oscillate/copyright` (DEP-5, GPL-3+) | `usr/share/doc/oscillate/LICENSE` |
| Feature list | `/usr/share/doc/oscillate/README` | `usr/share/doc/oscillate/README` |
| Third-party notices | `/usr/share/doc/oscillate/third-party/` | `usr/share/doc/oscillate/third-party/` |
| SBOM | `/usr/share/doc/oscillate/sbom.spdx.json` | `usr/share/doc/oscillate/sbom.spdx.json` |

**Notices.** [`package_notices.py`](../../scripts/package_notices.py) stages
the notices for code compiled into the executables (nlohmann/json, PFFFT, the
VST3 SDK and its `pluginterfaces`) and, in the AppImage, the type-2 runtime's
licence and the seven notices for the code it links statically
([runtime notices](appimage-runtime-static-notices.md)). For every library
and plugin the AppImage copies, it stages the owning Ubuntu package's
`copyright` file. `third-party/inventory.json` maps each copied file and hash
to its package, version and notice; staging, extraction and the smoke tests
reject a missing or changed notice.

**SBOM.** [`package_sbom.py`](../../scripts/package_sbom.py) writes an SPDX
2.3 JSON document listing every file in the package with SHA-1 and SHA-256,
except the SBOM itself and the AppImage's `.DirIcon` symlink. It also lists
the compiled-in components, each Ubuntu package the AppImage copies from,
each Ubuntu package the `.deb` depends on, and the AppImage runtime. The
project is declared GPL-3.0-or-later. Third-party licences are
`NOASSERTION`, with a pointer to the shipped notice: the SBOM records
provenance and makes no licence conclusion, which belongs to G8.

## Runtime dependencies

[`runtime-dependencies.json`](runtime-dependencies.json) lists, for each
package, the Ubuntu 26.04 packages it needs at run time:

| Field | `.deb` | AppImage |
| --- | --- | --- |
| `system` | The `Depends` field without versions; alternatives as `a \| b`. | Packages left to the host: `libc6` and `libproxy1v5`. |
| `bundled` | Empty: the `.deb` copies no library. | The package owning each library and Qt plugin copied into `usr/`. |

The lists are declared in each profile in
[`package_profiles.py`](../../scripts/package_profiles.py) and the file is
generated from them with `python3 -I scripts/package_dependencies.py write`;
a unit test fails when the file is out of date. The packaging workflow runs
`package_dependencies.py verify` on each built package's sidecar and fails on
a package that is declared but missing, or present but not declared.

## Checks

| Stage | Check |
| --- | --- |
| Configure | CMake verifies the VST3 SDK manifest, interface headers and notices by digest. |
| Package | Profile switches, source tree and version (a release tag must be `v<project version>`); x86-64 ELFs with no `RPATH` and no Qt Test; every library resolves. |
| Runtime dependencies | [`package_dependencies.py`](../../scripts/package_dependencies.py) `verify`: the `.deb`'s `Depends`, and the AppDir's copied and host packages, match the profile. |
| `.deb` | [`smoke_ubuntu_package.py`](../../tests/packaging/smoke_ubuntu_package.py): payload against the sidecar, notices, SBOM, QML runtime dependency, desktop entry. |
| AppDir | [`smoke_appdir.py`](../../tests/packaging/smoke_appdir.py): payload tree, notices, SBOM, no MP3 encoder, app startup with the Qt platform plugin loaded from the AppDir and nothing loaded from the host's Qt plugin directory, scanner helper reply, loader picking bundled libraries. |
| AppImage | [`build-appimage.py`](../../scripts/build-appimage.py): pinned appimagetool 1.9.1 and type-2 runtime 20251108 by SHA-256, runtime bytes unchanged outside appimagetool's MD5 slot, extracted payload, notices and SBOM. |
| Clean container | [`install-smoke.sh`](../../tests/packaging/install-smoke.sh) in a fresh `ubuntu:26.04`: install (`apt-get install` for the `.deb`; for the AppImage, only `libproxy1v5` is added to the base image), launch with `--check-startup` to create and reopen a project offscreen, uninstall, and check that nothing is left behind. |

The clean-container launch is offscreen with no audio server. Without FUSE
the AppImage runtime extracts the image under `TMPDIR` and keeps it as a
cache, so the AppImage check allows only that directory to remain. X11, Wayland,
real audio devices and third-party plug-ins are not covered by CI.

## Build locally

The packaging steps need the workflow's packages (see `Install dependencies`
in the workflow) and a tree from the matching preset:

```sh
scripts/ci/fetch-vst3-sdk.sh ~/.cache/vst3sdk
cmake --preset package-deb -DOSCILLATE_VST3_SDK_ROOT=$HOME/.cache/vst3sdk
scripts/build.sh build/package-deb --target oscillate oscillate_vst3_scan_helper
python3 -I scripts/package-deb.py --build-dir build/package-deb \
  --output-dir build/packages --version-suffix local
cmake --preset package-appimage -DOSCILLATE_VST3_SDK_ROOT=$HOME/.cache/vst3sdk
scripts/build.sh build/package-appimage --target oscillate oscillate_vst3_scan_helper
python3 -I scripts/stage-appdir.py --build-dir build/package-appimage \
  --output build/appdir/Oscillate.AppDir --version-suffix local
python3 -I scripts/build-appimage.py --appdir build/appdir/Oscillate.AppDir \
  --inventory build/appdir/Oscillate.AppDir.inventory.json --output-dir build/packages
```

## Open under G8

Distribution approval belongs to [#93](https://github.com/jcsduncan/oscillate/issues/93):
the starter-sound grant, a per-library licence and source review, and the
LGPL source offer and replacement route for the Qt, JACK and other LGPL
libraries the AppImage bundles. The AppImage's library list is the observed
closure, not a curated portability list.
