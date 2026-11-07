# VST3 verified bundle loader: first Linux runtime slice

**Status:** Synthetic bundle exercised locally; third-party compatibility unverified.  
**Requirements:** FS-04 PI-01, PI-03, PI-04, PI-08; ADR-0015.

The scanner now returns a control-side binary receipt after a successful
out-of-process scan. It records the canonical `.vst3` bundle and
`Contents/x86_64-linux/<bundle-stem>.so` paths, byte size and SHA-256 digest.
The binary must be a nonempty regular file inside its bundle and at most
512 MiB. The scan service fingerprints before and after the helper runs and
refuses a changed binary. The receipt is **not** added to the helper wire
protocol: the supervisor computes it from the local file so the helper cannot
claim a different path or digest.

`load_verified_vst3_effect` is a control-thread operation. It rechecks the
receipt, loads the canonical binary with `RTLD_NOW | RTLD_LOCAL`, checks the
required Linux VST3 exports, and calls `ModuleEntry` and `GetPluginFactory`.
Before creating a processor, it enumerates at most 256 class records and
matches the scanned class ID, category, name, effective vendor, version and
subcategories. It creates the existing narrow stereo float32 host through its
private factory seam. The loaded module remains resident until the host has
terminated and released its component, then `ModuleExit` and `dlclose` run.
Failure paths close the module; an invalid receipt, missing file or changed
binary never reaches `ModuleEntry`.

The receipt is an accidental-change check, not a security capability. A
hostile process can still replace a file between hashing and `dlopen`, or
modify a loaded binary. In-process plugin code can crash, block or allocate;
neither the scanner's process isolation nor this hash check contains such a
fault. Do not call scanning, fingerprinting, loading, state transfers or
module teardown from the audio callback. The first host handles only the
subset documented in [the effect-host note](vst3-effect-host.md); this loader
adds no plugin editor, controller, parameter automation, MIDI or general
third-party compatibility claim.

The loader compiles **no VST3 SDK `.cpp` source files**. It directly includes
`pluginterfaces/base/ipluginbase.h` and uses the same MIT-licensed transitive
`pluginterfaces` headers as the scanner and effect host. The root and
`pluginterfaces/LICENSE.txt` MIT notices remain required in release packages.
The new control-side SHA-256 fingerprint uses OpenSSL `libcrypto` 3, already a
project dependency. The release SBOM must include OpenSSL and its Apache 2.0
licence and confirm its linked Ubuntu/AppImage runtime version.

Integration requires `add_subdirectory(bundle_loader)` after `effect_host` in
`src/plugins/CMakeLists.txt`, plus `add_subdirectory(bundle_loader)` in
`tests/plugins/CMakeLists.txt`. The added target is
`oscillate::vst3_bundle_loader`; the tests link it to a synthetic working
`LoadedGain.vst3` fixture. That fixture exercises stereo gain, component
state, latency, class mismatch, binary replacement/removal, and ordering of
component termination/destruction before module exit. These tests are not a
substitute for a representative plugin compatibility matrix.
