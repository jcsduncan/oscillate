# Contributor guidance

These rules apply to the whole repository, for human contributors and coding
assistants alike. Read the [PRD](docs/PRD.md), [HLD](docs/HLD.md),
and [spec index](docs/specs/README.md) before changing a component.
Open work is tracked in [issues](https://github.com/jcsduncan/oscillate/issues). [CONTRIBUTING.md](CONTRIBUTING.md) covers bug reports and proposals.

## Source of truth and scope

1. The PRD defines product scope and acceptance. The HLD defines module boundaries. Functional specs define behaviour and tests. ADRs record cross-component technical decisions. If they disagree, stop the affected implementation, describe the conflict and update the documents through review. A change to a module boundary, persisted format, real-time contract, public API, telemetry layout or documented behaviour updates the HLD and specs in the same pull request; if it contradicts an accepted ADR, it adds a new ADR and marks the old one "Superseded by ADR-NNNN" instead of rewriting it. CodeRabbit's "Architecture docs current" check enforces this.
2. C++20 is the project standard. Do not use C++23 features, compiler extensions, JUCE or an unapproved runtime dependency. Keep Qt types inside `apps/daw_quick` and `src/app/plugin_ui`.
3. Keep public APIs small and versioned where persisted or used across components. Code belongs in `include/oscillate/<module>/`, `src/<module>/` and matching `tests/<module>/`. Give each component its own CMake target, declared with `oscillate_add_library` (its dependencies as `PUBLIC`/`PRIVATE`), which applies `oscillate_target_defaults`; features and third-party lookups live in `cmake/Features.cmake`.
4. Do not implement a deferred PRD feature under a different name. Do not present a scaffold, mock or synthetic plugin as a completed DAW workflow.
5. Keep production app entry points to startup and composition. Put UI scenario drivers in `tests/`, with a separate test executable. Add UI workflows to feature-owned adapters and QML components; do not extend the central Quick facade or shell with another large feature block.
6. The project is greenfield until its first public beta: the codec reads only the current schema. Do not add migrations or legacy paths for development schemas.

## Real-time and data-integrity rules

1. The audio callback must not allocate, block, wait on a mutex, perform I/O, format logs, call UI code, discover plugins or access mutable project state. Graph and buffer construction and reclamation happen off the callback.
2. Timing and automation semantics come from the shared `timeline` module. Playback and export use the same evaluator. Do not duplicate tempo maths in UI, plugins, media or export.
3. Project commands validate before mutation, publish one atomic revision and preserve undo/redo semantics. Asynchronous results install only when their generation and inputs still match.
4. Source media, project state and user renders are never cache entries. Save-format changes need interrupted-write and reopen tests. Missing plugin and media placeholders retain IDs, references and user state.
5. Treat project files, media, manifests and plugin state as untrusted input. Bound sizes, validate paths and report failures without losing edits.

## DSP performance policy

1. Make each processor correct and bounded first, with a simple reference path and independent audio oracles. Design callback paths for preallocated buffers, predictable work and no blocking from the first version.
2. Measure CPU time and allocations for changed processing paths at 48 kHz with 64- and 128-frame blocks in an uninstrumented build. Record hardware, compiler, processor settings and workload so results can be compared.
3. Optimise measured hot paths with buffer/layout changes, compiler vectorisation or justified SIMD/library choices. Keep the reference path or an equivalent output oracle, and test numerical tolerance, latency, tails and real-time/offline timing after each algorithmic change. Do not trade audible quality or deterministic semantics for an unmeasured speedup.
4. Report before/after CPU and allocation evidence for a change that claims a performance improvement. A single-thread kernel benchmark is not enough for a change to the playback path: also measure whole-process CPU on a light and a heavy project, including tail latencies (p99). Use sanitizer and coverage builds for correctness, not CPU comparisons.
5. Evaluate mature DSP libraries before writing a general-purpose algorithm. On x86-64, keep a tested baseline path and use runtime-dispatched SIMD only where it improves the measured workload. GPU acceleration is only for offline or background jobs, with a CPU path, and the audio callback never waits for a GPU result.

## Tests and coverage

1. Add the smallest meaningful unit, contract, integration or fixture test for each behaviour and failure path. Use hand-calculated or independent oracles, fake clocks/devices/workers for deterministic tests, and real filesystem tests for persistence. Avoid sleeps as unit-test oracles and tests that simply restate the implementation.
2. Name shared constants once (`constexpr`), in tests too: tolerances, notes, sample rates and block sizes get names rather than repeated literals.
3. Track executable-line coverage. 90% aggregate and 95% for high-risk modules are goals. Review uncovered high-risk branches and use callback, sanitizer, fault-injection, fuzz and reference-hardware tests where needed. Do not inflate coverage through broad excludes, trivial tests or happy-path-only tests; document justified exclusions and their alternative evidence.
4. During feature work, build changed targets and run only the tests for affected behaviour. Run the broad suite, clang-tidy, clang-format and coverage before merging a large change. Add a focused sanitizer or concurrency check when a changed high-risk path needs it. Performance qualification runs without sanitizer instrumentation.
5. Declare a test program with `oscillate_add_test_executable(<target> SOURCES … LIBS … [TEST <name> TIER …])` and register any further test with `oscillate_add_test(NAME … COMMAND … [TIER fast|slow|live])` (`cmake/ProjectOptions.cmake`); configuration fails on a raw `add_test`. Each test gets its tier, an area label (`tests/<area>`, `ui` for apps) and a per-tier timeout. `fast` is the default and must stay under about 10 s in a loaded Debug parallel run; mark longer DSP oracles, parity and long renders `slow`, and anything needing real audio devices, a JACK server or a display `live`. Split a long contract into cases with `tests/support/include/oscillate/test/cases.hpp` and `oscillate_add_case_tests` rather than raising a timeout; never loosen a tolerance or drop a case for speed. Audio kernels and the native DSP tests compile optimised in Debug (`oscillate_optimize_in_debug`, off under coverage).
6. Every test has a time budget: three times its measured `-j4` runtime, never below its tier's default (`OSCILLATE_TEST_DEFAULT_BUDGET_*` in `cmake/ProjectOptions.cmake`). `cmake/TestBudgets.cmake` lists every slow and live test, and configuration fails for one without an entry; a fast test needs an entry only when it measures over the fast default. After adding a test or changing its workload, run it with `--output-junit` and regenerate the table with `scripts/update-test-budgets.py --test-dir build/quick build/quick/junit.xml`. Sanitizer and coverage presets scale the budgets.

## Commands

Install dependencies and build the application as described in the README's
[Build](README.md#build) section; optional dependencies are in
[local dependency setup](docs/dependencies/local-development.md). Use
`CMakeUserPresets.json` (ignored by Git) for machine-specific paths.

Keep compiler scratch files and test fixtures inside the build tree: create
its `scratch/` directory and set `TMPDIR` to that absolute path. Do not put
build directories or scratch under `/tmp`.

`scripts/build.sh` runs the build in a memory-capped systemd scope at low
priority, with six compiler jobs by default ([environment](docs/environment.md#build-and-tooling)),
so a large build is throttled rather than taking down the desktop session.
Keep tests at `-j4` or below for the same reason, and run one build or test
sweep at a time. Pass Ninja options after `--`
(`scripts/build.sh build/quick -- -k 0`).

```sh
# A focused test binary, then only its contract.
scripts/build.sh build/quick --target oscillate_project_tests
TMPDIR="$PWD/build/quick/scratch" ctest --test-dir build/quick -R '^project_contract$' --output-on-failure
# All test binaries (the Quick preset leaves them out of its default build).
scripts/build.sh build/quick --target oscillate_tests
# Tiers, areas, or everything.
TMPDIR="$PWD/build/quick/scratch" ctest --preset quick-fast -j4
TMPDIR="$PWD/build/quick/scratch" ctest --preset quick-slow -j4
TMPDIR="$PWD/build/quick/scratch" ctest --test-dir build/quick -L native_dsp -j4
TMPDIR="$PWD/build/quick/scratch" ctest --test-dir build/quick --output-on-failure -j4
```

`ctest -LE live` skips tests that need devices or a display. `dev-fast`,
`sanitize-fast` and `tsan-fast` run the fast tier under those presets.

Development presets build the project libraries as shared objects with split
DWARF; Release links statically. ccache and mold are used when installed.

The shared source-analysis cache tests inspect every ancestor of their fixture
path. On a checkout created with a group-writable umask, run
`chmod g-w . && chmod 700 build` before those tests. Do not relax the cache's
path validation to accommodate a fixture.

Broad checks before merging a large change:

```sh
cmake --preset clang && cmake --build --preset clang && ctest --preset clang
./scripts/check-format.sh
./scripts/check-tidy.sh
cmake --preset coverage && cmake --build --preset coverage && ctest --preset coverage
./scripts/check-coverage.sh
cmake --preset sanitize && cmake --build --preset sanitize && ctest --preset sanitize
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
python3 -I scripts/check-doc-links.py
```

The coverage script requires `gcovr`; formatting and tidy require the LLVM
tools pinned in `scripts/qa-requirements.txt`. The tidy script checks every
configured feature variant in a separate compile-database batch, so do not
replace it with one direct `clang-tidy` call over duplicate Quick translation
units. Record any unavailable tool in the change description.

## Working on shared contracts

1. Keep each change to one coherent behaviour and its tests. Split work by component and contract, not by producing competing versions of the same module.
2. Agree and merge shared contracts (public headers, project format, timing semantics, top-level CMake, these rules) before work that depends on them. Propose a contract change in a small, separate change with notes on downstream impact.
3. A change description lists the requirement IDs, changed API or schema, affected tests, and real-time and licensing effects. Cross-boundary changes need review from someone familiar with the other component.
4. Rebase on current `main`, build the changed targets and run the affected tests before merging. Do not silently resolve conflicting edits, waive a failed check or bypass review.
5. A schema change takes the next free minor version on `main`; check open branches before claiming one.

## Git and changes

Make focused commits with clear messages. Do not rewrite published history. Do not add secrets, local media, built plugin binaries, caches or generated build output. New dependencies require the HLD's licence, quality and packaging review before merging; their terms must be compatible with GPL-3.0-or-later.
