# G7 mixed-production virtual-device repeat

This opt-in runner repeats the [existing saved-project mixed workload](g7-mixed-production-rehearsal.md) three times at each target buffer size on a newly created disposable PipeWire null sink. It uses the checked `oscillate_streaming_32track_rehearsal` fixture rather than a second graph implementation. Each run creates and reopens 32 managed audio tracks, four MIDI clips on one synthetic VST3 instrument track, one synthetic VST3 effect, two groups, two returns, 33 sends, saved send automation and the native master EQ/compressor/reverb/limiter chain. The fixture independently probes the effect, instrument and automation before timing the common live graph. The wrapper checks an audible monitor and sink cleanup and writes structured evidence only after all runs pass.

The workload covers the first currently supported mixed graph. It has only one trivial effect and one trivial instrument plugin, no third-party plugin mix, no physical audio output and no controlled UI/background load. PipeWire exposes no attributable stream xrun counter here, so a zero callback-over-period count must not be reported as zero xruns. This is a development rehearsal; G7 release qualification remains open.

## Reproduce

Use the pinned VST3 SDK and the installed PipeWire/JSON development packages. `$DEPS` below stands for a local dependency prefix; drop the sysroot and prefix variables when the packages are installed system-wide. Keep all compiler scratch and temporary project media under the worktree build directory.

```sh
mkdir -p build/g7-mixed/scratch
export TMPDIR="$PWD/build/g7-mixed/scratch"
export PKG_CONFIG_SYSROOT_DIR=$DEPS/unpacked
export PKG_CONFIG_PATH=$DEPS/unpacked/usr/lib/x86_64-linux-gnu/pkgconfig
cmake -S tests/qualification -B build/g7-mixed -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DOSCILLATE_WARNINGS_AS_ERRORS=ON \
  -DOSCILLATE_BUILD_VST3_SCANNER=ON \
  -DOSCILLATE_VST3_SDK_ROOT=$DEPS/vst3sdk-3.8.1 \
  '-DCMAKE_PREFIX_PATH=$DEPS/qt/usr;$DEPS/nlohmann/usr'
cmake --build build/g7-mixed --target oscillate_streaming_32track_rehearsal -j8
python3 tests/qualification/qualify_g7_mixed_virtual.py \
  --output build/g7-mixed/mixed-virtual.json
```

The JSON records 48 kHz 64/128-frame per-run callback timing, source misses, render/plugin failures, nullable backend xruns, monitor energy and cleanup. It deliberately does not produce a release verdict. The wrapper itself has a small parser contract test:

```sh
python3 -m unittest discover -s tests/qualification -p test_g7_mixed_virtual.py -v
```

## Local result

18 October 2026, Ubuntu 26.04.1 LTS, Linux 7.0.0-38-generic, Intel Core i7-10700K, GCC 15.2.0 Release. CPU frequency, thermal state, page cache and background load were uncontrolled. The exact command above passed three repetitions at each size, with 2,000 requested measured blocks and 100 warmup blocks per run. Each fresh sink monitor had peak 0.8913 and 140–141 (64-frame) or 280 (128-frame) active 20 ms windows; all six sinks were removed. Saved-project, effect, instrument and automation oracles ran on every repetition. The JSON evidence was written to `build/g7-mixed/mixed-virtual.json` and is local build output.

| Frames | Longest callback per repetition, µs | Measured callbacks across repeats | Over nominal processing period | Source misses / render faults / plugin faults | Backend xruns |
| ---: | --- | ---: | ---: | --- | --- |
| 64 | 175.354, 146.089, 146.273 | 6,409 | 0 | 0 / 0 / 0 | Unknown |
| 128 | 956.305, 256.054, 264.774 | 6,399 | 0 | 0 / 0 / 0 | Unknown |

The first attempt completed three 64-frame runs and one 128-frame run, then failed before playback because its fifth sink's links did not appear within the former eight-second startup bound. The harness did not publish success evidence. The readiness poll waits up to 20 seconds for *actual* links or a player exit, and the entire six-run command passed. The startup observation is separate from callback or backend-underrun evidence; a future run should retain and classify any recurrence rather than silently retry it.
