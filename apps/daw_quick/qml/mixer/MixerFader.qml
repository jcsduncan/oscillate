pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import "../shell"

// A vertical channel fader over linear gain, scaled in decibels from -60 to
// +12 dB: the kit Fader with the mixer's look, driven by the strip through a
// small controller of its own rather than a device parameter. Drag the cap
// (Shift for fine), click the track to jump, scroll or use the arrow keys
// (0.5 dB, Shift 0.1 dB); double-click or Delete returns to 0 dB. The strip
// owns the value and coalesces each drag as one gesture.
Kit.Fader {
    id: fader
    property real level: 1
    property string accessibleName: "Level"
    property bool enabled_: true
    // The level shown while dragging (-1: not dragging).
    property real preview: -1
    readonly property real shown: preview >= 0 ? preview : level
    // The scale's ends and the arrow-key steps, in decibels.
    readonly property real floorDb: -60
    readonly property real ceilingDb: 12
    readonly property real stepDb: 0.5
    readonly property real fineStepDb: 0.1

    signal begun()
    signal moved(real level)
    signal committed(real level)
    signal resetRequested()

    // Decibels from linear gain (20 dB per decade of amplitude) and back.
    function toDb(value) { return 20 * Math.log10(value) }
    function fromDb(db) { return Math.pow(10, db / 20) }
    function clamp(value) { return Math.max(0, Math.min(1, value)) }
    function positionOf(value) { return value <= 0 ? 0 : clamp((toDb(value) - floorDb) / (ceilingDb - floorDb)) }
    function levelAt(position) {
        return position <= 0 ? 0 : fromDb(floorDb + (ceilingDb - floorDb) * clamp(position))
    }
    function decibels(value) {
        return value <= 0 ? "−∞ dB" : (toDb(value) >= 0 ? "+" : "")
            + toDb(value).toFixed(1).replace("-", "−") + " dB"
    }
    function nudge(db) {
        if (!enabled_)
            return
        const current = level <= 0 ? floorDb : toDb(level)
        const next = Math.max(floorDb, Math.min(ceilingDb, current + db))
        begun()
        committed(next <= floorDb ? 0 : fromDb(next))
    }

    parameterId: ""
    longName: accessibleName
    detent: positionOf(1)
    tipText: accessibleName + ": drag, or wheel; Shift for fine, double-click resets"
    Accessible.description: decibels(level)

    // The fader's gestures land on the strip's signals.
    controller: QtObject {
        readonly property int revision: 0
        readonly property bool editable: fader.enabled_
        function parameter(id) { return ({}) }
        function normalised(id) { return fader.positionOf(fader.shown) }
        function display(id) { return "" }
        function begin_gesture(id) { fader.begun() }
        function set_normalised(id, position) {
            fader.preview = fader.levelAt(position)
            fader.moved(fader.preview)
        }
        function end_gesture(id) {
            const value = fader.preview
            fader.preview = -1
            fader.committed(value)
        }
        function cancel_gesture(id) { fader.preview = -1 }
        function reset_to_default(id) { fader.resetRequested() }
        function step(id, direction, fine) { fader.nudge(direction * (fine ? fader.fineStepDb : fader.stepDb)) }
    }

    // The mixer's fader look and feel as kit tokens: a thin line with a tall
    // cap and a rule at 0 dB; a press off the cap jumps there.
    theme: Kit.KitTheme {
        prefix: "mixer"
        faderAccent: Theme.accent
        faderWidth: 30
        faderDisabledOpacity: 1
        faderCursor: Qt.ArrowCursor
        faderSideKeys: false
        resetOnBackspace: false
        faderTop: 0
        faderBottom: 0
        faderJump: "offCap"
        faderPressSets: true
        faderFineScale: 0.15
        faderTrackWidth: 2
        faderTrackRadius: 0
        faderTrackFull: false
        faderTrackFill: Theme.mixerFaderTrack
        faderTrackEdge: "transparent"
        faderTrackFocusEdge: "transparent"
        faderLit: "none"
        faderCapWidth: 0
        faderCapHeight: 40
        faderCapRadius: radiusLg
        faderGripInset: 8
        faderGripInk: Theme.textMain
        faderDetentInk: Theme.mixerFaderDetent
        property color faderCapTop: Theme.mixerFaderCapTop
        property color faderCapBottom: Theme.mixerFaderCapBottom
        property color faderCapRim: Theme.mixerFaderCapRim
        function faderCapFill(fader, stop) {
            return stop === 0 ? faderCapTop : faderCapBottom
        }
        function faderCapEdge(fader) {
            return fader.activeFocus ? fader.accent : faderCapRim
        }
    }
}
