# Local Ubuntu 26.04 development setup

This repository runs its quality gates locally. The former CI workflow was removed because it omitted required build dependencies and was never a passing gate. Release automation can be added after the local dependency and packaging contract is stable.

## Distribution packages

On Ubuntu 26.04 x86-64, install the core packages listed in the [README](../../README.md#build). They provide CMake, Ninja, GCC, `pkgconf`, [PipeWire development files](https://packages.ubuntu.com/resolute/libpipewire-0.3-dev), OpenSSL 3 and [nlohmann/json development files](https://packages.ubuntu.com/resolute/nlohmann-json3-dev). `cmake --preset dev` checks their CMake and pkg-config identities before compilation.

For `quick`, install [qt6-base-dev](https://packages.ubuntu.com/resolute/qt6-base-dev) and [qt6-declarative-dev](https://packages.ubuntu.com/resolute/qt6-declarative-dev). The latter supplies Qt Quick and QML development files; CMake requires Qt 6.10 or newer. For optional encoded audio, source/export resampling, or JACK support, install `libsndfile1-dev`, `libsamplerate0-dev`, or `libjack-jackd2-dev` respectively. `BUILD_TESTING=ON` uses Python 3 for starter-sound fixtures.

For pkg-config packages unpacked outside the system prefix (PipeWire, JACK), set `PKG_CONFIG_PATH` and `PKG_CONFIG_SYSROOT_DIR` in the configure environment, for example in a `CMakeUserPresets.json` `environment` block; after moving that root, delete the build tree's `CMakeCache.txt` so the cached paths are found again.

The VST3 host uses the upstream SDK tag `v3.8.1_build_84` with its submodules. Set `OSCILLATE_VST3_SDK_ROOT` to that checkout when configuring a scanner-enabled build. The [SDK source inventory](vst3-sdk-source.md) records the expected tag, submodule commits and licence review. The SDK is a build input, not a bundled system package.
PFFFT is vendored in `third_party/pffft` and needs no setup; see the [PFFFT inventory](pffft-convolution.md).

## Local quality tools

The repository's formatting baseline is clang-format 23.1.3; the current static-analysis and coverage tools are clang-tidy 22.1.8 and gcovr 8.6. A project-local virtual environment reproduces those tool versions without changing system LLVM packages:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r scripts/qa-requirements.txt
PATH="$PWD/.venv/bin:$PATH" ./scripts/check-format.sh
PATH="$PWD/.venv/bin:$PATH" ./scripts/check-tidy.sh
PATH="$PWD/.venv/bin:$PATH" ./scripts/check-coverage.sh
```

Configure and test the corresponding CMake preset before each script. `CMakeUserPresets.json` is ignored and is the place for machine-specific SDK and package paths. The local checks in [AGENTS.md](../../AGENTS.md) remain the integration gates. The coverage script currently measures core production C++ and explicitly listed player sources; [test notes](../../tests/README.md) describe its exclusions. A separate full-configuration coverage gate remains open.
