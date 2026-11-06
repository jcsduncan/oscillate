pragma Singleton
import QtQuick

// The shell's motion: one set of timings for every transition (ADR-0059).
// Off-screen runs (tests, --screenshot) and the reduced-motion preference
// (QSettings "ui/reducedMotion", toggled from the command palette) are
// instant: animations are disabled, never shortened to a zero-length run,
// so a settled layout is the same with or without them.
QtObject {
    readonly property bool offscreen: Qt.platform.pluginName === "offscreen"
                                      || Qt.platform.pluginName === "minimal"
    readonly property var palette: typeof projectBridge !== "undefined" && projectBridge
                                   ? projectBridge.commandPalette : null
    readonly property bool reduced: palette !== null && palette.reducedMotion
    readonly property bool enabled: !offscreen && !reduced

    // Hover and press feedback.
    readonly property int hover: 90
    // Small changes: a highlight, a chip, a popover's scale.
    readonly property int fast: 120
    // Panels and popovers opening, rows arriving.
    readonly property int normal: 170
    // View changes and larger movements.
    readonly property int slow: 220
    readonly property int easing: Easing.OutCubic
    // Popovers grow from this scale as they fade in.
    readonly property real popScale: 0.96
}
