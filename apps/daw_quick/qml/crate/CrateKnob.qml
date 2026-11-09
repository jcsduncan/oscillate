pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// A Crate knob: the kit Knob with Crate's face (CrateTheme.paintKnob: a 270°
// track with the value arc in the knob's colour, from the centre on bipolar
// ranges, around a dark face), the value under it and the caption below.
// Drag to change it (Shift fine), scroll or use the arrow keys; double-click,
// Ctrl+click or Delete resets. Click the value to type one (Enter sets it,
// Esc cancels). Right-click maps it to a track macro. `parameterId` may
// change under it (the selected chop's controls follow the selection) or be
// empty (no chop): it then shows an idle track and ignores input.
Kit.Knob {
    id: knob
    property color colour: theme.accent
    // "" (the full-size dial) or "sm".
    property string size: ""
    // A panel-side formatter for the plain value (pan as L/C/R, say).
    property var formatter: null
    readonly property bool small: size === "sm"
    readonly property real dial: small ? theme.knobDialSmall : theme.knobDial
    readonly property string fullName: (info.group !== undefined && info.group !== "" ? info.group + " · " : "")
        + (info.name !== undefined ? info.name : label)

    label: info.name !== undefined ? info.name.toUpperCase() : ""
    bound: parameterId !== "" && info.id !== undefined
    diameter: dial
    accent: colour
    longName: fullName
    valueText: !bound || controller.revision < 0 ? "—"
        : formatter !== null ? formatter(controller.value(parameterId)) : controller.display(parameterId)
    valueItem: valueReadout
    menuOwned: true
    tipDetail: "Click the value to type one · double-click or Ctrl+click resets · right-click maps to a macro"
    objectName: theme.prefix + "Knob:" + (bound ? parameterId : label.toLowerCase())
    implicitWidth: Math.max(dial + theme.knobWidthPad, caption.implicitWidth + theme.knobCaptionPad)
    implicitHeight: dial + theme.knobValueGap + theme.valueSize + theme.knobLinePad
        + theme.labelSize + theme.knobLinePad
    opacity: bound ? (controller.editable ? 1 : theme.knobDisabledOpacity) : theme.knobInactiveOpacity
    Accessible.description: valueText + ". Drag, scroll or use arrow keys; double-click resets; click the value to type"

    onMenuRequested: openMacroMenu()

    Text {
        id: valueReadout
        objectName: knob.theme.prefix + "KnobValue:" + knob.parameterId
        visible: !knob.editing
        y: knob.face.height + knob.theme.knobValueGap
        width: Math.max(knob.width, implicitWidth)
        x: (knob.width - width) / 2
        horizontalAlignment: Text.AlignHCenter
        text: knob.valueText
        color: knob.modeInactive && knob.editable ? knob.theme.knobInactiveInk
            : knob.touched ? knob.colour : knob.theme.ink
        font.family: knob.theme.mono
        font.pixelSize: knob.small ? knob.theme.knobValueSizeSmall : knob.theme.valueSize
        MouseArea {
            anchors.fill: parent
            // At least the kit's least hit size: it reaches down over the
            // caption (the dial sits just above).
            anchors.leftMargin: -knob.theme.hitOutset(parent.width)
            anchors.rightMargin: -knob.theme.hitOutset(parent.width)
            anchors.bottomMargin: -Math.max(0, knob.theme.minHitSize - parent.height)
            enabled: knob.editable
            cursorShape: Qt.IBeamCursor
            onClicked: knob.beginEdit()
        }
    }
    Text {
        id: caption
        y: valueReadout.y + valueReadout.height + knob.theme.knobCaptionGap
        width: knob.width
        horizontalAlignment: Text.AlignHCenter
        text: knob.label
        color: knob.theme.inkDim
        font.family: knob.theme.sans
        font.pixelSize: knob.theme.labelSize
        font.letterSpacing: knob.theme.labelSpacing
    }
}
