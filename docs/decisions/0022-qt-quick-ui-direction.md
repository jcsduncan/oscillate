# ADR-0022 — Custom Qt Quick UI for the DAW

**Status:** Accepted for the production UI direction; module and packaging qualification open
**Date:** 14 October 2026
**Requirements:** PRD §§5–6, 20; FS-07; G8 dependency gate.
**Supersedes:** ADR-0012's choice of Widgets as the production presentation layer. Its Core/Gui licensing boundary and headless build requirement remain applicable.

## Context

The first Qt Widgets window proved the project, MIDI, starter-sound, import, export and playback command paths. Its default appearance is unsuitable for the intended polished DAW. A custom Qt Quick/QML interface was chosen over an embedded browser frontend, which was evaluated as the alternative. The audio engine, project model and application services are independent of Qt presentation code, so replacing the window does not change their real-time contracts or project format.

## Decision

Build the production desktop shell with Qt Quick/QML and a DAW-specific dark design system. Use Qt Core/Gui/Quick/Qml and only the Qt Quick Controls modules actually needed. Timeline, piano roll, automation and meters may use custom scene-graph items where ordinary controls are insufficient. C++ application services expose typed commands and revisioned read models to a thin UI bridge; QML never accesses the engine callback or mutable project state directly. The UI bridge remains on the UI thread. Keep the current Widgets window as a temporary, tested prototype until the replacement covers its workflows, then remove it.

Do not add Qt WebEngine, CEF, Tauri or a JavaScript runtime to the production dependency set for this decision. Browser UI remains a documented alternative if a measured spike later shows a material advantage. Plugin editor attachment stays behind the existing `plugin_ui` native-window contract and still requires the Wayland/X11 compatibility spike.

## Migration and acceptance

1. Establish a small QML shell with the design tokens, transport status and one track/clip view. Keep the C++20 application boundary and headless targets intact.
2. Port import, MIDI editing, starter-sound binding, playback status, save/reopen and export through typed bridge methods. Preserve stale-revision and save-failure explanations.
3. Validate 1080p and 4K/high-DPI layout, keyboard focus, accessible names, non-colour state cues and interaction latency with representative tracks and lanes.
4. Remove the Widgets window only after its behavioural tests have equivalents against the new shell. No release claim follows from a styled shell alone.

Before distributing a package, inventory the exact Quick/Qml/Controls libraries, QML imports, platform plugins and transitive dependencies. Review their available open-source terms and LGPL relinking obligations for both the Ubuntu package and AppImage. Benchmark the arrangement canvas and plugin-editor window path on the target Ubuntu Wayland and X11 sessions.

## Alternatives considered

- **Styled Widgets:** smallest code change, but the intended custom, animated workspace would require extensive bespoke painting and control styling on the current prototype.
- **Embedded web UI:** HTML/CSS/TypeScript can deliver a strong visual system and browser tooling. CEF or Qt WebEngine adds a renderer process, packaged runtime assets and an explicit asynchronous JS/C++ bridge; native plugin editors and high-density arrangement rendering still need a host-window and performance spike. This cost is not justified for the current production direction.
- **Tauri:** supplies a webview shell but introduces a Rust host and an additional C++/Rust boundary for this C++-first product.

Qt documents [custom Quick Controls](https://doc.qt.io/qt-6/qtquickcontrols-customize.html), the [Quick scene graph](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html) and [module licensing](https://doc.qt.io/qt-6/licensing.html). The alternative browser deployment costs are documented by [Qt WebEngine](https://doc.qt.io/qt-6/qtwebengine-deploying.html) and [CEF](https://github.com/chromiumembedded/cef/blob/master/docs/general_usage.md).
