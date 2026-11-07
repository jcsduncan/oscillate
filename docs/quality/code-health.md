# Code health and cleanup backlog

This is a maintenance inventory, not a release
qualification result. The [implementation plan](../IMPLEMENTATION_PLAN.md)
tracks product and design gates.

## Current structure

The main module boundaries follow the [HLD](../HLD.md): `timeline`, `project`,
`application`, `engine`, `media`, `native_dsp`, `plugins`, `persistence`,
`export` and the Qt Quick app have separate targets. Public C++ contracts live
under `include/oscillate`; source and matching tests live under `src` and
`tests`. The desktop app has feature controllers and QML components, but a few
composition and session files remain too large.

| Area | Current concentration | Next safe seam |
| --- | --- | --- |
| `src/application/project_session.cpp` | Roughly 6,600 lines of commands and private helpers | Move a cohesive command family only after its helper ownership and test contract are explicit. Keep one atomic project revision per edit. |
| `src/project/model.cpp` | Roughly 3,500 lines of model validation and command application | Separate validation families from command application without changing public model semantics. |
| `src/application/routed_playback_control.cpp` | Roughly 2,600 lines of render-control composition | Split graph preparation from device/revision orchestration after live-edit and callback probes are in place. |
| `src/persistence/store.cpp` | Roughly 2,400 lines of durable store and recovery code | Isolate generation enumeration and integrity verification behind existing store tests. |
| `apps/daw_quick/src/project_bridge.cpp` | Roughly 2,000 lines of QML-facing composition | Route new features through owned controllers; extract existing groups when command/read-model boundaries are stable. |

These are maintenance priorities, not permission for a mechanical rewrite.
The recent live-control and audio changes need behaviour-preserving tests around
any extraction. Effect-panel curve generation has been separated from synth
visuals as one low-risk ownership boundary.

## Validation debt

- The pinned formatter is clang-format 23.1.3. Keep formatting changes in
  separate commits from behaviour changes.
- The live-device tier, a full sanitizer sweep and the full configured
  clang-tidy sweep have not been run on the current tree.
- Physical-device 32-track and underrun tests, recorded-material listening,
  representative VST3 compatibility, usability and distribution licensing
  remain release gates. Passing unit tests does not close them.
- The project is licensed GPL-3.0-or-later. Binary redistribution still needs
  the third-party notice and package review in [dependencies](../dependencies/).

Run `scripts/check-doc-links.py` for local documentation links. It checks file
targets, not heading anchors or remote destinations.
