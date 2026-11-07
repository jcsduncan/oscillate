# Ubuntu native package feature gate

**Status:** Local packaging input contract, not a distribution approval.

The `ubuntu-package-prototype` CMake preset fixes one production-like Qt Quick
configuration: Release, tests and legacy Widgets UI off; Quick UI, VST3 scanner,
codec adapter, source-rate converter and export-rate converter on. The optional
convolution core remains off because its graph admission and packaging review
are unfinished. `CMAKE_SKIP_BUILD_RPATH=ON` keeps local dependency paths out of
the executable. System Ubuntu shared libraries supply the runtime closure.

Use `CMakeUserPresets.json` or command-line CMake variables for local Qt,
VST3 SDK, codec and PipeWire development paths. These paths are build inputs,
not package contents. After building `oscillate`, run:

```sh
python3 scripts/package-ubuntu-prototype.py \
  --binary build/ubuntu-package-prototype/apps/daw_quick/oscillate \
  --scanner-helper build/ubuntu-package-prototype/apps/daw_quick/oscillate_vst3_scan_helper \
  --cmake-cache build/ubuntu-package-prototype/CMakeCache.txt \
  --output-dir build/ubuntu-package-artifacts
```

The `--cmake-cache` gate checks every required switch, the CMake source root,
and that the app and helper are siblings of that build's cache. The existing
ELF checks then reject x86-64 mismatches, RPATH/RUNPATH, Qt Test linkage and
unresolved host dependencies. `dpkg-shlibdeps` computes native package
dependencies from **both** executables. The inventory sidecar records the
verified feature switches, ELF hashes, package hash, direct `DT_NEEDED` names,
host `ldd` observation and QML/QRC source hashes. It cannot prove that a
binary placed at the expected path was built from that cache; reproducible
source-build attestation and a clean-host installation remain separate gates.

The flag is opt-in so earlier local prototype builds can still be inspected.
Any candidate package for release review must pass it and use the checked
feature profile. The package remains named `oscillate-quick-prototype` and
contains an explicit non-distribution note. Code and starter-sound licences,
compiled-header notices, Qt/LGPL obligations, the MP3/LAME interpretation,
Wayland/X11 and physical audio qualification remain open. Passing this gate
does not authorize publication.

## Local verification (15 October 2026)

An out-of-tree build using the preset values, GCC 15.2, the pinned SDK and
locally staged Ubuntu development headers produced both ELFs. The package
contains only the app, scanner helper, desktop entry and prototype warning.
`dpkg-shlibdeps` declared `libqt6quick6`, `libqt6qml6`, `libqt6widgets6`,
`libpipewire-0.3-0t64`, `libsndfile1`, `libsamplerate0`, `libssl3t64` and C/C++
runtime packages with their generated ABI floors. It does not bundle these
system libraries. Two package runs from the same ELFs and default epoch had
identical SHA-256
`946837e41d8bc17cbb4303612d51e484a099cc09402d66a4a9b009a2d17231f5`.
This checks package-byte repeatability from fixed inputs, not full C++ build
reproducibility.

`desktop-file-validate` passed on the extracted launcher. The extracted app
loaded under `QT_QPA_PLATFORM=offscreen` and `QT_QUICK_BACKEND=software` for
`--create` and `--open` of a disposable project without startup errors;
the create path wrote a generation manifest and `CURRENT`. The current
production executable has no `--smoke` switch, so the runs were deliberately
terminated after three seconds (`timeout` status 124). This is an offscreen
startup and persistence-open check, not an automated workflow, clean-machine
installation, audio-device, Wayland or X11 test.
