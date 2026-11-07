# Full C++ coverage baseline

## Current measurement — 16 October 2026

The reset-counter `coverage-full` profile passed 108/108 CTest targets. GCC
15.2 and gcovr 8.6 report 26,345/30,272 measured C++ lines (87.03%). Qt
Quick C++ contributes 4,784/6,521 (73.36%). The separate reset-counter
`coverage` profile reports 21,681/23,876 (90.81%) and passes its narrower
90% core gate. QML is exercised by interaction tests but is outside line
coverage. These figures do not close the full-source 90% gate or any release
qualification gate.

For the full profile, `src/` contributes 19,187/21,214 (90.44%) and
`include/` contributes 2,326/2,488 (93.49%). The biggest measured gaps are
Quick bridge and transport workflows, application playback, export and
persistence failure paths. Follow the relevant user-visible recovery and
audio-correctness requirements when adding tests; do not add cases that only
exercise an unvisited line.

The reused build directory contained a `.gcno` file for the removed
`src/routing/send_gain_blocks.cpp`; it was removed before this report so an
obsolete source did not inflate the denominator. The coverage script now
merges duplicate inline function locations from different GCC translation
units at the earliest source line. That setting changes function attribution,
not the reported line counts or the core line gate.

## Earlier checkpoint — 15 October 2026

The `coverage-full` preset enabled Qt Quick, VST3, codec, convolution and both resamplers. At the integrated `6bc6647` checkpoint, a clean generated-counter run with GCC 15.2, gcovr 8.6 and 97 passing CTest targets produced these executable-line counts:

| Scope | Covered / total | Exact rate | Status |
| --- | ---: | ---: | --- |
| Full measured C++ | 22,319 / 25,358 | 88.02% | Below the 90% target |
| Qt Quick C++ (`apps/daw_quick/`) | 3,435 / 4,605 | 74.59% | Main gap |
| Core measured C++ in the separate `coverage` build | 18,426 / 20,399 | 90.33% | Local core gate passes |

The full report includes Quick source C++, not QML. The Quick smoke and component tests check QML workflows. `src/audio_io/pipewire_output.cpp` and the M2 launcher remain outside these line percentages for the reasons in [test notes](../../tests/README.md), with separate live-device and launcher evidence required. This report is a diagnostic baseline, not a full-product coverage pass.

The largest uncovered Quick areas remain the project bridge and its transport, VST3 and export adapters. The shipped app's deterministic startup fixture now covers 44/54 measured entry-point lines. Focused tests added after this checkpoint cover repeated stem publication, failed master export and a saved routing edit that becomes audible only after restart. Continue with distinct failure/recovery workflows and feature-owner extraction, with QML interaction tests for visible state. Do not raise the percentage by excluding Quick source or adding tests that repeat the implementation.

GCC 15 gcov emits a few negative branch counters even after generated counters are cleared and the suite runs once. The coverage script warns once per affected file and retains valid executable lines; it also retains the prior suspicious-counter warning. This is a tool limitation, not a waiver of the 90% full-source gate.

The full 90% gate closes only after the full report reaches at least 90% with the same source scope, the core gate remains green, and risk-based project, persistence, timing and automation evidence is reviewed separately. Relevant commands are `cmake --preset coverage-full` with the pinned SDK and PFFFT paths, `cmake --build --preset coverage-full`, `ctest --preset coverage-full`, and `./scripts/check-coverage.sh coverage-full`.
