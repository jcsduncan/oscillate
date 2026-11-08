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
| `Oscillate-<version>-x86_64.AppImage.NOTICES.txt` | The AppImage's third-party notices and licence texts in one file. |
| `Oscillate-<version>-x86_64.AppImage.SOURCE-OFFER.txt` | The AppImage's written source offer, with a link to the exact source of every library and runtime component it bundles. |
| `Oscillate.AppDir.inventory.json` | The AppImage's payload: every file with hash and mode, each copied ELF's Ubuntu package and source package, its notices and its source offer. |
| `SHA256SUMS` | Checksums of everything above. |

The release tag that Release Please pushes starts the workflow. A manual run with a
`tag` input rebuilds that release's packages and replaces them. Pull requests
run it when a packaging input changes (`packaging` in
[`changes.py`](../../scripts/ci/changes.py)). It is not a required check.

Once the packages are attached, the workflow starts the Pages workflow,
which rebuilds the signed [apt repository](../install.md) from the `.deb` of
the newest three releases. Pages publishes no repository while the
`APT_GPG_PRIVATE_KEY` secret is unset.

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
| AIFF, FLAC and MP3 import and export (libsndfile, libmpg123) | yes | yes |
| Convolution reverb | **no** | **no** |

- **Codecs.** The codec adapter links Ubuntu's libsndfile, which links
  LAME (`libmp3lame.so.0`) for MP3 export and mpg123, FLAC, Ogg, Vorbis and
  Opus for the rest, and links mpg123 (`libmpg123.so.0`) itself for MP3
  import. The AppImage bundles all of them, with their notices and a source
  offer ([MP3 and FLAC gate](mp3-flac-redistribution-gate.md)). The `.deb`
  copies no codec: it depends on Ubuntu's `libsndfile1`, which brings its own
  codec packages, and `libmpg123-0t64`.
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
| Source offer | none: the `.deb` bundles no library | `usr/share/doc/oscillate/SOURCE-OFFER.txt` and `third-party/sources.json` |
| Feature list | `/usr/share/doc/oscillate/README` | `usr/share/doc/oscillate/README` |
| Third-party notices | `/usr/share/doc/oscillate/third-party/` | `usr/share/doc/oscillate/third-party/` |
| SBOM | `/usr/share/doc/oscillate/sbom.spdx.json` | `usr/share/doc/oscillate/sbom.spdx.json` |

**Notices.** [`package_notices.py`](../../scripts/package_notices.py) stages
the notices for code compiled into the executables (nlohmann/json, PFFFT, the
VST3 SDK and its `pluginterfaces`) and, in the AppImage, the type-2 runtime's
licence and the seven notices for the code it links statically
([runtime notices](appimage-runtime-static-notices.md)). For every library
and plugin the AppImage copies, it stages the owning Ubuntu package's
`copyright` file, and under `third-party/common-licenses/` the full text of
each `/usr/share/common-licenses` licence those files cite.
`third-party/NOTICES.txt` joins every notice into one file, which the
AppImage build also publishes as a release asset. `third-party/inventory.json`
maps each copied file and hash to its package, version and notice; staging,
extraction and the smoke tests reject a missing or changed notice, a cited
licence text that is not staged, or a `NOTICES.txt` that differs from the
staged files.

The `.deb` ships the same compiled-in notices and its own `NOTICES.txt`; the
libraries it uses come from Ubuntu packages that install their own
`copyright` files.

**Source offer.** [`package_sources.py`](../../scripts/package_sources.py)
writes the AppImage's `SOURCE-OFFER.txt`: a written offer, valid for three
years, to supply the corresponding source on request, and a link to each
exact source:

- every Ubuntu source package the AppImage copies a library or Qt plugin
  from (Qt, JACK, LAME, libsndfile and the rest of the closure), at the exact
  source version `dpkg-query` reports, linked to that version's `.dsc` on
  Launchpad. The `.dsc` names and hashes the upstream and Ubuntu tarballs;
  `dget <link>` downloads and checks them;
- the type-2 runtime's source tag, and the pinned libfuse 3.15.0 and
  squashfuse 0.5.2 archives it links statically, with their SHA-256;
- Oscillate's own source at the commit the package was built from.

Launchpad keeps every published source version, so the links stay valid
after Ubuntu moves on. `third-party/sources.json` holds the same entries, and
the AppDir sidecar copies them. The offer also says how to run the app with a
modified library: extract the image, replace the library in `usr/lib/`, and
start `AppRun`.

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
| `system` | The `Depends` field without versions; alternatives as `a \| b`. | The host packages the app's libraries link against and the AppDir leaves out: `libc6` and `libproxy1v5`. |
| `bundled` | Empty: the `.deb` copies no library. | The package owning each library and Qt plugin copied into `usr/`. |
| `conditional` | Empty: `Depends` brings them. | Host packages loaded at run time, not linked, with when they are needed: `libspa-0.2-modules`, whose SPA plugins the bundled PipeWire client loads for PipeWire output. |

The lists are declared in each profile in
[`package_profiles.py`](../../scripts/package_profiles.py) and the file is
generated from them with `python3 -I scripts/package_dependencies.py write`;
a unit test fails when the file is out of date. The packaging workflow runs
`package_dependencies.py verify` on each built package's sidecar and fails on
a `system` or `bundled` package that is declared but missing, or present but
not declared. The sidecars record linked libraries only, so `conditional`
packages are declared but not checked.

## Checks

| Stage | Check |
| --- | --- |
| Configure | CMake verifies the VST3 SDK manifest, interface headers and notices by digest. |
| Package | Profile switches, source tree and version (a release tag must be `v<project version>`); x86-64 ELFs with no `RPATH` and no Qt Test; every library resolves. |
| Runtime dependencies | [`package_dependencies.py`](../../scripts/package_dependencies.py) `verify`: the `.deb`'s `Depends`, and the AppDir's copied and host packages, match the profile; every package the AppDir copies from has a staged notice and a source-offer entry with the expected Launchpad link, and the offer lists the runtime sources. |
| `.deb` | [`smoke_ubuntu_package.py`](../../tests/packaging/smoke_ubuntu_package.py): payload against the sidecar, notices, SBOM, QML runtime dependency, desktop entry. |
| AppDir | [`smoke_appdir.py`](../../tests/packaging/smoke_appdir.py): payload tree, notices, source offer, SBOM, app startup with the Qt platform plugin loaded from the AppDir and nothing loaded from the host's Qt plugin directory, scanner helper reply, loader picking bundled libraries (libsndfile and LAME among them). |
| AppImage | [`build-appimage.py`](../../scripts/build-appimage.py): pinned appimagetool 1.9.1 and type-2 runtime 20251108 by SHA-256 (downloaded with retries on server errors and timeouts, and cached between CI runs), runtime bytes unchanged outside appimagetool's MD5 slot, extracted payload, notices, source offer and SBOM. |
| Source links | `package_sources.py check-urls`: every link in the AppImage's source offer answers. |
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
