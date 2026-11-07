# Local Qt Quick AppDir prototype

**Status:** Ubuntu 26.04 x86-64 staging experiment, 15 October 2026. **Do not distribute.** This is an [AppDir](https://docs.appimage.org/reference/appdir.html); a later [local AppImage prototype](appimage-local-prototype.md) pins the tool and runtime and verifies the extracted payload. The package's product licence, bundled-library notices/source and replacement route have not been approved. The companion [combined `.deb` audit](quick-scanner-codec-combined-audit.md) describes the same feature configuration, but its system-library payload is materially different.

## Inputs and layout

At repository commit `b3704e5`, a GCC 15.2 Release build used `BUILD_TESTING=OFF`, Quick UI, VST3 scanner, libsndfile codec adapter and export libsamplerate **on**; legacy Widgets UI, source resampler and convolution **off**. The build used Qt 6.10.2, PipeWire 1.6.2, OpenSSL 3.5.5, nlohmann/json 3.11.3 and pinned VST3 SDK `v3.8.1_build_84`. JACK headers were not found. `scripts/stage-appdir-prototype.py` takes the sibling Quick executable and scanner helper, checks x86-64 and rejects either ELF's RPATH/RUNPATH, then stages a root desktop entry, generated SVG icon, `AppRun`, `usr/bin/qt.conf`, the two executables, selected Qt plugins and their observed `ldd` shared-library closure. It refuses an existing output and only writes under the checkout's ignored `build/` directory.

`AppRun` points `LD_LIBRARY_PATH` and Qt plugin paths into this AppDir. `qt.conf` gives Qt relative `Prefix = ..`, `Plugins = plugins` and `QmlImports = qml`; the observed QML imports resolve from Qt resources, not host QML files. This follows Qt's [plugin-path guidance](https://doc.qt.io/qt-6.10/linux-deployment.html) and [AppDir layout guidance](https://docs.appimage.org/packaging-guide/manual.html), without claiming that X11/Wayland or a portable AppImage works.

The local output at `build/oscillate-prototype.AppDir` has **122 regular payload files**, including **2 project executables, 4 Qt plugins and 111 copied shared libraries**, totalling 128.3 MiB. The sorted file/hash/size/mode list has tree SHA-256 `4d0383a70e2b5cd1ec398e9302e2df1c2d1e503bc2facfa814463f380ab93158`; a second staging run from identical inputs produced the same tree hash. This is an AppDir content-and-mode manifest hash, **not** an AppImage or archive hash. The ignored `build/oscillate-prototype.AppDir.inventory.json` lists each ELF's SHA-256, `DT_NEEDED`, source path, Ubuntu owning package/version and installed copyright-file path, plus every payload file/hash and deliberate host dependencies. Every staged ELF reported an empty RPATH/RUNPATH. The four staged Qt plugins are `platforms/libqoffscreen.so`, `libqxcb.so`, `libqwayland.so` and `platforminputcontexts/libibusplatforminputcontextplugin.so`, all from the installed Qt 6.10.2 packages. No QML plugin `.so` was loaded during the offscreen run.

The script deliberately leaves `libc`, `libm`, `libmvec` and `libresolv` on the target Ubuntu system. It also leaves `libproxy.so.1` and `libpxbackend-1.0.so` on the host: the local `libproxy` carries an absolute RUNPATH to `/usr/lib/x86_64-linux-gnu/libproxy`. The first staging attempt failed on that path rather than silently copying it. The inventory records those six observed host dependencies and their package versions. The **111 copied libraries are not a reviewed AppImage inclusion list**: graphics and other system integration libraries still need a curated keep/exclude decision under [AppImage's portability guidance](https://docs.appimage.org/introduction/concepts.html). A clean Ubuntu 26.04 install may resolve a different optional closure.

## Local checks and replay

With the exact local dependency paths from the [combined build audit](quick-scanner-codec-combined-audit.md), configure a Release Quick/scanner/codec build with `CMAKE_SKIP_BUILD_RPATH=ON` and `BUILD_TESTING=OFF`, then build `oscillate` and `oscillate_vst3_scan_helper` at `-j1`. The following commands stage and verify it; the separate test-only fixture build does not alter the release-style executable:

```sh
python3 scripts/stage-appdir-prototype.py \
  --binary build/appdir-quick/apps/daw_quick/oscillate \
  --scanner-helper build/appdir-quick/apps/daw_quick/oscillate_vst3_scan_helper \
  --output build/oscillate-prototype.AppDir
cmake -S . -B build/appdir-fixture -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DOSCILLATE_BUILD_QUICK_UI=OFF \
  -DOSCILLATE_BUILD_VST3_SCANNER=ON \
  -DOSCILLATE_VST3_SDK_ROOT=/path/to/pinned/vst3sdk \
  -Dnlohmann_json_DIR=/path/to/nlohmann_json/cmake
cmake --build build/appdir-fixture --target oscillate_vst3_fixture_Valid -j1
python3 tests/packaging/smoke_appdir.py \
  --appdir build/oscillate-prototype.AppDir \
  --inventory build/oscillate-prototype.AppDir.inventory.json \
  --fixture build/appdir-fixture/tests/plugins/fixtures/Valid.vst3
desktop-file-validate build/oscillate-prototype.AppDir/oscillate-prototype.desktop
```

The smoke verifies the complete payload hash list and tree hash, no staged ELF runtime paths, offscreen `AppRun --create` and `--open`, QML's `QtQuick` resource import, `libqoffscreen` and ibus loaded **inside** the AppDir, staged Qt Quick/PipeWire/libsndfile/libsamplerate/OpenSSL loader resolution, and the staged helper's successful `LDV3` response for the synthetic VST3 fixture. An independent `LD_DEBUG=libs` run also reported `libQt6Quick.so.6` initialization from the AppDir. `desktop-file-validate` passed. These checks ran with no inherited `LD_LIBRARY_PATH`. The development UI's `--smoke-vst3-browser` hook requires Qt Test input and rejected this `BUILD_TESTING=OFF` executable, so it is not counted as package browser evidence.

## Why no release claim follows

The later [local AppImage experiment](appimage-local-prototype.md) has pinned, hashed tool and runtime inputs and a verified image. It does not change this AppDir's licence status. The staged AppDir copies 111 Ubuntu libraries but has only **package copyright paths**, not a file-by-file approved licence expression, complete notice bundle, corresponding source/offer or demonstrated LGPL replacement instructions. Qt, libsndfile, mpg123 and LAME need particular review; the [codec audit](mp3-flac-redistribution-gate.md) leaves the exact LAME `fft.c` classification unresolved. Product code and starter-sound grants are also open. Compressing this directory does not close those gates.

The staged XCB and Wayland plugins have **not** been exercised on clean X11/Wayland Ubuntu 26.04 targets. Graphics-driver exclusions, QML/platform plugin closure, font/input methods, physical audio device use, upgrade/rollback and third-party plugin compatibility remain unqualified. A release AppImage must first pin its runtime/tool, decide and test the file inclusion list, assemble notices and source/replacement paths, and repeat the loader and workflow checks against the exact resulting image. This prototype makes those next checks concrete; it does not authorize distribution.
