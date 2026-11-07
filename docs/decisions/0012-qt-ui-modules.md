# ADR-0012 — Qt modules for the desktop UI

**Status:** Superseded by [ADR-0022](0022-qt-quick-ui-direction.md) for the presentation layer. Its Core/Gui licensing boundary still applies; Qt Widgets is used only for file dialogs and the VST3 editor host (`src/app/plugin_ui`).
**Date:** 14 October 2026  
**Requirements:** PRD §§5–6, 20, 23; FS-07; G8 dependency gate.

## Context

The HLD prefers Qt for a resizable, keyboard-accessible desktop workspace, but the exact modules and redistribution route must be chosen before UI code depends on it. Ubuntu 26.04 currently offers Qt 6.10.2 packages on the development machine. The engine and headless player already build without Qt.

## Decision

Use Qt 6 **Core, Gui and Widgets** for the desktop application, dynamically linked under the modules' LGPLv3 option. UI code uses `Qt6::Core`, `Qt6::Gui` and `Qt6::Widgets` only in the `ui`/`app` targets. Engine, timeline, project, media, persistence, export and DSP public APIs remain independent of Qt. Do not add Qt Multimedia, WebEngine, Quick, Network or GPL-only modules by implication; any additional module requires a component-level licence and dependency review. Use target-based `find_package(Qt6 6.10 COMPONENTS Core Gui Widgets REQUIRED)` in the graphical target, leaving headless targets available without Qt development files.

Before distributing the Ubuntu package or AppImage, inventory the exact Qt libraries, platform plugins and their transitive components actually shipped. Provide the applicable licence notices and Qt source or a project-controlled written offer, preserve users' ability to substitute/relink the LGPL libraries, and test that ability in each package format. The project's own source licence is a separate decision. This ADR authorizes implementation against the named modules; it does not close the release licensing gate.

Qt documents [Core](https://doc.qt.io/qt-6.10/qtcore-index.html), [Gui](https://doc.qt.io/qt-6.10/qtgui-index.html) and [Widgets](https://doc.qt.io/qt-6.10/qtwidgets-index.html) as available under LGPLv3. Its [open-source licensing FAQ](https://www.qt.io/faq/qt-open-source-licensing) describes source-availability and relinking obligations, and its [Qt 6.10 module list](https://doc.qt.io/qt-6.10/licensing.html) identifies modules with different open-source terms.

## Alternatives considered

- A custom widget toolkit would delay the first usable arrangement workspace and duplicate established accessibility and windowing infrastructure.
- Static Qt linking complicates the planned LGPL distribution route and is unnecessary for the Ubuntu package or AppImage.
- Adding a broader Qt module set now would increase licensing and packaging work without a defined v1 UI need.

## Consequences and evidence

The C4 UI can begin once its read-model and MIDI contracts are frozen. Unit and interaction tests remain local; a Wayland and X11 application smoke checks layouts, focus, keyboard access and high DPI. Packaging acceptance later checks exact shipped module versions, notices, source availability, replacement/relinking and the component-level SBOM. Plugin-editor embedding has its separate G8 Wayland/X11 spike and generic-editor fallback.
