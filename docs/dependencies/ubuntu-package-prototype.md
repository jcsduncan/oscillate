# Ubuntu 26.04 Qt Quick package prototype

**Later notice gate:** The packaging script now stages checked-in VST3 and nlohmann notices with an embedded inventory; an extracted-package smoke checks every file and hash. See the [notice payload gate](package-notice-payload.md). The historical package hashes below predate these added files.

For a checked, combined CMake input profile before producing another
local package, use the [native package feature gate](ubuntu-package-feature-gate.md).
The historical commands and artifact hashes below describe earlier prototype
configurations with different feature switches.

**Status:** Local build and extracted-package smoke passed on 15 October 2026.
**Not for distribution.** The `.deb` is deliberately named
`oscillate-quick-prototype`, contains a pre-release warning, and leaves the
code/content licence and LGPL source/replacement gates open. No AppImage is
claimed. This is a packaging mechanism and payload observation, not release
approval or a clean-machine qualification.

## Reproduce the local build

The tested build uses Ubuntu 26.04.1 x86-64, GCC 15.2, Ninja, CMake and
Ubuntu runtime libraries. Development headers/CMake files on this machine
come from extracted Ubuntu packages under a local cache (`$DEPS` below) and `/tmp`.
These paths are **build inputs only**; the resulting executable has no
`RPATH`/`RUNPATH` and resolves shared libraries from system locations. On
another machine, use equivalent Ubuntu development packages and remove the
explicit cache paths. The [packaging script](../../scripts/package-ubuntu-prototype.py)
accepts any matching clean-path Quick executable; it does not fetch or copy
third-party runtime libraries.

```sh
PKG_CONFIG_PATH=/tmp/oscillate-jack-dev/root/usr/lib/x86_64-linux-gnu/pkgconfig \
PKG_CONFIG_SYSROOT_DIR=/tmp/oscillate-jack-dev/root \
cmake -S . -B build/package-release2 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_SKIP_BUILD_RPATH=ON \
  -DBUILD_TESTING=OFF \
  -DOSCILLATE_BUILD_QUICK_UI=ON -DOSCILLATE_BUILD_CODEC_ADAPTER=ON \
  -DOSCILLATE_BUILD_EXPORT_RESAMPLER=ON \
  -DOSCILLATE_BUILD_VST3_SCANNER=OFF \
  -DQt6_DIR=$DEPS/qt/usr/lib/x86_64-linux-gnu/cmake/Qt6 \
  -Dnlohmann_json_DIR=$DEPS/nlohmann/usr/share/cmake/nlohmann_json \
  -DOSCILLATE_SNDFILE_INCLUDE_DIR=$DEPS/sndfile/root/usr/include \
  -DOSCILLATE_SNDFILE_LIBRARY=/usr/lib/x86_64-linux-gnu/libsndfile.so.1 \
  -DOSCILLATE_SAMPLERATE_INCLUDE_DIR=$DEPS/samplerate/usr/include \
  -DOSCILLATE_SAMPLERATE_LIBRARY=$DEPS/samplerate/usr/lib/x86_64-linux-gnu/libsamplerate.so
cmake --build build/package-release2 -j4 --target oscillate
python3 scripts/package-ubuntu-prototype.py \
  --binary build/package-release2/apps/daw_quick/oscillate \
  --output-dir build/package-artifacts
```

The script checks Ubuntu/x86-64, Qt Quick ELF identity, no runtime path,
absence of `Qt6Test`, and local shared-library resolution. It computes
Debian `Depends` with `dpkg-shlibdeps` against installed Ubuntu package
metadata, stages only the executable, desktop launcher and warning note,
and writes a JSON payload/dependency/QML-source inventory beside the `.deb`.
For the same input ELF and `SOURCE_DATE_EPOCH`, two consecutive package runs
produced identical SHA-256 hashes. The default epoch is zero; a release
builder can supply `SOURCE_DATE_EPOCH` explicitly. This is package-byte
repeatability for the **same binary**, not reproducibility of the C++ build.

## Inspected artifact

Local artifact:
`build/package-artifacts/oscillate-quick-prototype_0.1.0~prototype1_amd64.deb`.
SHA-256:
`60b3512a97a7f6dee1d9fb0d2bbead3a82fcbe8f91d987b43bcac92a7ff07495`.
It is generated under ignored `build/` and is not committed.

| Payload path | Purpose |
| --- | --- |
| `/usr/bin/oscillate` | 2,490,152-byte executable, SHA-256 `4dfc383f0abc0a17a36536195d16732715b79b51bde7366b7fb7fa566ae37546` |
| `/usr/share/applications/oscillate-quick-prototype.desktop` | Validated desktop launcher |
| `/usr/share/doc/oscillate-quick-prototype/README.prototype` | Local-only and unresolved-licence warning |

No Qt, codec, PipeWire, JACK, VST3, PFFFT or other `.so` is in this `.deb`.
`readelf` found 13 direct `DT_NEEDED` names: Qt Quick, Widgets, Qml, Gui,
Core; PipeWire; libsamplerate; OpenSSL Crypto; libsndfile; libstdc++, libm,
libgcc_s and libc. There is no Qt Test, JACK or VST3 SDK shared-library
link in this executable. The sidecar inventory records the 96 lines of
local `ldd` resolution, including indirect Qt modules and
libsndfile's FLAC/Vorbis/Ogg/Opus/mpg123/LAME codec libraries. Those files
are **system dependencies**, not package payload. The generated control
file has 12 package dependencies, including Ubuntu `libqt6quick6`,
`libqt6qml6`, `libqt6widgets6`, `libpipewire-0.3-0t64`, `libsndfile1`,
`libsamplerate0` and `libssl3t64`; inspect `dpkg-deb --field` for its exact
version constraints rather than copying this summary.

Source inspection finds only `import QtQuick` in the three bundled QML
files (`DawShell.qml`, `RoutingInspector.qml`, `WelcomeScreen.qml`). The QRC
also embeds the four generated starter WAVs. The sidecar records hashes of
all seven QRC source files, QML source files and their import names, but a
source list does not prove
the installed runtime closure. With the **extracted executable** under
`QT_QPA_PLATFORM=offscreen`, `QML_IMPORT_TRACE=1` resolved `QtQuick`,
`QtQml`, `QtQml.Models` and `QtQml.WorkerScript` from Qt resources;
`QT_DEBUG_PLUGINS=1` reported system `libqoffscreen.so` and ibus input
context. No Wayland or X11 platform run was performed for this artifact.

## Smoke and inspection commands

```sh
dpkg-deb --field build/package-artifacts/oscillate-quick-prototype_0.1.0~prototype1_amd64.deb
dpkg-deb --contents build/package-artifacts/oscillate-quick-prototype_0.1.0~prototype1_amd64.deb
dpkg-deb --extract build/package-artifacts/oscillate-quick-prototype_0.1.0~prototype1_amd64.deb build/package-smoke-root
desktop-file-validate build/package-smoke-root/usr/share/applications/oscillate-quick-prototype.desktop
env -u LD_LIBRARY_PATH QT_QPA_PLATFORM=offscreen \
  build/package-smoke-root/usr/bin/oscillate \
  --create build/package-smoke-project --smoke
env -u LD_LIBRARY_PATH QT_QPA_PLATFORM=offscreen \
  QT_DEBUG_PLUGINS=1 QML_IMPORT_TRACE=1 \
  build/package-smoke-root/usr/bin/oscillate \
  --open build/package-smoke-project --smoke
```

Both create and reopen returned `revision 4, 1 tracks; clip delegates: 1`
with exit code zero. The command uses an extracted payload without root
installation; it does not exercise package-manager install/upgrade/removal.

## Scanner-enabled follow-up

A second local prototype on 15 October 2026 used a Release Quick build with
`BUILD_TESTING=OFF`, `OSCILLATE_BUILD_VST3_SCANNER=ON`, and codec, source/output
resampler and convolution switches off. It uses the same system-library model
as the first prototype. The build's scanner helper is copied beside the Quick
executable; packaging it is explicit:

```sh
cmake --build build/package-quick-vst3 --target oscillate -j2
python3 scripts/package-ubuntu-prototype.py \
  --binary build/package-quick-vst3/apps/daw_quick/oscillate \
  --scanner-helper build/package-quick-vst3/apps/daw_quick/oscillate_vst3_scan_helper \
  --output-dir build/package-artifacts-vst3
```

The configured build used pinned VST3 SDK 3.8.1 sources and local Qt and
nlohmann/json development inputs; the helper must come from the **same build**
as the app. The script checks both ELFs for x86-64, absence of runtime paths,
absence of Qt Test and resolved local dependencies, and refuses a helper from
a different build directory. `dpkg-shlibdeps` receives
both binaries. The resulting `.deb` contains four files: the Quick executable,
the executable scanner helper, desktop launcher and prototype warning. It
does not contain VST3 plugins or SDK shared libraries. The sidecar inventory
records both binary hashes, direct dependencies and host `ldd` resolution.

The `.deb` SHA-256 was
`0f7e6dad25f62c0f6374723d4f6248360ef1074c3ac7b2e2ebae03e1c65c8d75`;
the Quick ELF was
`5a5d6212c08678c4e4bbd86f53affd223bb05370a7273fa7fc75a9a6d991afa1`,
and the helper was
`5e99ca19de23de5f6e18841772d6f3832e2addb076ce29f3c8f26a5a18ffc869`.
Two package runs with these exact binaries and the default source epoch yielded
the same package hash. The extracted executable created and reopened an
offscreen project, `desktop-file-validate` accepted the launcher, and the
extracted helper scanned the synthetic Loaded Gain fixture and returned the
expected `LDV3` protocol header. The scanner-enabled merged Quick suite passed
72/72 tests, including its verified-browser and routed-playback contracts.
This is local mechanism evidence; it does not test a clean-machine install,
third-party plugins, custom editors, sandbox containment or user-device audio.

After the Quick VST3 instrument path was merged, the scanner-enabled Release
build with `BUILD_TESTING=OFF` still packaged both app and helper. The updated
local `.deb` SHA-256 is
`f1b4413480f3d3e48ff2e072c4d6f163036a0afae11551b588410f5d564a4580`.
`desktop-file-validate` passed; the extracted executable created and reopened
an offscreen project, and the extracted helper scanned the synthetic Loaded
Gain bundle and returned the `LDV3` header. Neither ELF has an RPATH/RUNPATH
or Qt Test dependency. The merged scanner-enabled Quick suite passed 74/74.
This remains a local prototype with the same release gates below.

## Open packaging gates

1. Choose the project's code licence and the starter-sound content grant;
   complete the [distribution SBOM and licence review](distribution-sbom-review.md).
   The current package contains no full copyright file or Qt/LGPL source offer.
2. Define the release feature configuration. The first prototype turns VST3
   scanning **off**. The scanner-enabled follow-up includes its helper but
   does not yet establish full VST3 compatibility or the required codec/export
   feature set. Verify those capabilities in the selected release configuration
   and add future files deliberately.
3. Build from a clean source/dependency environment, eliminate `/tmp` and
   user-cache build inputs, install the `.deb` on a clean Ubuntu 26.04 target,
   and test the launcher, QML/platform plugin closure, device selection,
   import/export, upgrade and rollback under Wayland and X11.
4. Prove Qt and other LGPL library replacement, source/offer, notices and
   package copyright behavior against the exact artifact. System package
   dependency metadata alone does not close these obligations.
5. AppImage remains unbuilt. This host has `mksquashfs` but no
   `appimagetool`/`linuxdeployqt` or reviewed AppImage runtime. Do not pull a
   runtime or copy the complete local Qt tree without dependency, notice and
   library-replacement review. Build a separate AppImage prototype after
   those inputs are selected, then inspect its actual bundled file closure.
