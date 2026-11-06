pragma ComponentBehavior: Bound
import QtQuick

// A performance wheel: a ribbed vertical strip. A spring wheel (pitch bend)
// returns to centre on release.
Item {
    id: wheel
    required property var controller
    required property EmberTheme theme
    required property string parameterId
    property string label: info.name !== undefined ? info.name : ""
    property bool spring: false
    property color accent: theme.amber
    readonly property var info: controller.parameter(parameterId)
    readonly property real position: controller.revision >= 0 ? controller.normalised(parameterId) : 0
    property real dragOrigin: 0
    property real dragStartY: 0
    // How far the ribs roll over the wheel's full travel, in rib spacings.
    readonly property real ribRoll: 0.6
    readonly property real springCentre: 0.5

    objectName: "emberWheel:" + parameterId
    implicitWidth: theme.wheelWidth
    implicitHeight: theme.wheelImplicitHeight
    activeFocusOnTab: controller.editable
    opacity: controller.editable ? 1 : theme.knobDisabledOpacity
    Accessible.role: Accessible.Slider
    Accessible.name: label
    Accessible.description: controller.revision >= 0 ? controller.display(parameterId) : ""
    Keys.onPressed: event => {
        if (!controller.editable)
            return
        if (event.key === Qt.Key_Up) {
            controller.step(parameterId, 1, (event.modifiers & Qt.ShiftModifier) !== 0)
            event.accepted = true
        } else if (event.key === Qt.Key_Down) {
            controller.step(parameterId, -1, (event.modifiers & Qt.ShiftModifier) !== 0)
            event.accepted = true
        } else if (event.key === Qt.Key_Delete) {
            controller.reset_to_default(parameterId)
            event.accepted = true
        }
    }

    Rectangle {
        id: slot
        width: parent.width
        height: parent.height - wheel.theme.wheelLabelSpace
        radius: wheel.theme.radiusMd
        color: wheel.theme.engraving
        border.color: wheel.activeFocus ? wheel.accent : wheel.theme.line
        clip: true
        Repeater {
            model: wheel.theme.wheelRibs
            delegate: Rectangle {
                required property int index
                readonly property real phase: (index / (wheel.theme.wheelRibs - 1) + wheel.position * wheel.ribRoll) % 1
                x: wheel.theme.wheelRibInset
                width: slot.width - 2 * wheel.theme.wheelRibInset
                y: wheel.theme.wheelRibTop + phase * (slot.height - wheel.theme.wheelRibTop - wheel.theme.wheelRibBottom)
                height: wheel.theme.wheelRibHeight
                radius: height / 2
                color: wheel.theme.knobBody
                opacity: wheel.theme.wheelRibMinOpacity + (1 - wheel.theme.wheelRibMinOpacity) * Math.sin(Math.PI * phase)
            }
        }
        Rectangle {
            x: wheel.theme.wheelThumbInset
            width: slot.width - 2 * wheel.theme.wheelThumbInset
            y: wheel.theme.wheelThumbInset + (1 - wheel.position)
               * (slot.height - wheel.theme.wheelThumbHeight - 2 * wheel.theme.wheelThumbInset)
            height: wheel.theme.wheelThumbHeight
            radius: wheel.theme.radiusXs
            color: wheel.accent
        }
    }
    Text {
        y: slot.height + wheel.theme.spaceXs
        width: parent.width + 2 * wheel.theme.wheelLabelOverhang
        x: -wheel.theme.wheelLabelOverhang
        horizontalAlignment: Text.AlignHCenter
        text: wheel.label
        color: wheel.theme.dim
        font.pixelSize: wheel.theme.typeMicro
        font.bold: true
    }
    MouseArea {
        anchors.fill: slot
        enabled: wheel.controller.editable
        preventStealing: true
        onPressed: mouse => {
            wheel.forceActiveFocus()
            wheel.dragOrigin = wheel.position
            wheel.dragStartY = mouse.y
            wheel.controller.begin_gesture(wheel.parameterId)
        }
        onPositionChanged: mouse => {
            if (pressed)
                wheel.controller.set_normalised(wheel.parameterId,
                    Math.max(0, Math.min(1, wheel.dragOrigin + (wheel.dragStartY - mouse.y) / slot.height)))
        }
        onReleased: {
            if (wheel.spring)
                wheel.controller.set_normalised(wheel.parameterId, wheel.springCentre)
            wheel.controller.end_gesture(wheel.parameterId)
        }
        onCanceled: wheel.controller.cancel_gesture(wheel.parameterId)
        onDoubleClicked: wheel.controller.reset_to_default(wheel.parameterId)
    }
}
