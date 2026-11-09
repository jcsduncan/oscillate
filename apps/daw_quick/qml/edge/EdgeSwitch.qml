import QtQuick
import "../devicekit" as Kit

// A labelled on/off row for a two-choice parameter: the label on the left,
// a pill switch on the right that fills with the accent when on. Click,
// Space or Return flips it; Delete resets. Each flip is one undo step.
Item {
    id: row
    required property var controller
    required property EdgeTheme theme
    required property string parameterId
    property string label: info.name !== undefined ? info.name : ""
    readonly property var info: controller.parameter(parameterId)
    readonly property bool on: controller.revision >= 0 && controller.choice(parameterId) === 1

    objectName: theme.prefix + "Toggle:" + parameterId
    implicitHeight: theme.toggleRowHeight
    activeFocusOnTab: controller.editable
    opacity: controller.editable ? 1 : theme.switchDisabledOpacity
    Accessible.role: Accessible.CheckBox
    Accessible.name: label
    Accessible.checkable: true
    Accessible.checked: on

    function flip() {
        if (!controller.editable)
            return
        theme.lastTouched = parameterId
        controller.set_choice(parameterId, on ? 0 : 1)
    }
    Keys.onSpacePressed: flip()
    Keys.onReturnPressed: flip()
    Keys.onDeletePressed: {
        controller.reset_to_default(parameterId)
        theme.lastTouched = parameterId
    }

    Text {
        anchors.verticalCenter: parent.verticalCenter
        width: track.x - row.theme.spaceSm
        text: row.label
        color: row.theme.ink
        font.family: row.theme.sans
        font.pixelSize: row.theme.typeLabel
        elide: Text.ElideRight
    }
    Rectangle {
        id: track
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: row.theme.toggleTrackWidth
        height: row.theme.toggleTrackHeight
        radius: height / 2
        color: row.on ? row.theme.accent : row.theme.track
        Rectangle {
            readonly property real inset: row.theme.toggleKnobInset
            x: row.on ? track.width - width - inset : inset
            y: inset
            width: track.height - 2 * inset
            height: width
            radius: width / 2
            color: row.theme.dotFill
        }
    }
    // The focus ring, clear of the track so it shows on and off.
    Rectangle {
        objectName: row.theme.prefix + "ToggleFocus:" + row.parameterId
        visible: row.activeFocus
        anchors.fill: track
        anchors.margins: -row.theme.toggleFocusGap
        radius: height / 2
        color: "transparent"
        border.color: row.theme.accent
        border.width: row.theme.strokeBold
    }
    Kit.Tip {
        theme: row.theme
        shown: area.containsMouse && !area.pressed
        text: row.label + "  " + (row.on ? "On" : "Off")
        detail: "Click, Space or Return flips it · Delete resets"
    }
    MouseArea {
        id: area
        anchors.fill: parent
        enabled: row.controller.editable
        hoverEnabled: true
        onClicked: {
            row.forceActiveFocus()
            row.flip()
        }
    }
}
