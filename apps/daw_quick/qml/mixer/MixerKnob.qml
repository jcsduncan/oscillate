pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import "../shell"

// A mixer knob over a 0..1 value: the kit Knob with the mixer's face (a
// round-capped arc around a dark face with an index dot) and its captions,
// driven by the strip through a small controller of its own rather than a
// device parameter. Drag vertically (Shift for fine), scroll, arrow keys
// (Shift for fine), double-click or Delete to reset. The strip owns the
// value: the knob previews while dragging and reports begin / move /
// commit so the owner can coalesce one gesture.
Kit.Knob {
    id: knob
    // The strip's value as a knob position; `origin` is where its arc starts
    // (0 for a send, 0.5 for pan).
    property real value: 0
    property string accessibleName: label
    property bool enabled_: true
    // The position shown while dragging (-1: not dragging).
    property real preview: -1
    readonly property real shown: preview >= 0 ? preview : value

    signal begun()
    signal moved(real position)
    signal committed(real position)
    signal resetRequested()

    function clamp(position) { return Math.max(0, Math.min(1, position)) }

    parameterId: ""
    longName: accessibleName
    tipText: accessibleName + ": drag up or down, or wheel; Shift for fine, double-click resets"
    tipDetail: ""
    implicitWidth: Math.max(diameter, caption.implicitWidth)
    implicitHeight: diameter + caption.implicitHeight + valueReadout.implicitHeight + theme.knobHeightPad
    Accessible.description: valueText

    // The knob's gestures land on the strip's signals.
    controller: QtObject {
        readonly property int revision: 0
        readonly property bool editable: knob.enabled_
        function parameter(id) { return ({}) }
        function normalised(id) { return knob.shown }
        function display(id) { return "" }
        function begin_gesture(id) {
            knob.preview = knob.value
            knob.begun()
        }
        function set_normalised(id, position) {
            knob.preview = position
            knob.moved(position)
        }
        function end_gesture(id) {
            const position = knob.preview
            knob.preview = -1
            knob.committed(position)
        }
        function cancel_gesture(id) { knob.preview = -1 }
        function reset_to_default(id) { knob.resetRequested() }
        function step(id, direction, fine) {
            if (!knob.enabled_)
                return
            knob.begun()
            knob.committed(knob.clamp(knob.value + direction * (fine ? knob.theme.knobFineStep : knob.theme.knobStep)))
        }
    }

    // The mixer's knob look and feel as kit tokens.
    theme: Kit.KitTheme {
        prefix: "mixer"
        knobDiameter: 34
        knobDialPad: 0
        knobCaptions: false
        knobEases: false
        knobMacros: false
        knobRightClick: false
        knobCursor: Qt.ArrowCursor
        knobDisabledOpacity: 1
        knobDragPixels: 150
        knobFinePixels: 750
        resetOnBackspace: false
        knobAccent: Theme.accent
        // Captions: the label, then the value right under it.
        knobCaptionGap: 1
        knobLabelSize: typeCaption
        knobLabelSpacing: 0.6
        knobLabelBold: true
        knobLabelInk: Theme.textDim
        knobValueSize: typeSmall
        knobValueFont: Theme.monoFamily
        knobHeightPad: 2
        ink: Theme.textMain
        // A step (arrow key, wheel notch) and a fine one, as knob positions.
        property real knobStep: 0.02
        property real knobFineStep: 0.005
        // The face: a 270° sweep from the lower left in degrees, a round-capped
        // arc inset from the edge, a face inside it and an index dot.
        property real knobStartDegrees: -225
        property real knobSweepDegrees: 270
        track: Theme.sendKnobTrack
        property color knobFace: Theme.sendKnobCap
        property color knobFaceEdge: Theme.mixerKnobFaceEdge
        knobArcInset: 3
        knobArcStroke: 3.5
        knobArcMinimum: 0.002
        knobCapInset: 6
        property real knobDotReach: 10
        property real knobDotRadius: 1.8
        function paintKnob(ctx, knob, size) {
            const c = size / 2
            const r = size / 2 - knobArcInset
            const angle = function(p) { return (knobStartDegrees + knobSweepDegrees * p) * Math.PI / 180 }
            ctx.lineCap = "round"
            ctx.lineWidth = knobArcStroke
            ctx.strokeStyle = track
            ctx.beginPath()
            ctx.arc(c, c, r, angle(0), angle(1), false)
            ctx.stroke()
            const from = Math.min(knob.origin, knob.position)
            const to = Math.max(knob.origin, knob.position)
            if (to - from > knobArcMinimum) {
                ctx.strokeStyle = knob.accent
                ctx.beginPath()
                ctx.arc(c, c, r, angle(from), angle(to), false)
                ctx.stroke()
            }
            ctx.fillStyle = knobFace
            ctx.strokeStyle = knob.activeFocus ? knob.accent : knobFaceEdge
            ctx.lineWidth = hairline
            ctx.beginPath()
            ctx.arc(c, c, r - knobCapInset, 0, 2 * Math.PI, false)
            ctx.fill()
            ctx.stroke()
            const a = angle(knob.position)
            ctx.fillStyle = ink
            ctx.beginPath()
            ctx.arc(c + (r - knobDotReach) * Math.cos(a), c + (r - knobDotReach) * Math.sin(a),
                    knobDotRadius, 0, 2 * Math.PI, false)
            ctx.fill()
        }
    }

    Text {
        id: caption
        anchors.top: knob.face.bottom
        anchors.topMargin: knob.theme.knobCaptionGap
        anchors.horizontalCenter: parent.horizontalCenter
        text: knob.label
        color: knob.theme.knobLabelInk
        font.pixelSize: knob.theme.knobLabelSize
        font.letterSpacing: knob.theme.knobLabelSpacing
        font.bold: knob.theme.knobLabelBold
    }
    Text {
        id: valueReadout
        anchors.top: caption.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        text: knob.valueText
        color: knob.theme.ink
        font.pixelSize: knob.theme.knobValueSize
        font.family: knob.theme.knobValueFont
    }
}
