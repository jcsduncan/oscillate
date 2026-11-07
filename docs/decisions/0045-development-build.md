# ADR-0045 — The development build

**Status:** Accepted and implemented. **Date:** 20 October 2026.

## Context

On a 16-thread, 30 GB workstation the development build had become the slowest part of the work, and twice it took the desktop down:
- **Linking flooded the disk.** Every executable embedded static copies of the project libraries. Each of about 55 Quick test programs was a ~240 MB Debug binary, and one library change relinked them all. At `-j8` the NVMe saturated: I/O pressure was about 80%, the audio session manager stalled in uninterruptible sleep, and `top` looked idle.
- **Memory pressure killed the session.** A `-j14` build drove the machine into memory pressure, and `systemd-oomd` killed GNOME Shell.
- **Timeouts hid hangs.** Tests had hand-set timeouts of 10–600 s and a 300 s default, so a hang took minutes to report.

## Decision

1. **Shared libraries for development.** The `dev`, `quick`, sanitizer and coverage presets build the project's libraries as shared objects (`OSCILLATE_SHARED_LIBS`); `release` links them statically into one application. Shared libraries link with `-Wl,--no-undefined`, so a missing dependency fails the build. Executables are not relinked when a library they load changes (`CMAKE_LINK_DEPENDS_NO_SHARED`). The one exception is the convolution library, which stays static because its allocation-failure tests wrap `malloc` at link time.
2. **Cheaper links and debug info.** Debug builds use split DWARF. Links share a two-job Ninja pool. `mold` and `ccache` are used when installed.
3. **One copy of the shell's QML and one bridge.** The shell's QML resource is compiled into the bridge library once, not into every program. The app and its tests share one build of that library: its test seams are always compiled and inert. A second, minimal build without the optional features backs the feature-disabled tests.
4. **Fewer, smaller test programs.** The 22 device-panel tests run from one program whose generated dispatcher calls each test file's renamed `main`. Unused test programs and settled prototypes are deleted.
5. **Measured time budgets.** Every test has a budget in `cmake/TestBudgets.cmake`: three times its measured runtime under `ctest -j4`, and at least five seconds. Configuration fails for a test without one. `scripts/update-test-budgets.py` regenerates the table from JUnit reports, and sanitizer and coverage presets scale it. (Amended 2026-11-06: each tier has a default budget in `cmake/ProjectOptions.cmake`; the table lists every slow and live test and only the fast tests that need more than the fast default.)
6. **Builds cannot take the desktop.** `scripts/build.sh` runs a build in its own systemd user scope with a memory ceiling, at low CPU and I/O priority and six jobs. Memory pressure throttles or stops the build instead of the session's OOM killer.

## Measured

Debug `quick` tree, `-j6` and `-j8`:

| Change | Rebuild |
|---|---|
| a synth `.cpp` | about 5 s (one shared library) |
| a QML file | about 4 s |
| `model.hpp` (half the objects depend on it) | 7–8 min |

- **Test programs:** each Quick test program shrank from about 240 MB to about 2.6 MB.
- **Where full-build time goes:** compiling is about 43 CPU-minutes. Tests account for 26 of them.
- **Why a header change costs so much:** most of each file's time is code generation for inline and template code from the model and session headers. Parsing is about 11% and template instantiation 20%, so precompiled headers would save little.

## Consequences

- While iterating, build only the app target. Build and run the tests at convergence points, before a commit.
- A change to `model.hpp` or `project_session.hpp` still rebuilds about half the tree. The next lever is splitting `model.hpp`, so that low-level headers include only the types they use.
- A new test needs a budget line before it configures.
