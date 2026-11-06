pragma ComponentBehavior: Bound
import QtQuick

// Performance wheel. A springy wheel (pitch bend) returns to centre on release.
Item {
    id: wheel
    required property var controller
    required property PrismTheme theme
    required property string parameterId
    property string label: ""
    property color accent: theme.voice
    property bool springy: false
    readonly property real position: controller.revision >= 0 ? controller.normalised(parameterId) : 0
    readonly property bool bipolar: controller.parameter(parameterId).bipolar === true
    property real originPosition: 0
    property real originY: 0

    objectName: "prismWheel:" + parameterId
    implicitWidth: theme.wheelWidth
    implicitHeight: theme.wheelHeight
    activeFocusOnTab: controller.editable
    Accessible.role: Accessible.Slider
    Accessible.name: label
    Accessible.description: controller.display(parameterId)
        + (springy ? ". Returns to centre when released" : "")

    Keys.onPressed: event => {
        const fine = (event.modifiers & Qt.ShiftModifier) !== 0
        if (event.key === Qt.Key_Up) {
            controller.step(parameterId, 1, fine)
            event.accepted = true
        } else if (event.key === Qt.Key_Down) {
            controller.step(parameterId, -1, fine)
            event.accepted = true
        } else if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
            controller.reset_to_default(parameterId)
            event.accepted = true
        }
    }

    Rectangle {
        id: slot
        x: (parent.width - width) / 2
        y: 0
        width: wheel.theme.wheelSlotWidth
        height: parent.height - wheel.theme.wheelCaptionSpace
        radius: wheel.theme.wheelSlotRadius
        color: wheel.theme.well
        border.color: wheel.activeFocus ? wheel.accent : wheel.theme.line
        // Ridges suggest a physical wheel.
        Repeater {
            model: wheel.theme.wheelRidges
            delegate: Rectangle {
                required property int index
                x: wheel.theme.wheelRidgeInset
                y: wheel.theme.wheelRidgeTop + index * (slot.height - 2 * wheel.theme.wheelRidgeTop)
                   / (wheel.theme.wheelRidges - 1)
                width: slot.width - 2 * wheel.theme.wheelRidgeInset
                height: wheel.theme.hairline
                color: wheel.theme.line
            }
        }
        Rectangle {
            visible: wheel.bipolar
            x: wheel.theme.wheelThumbInset
            y: slot.height / 2
            width: slot.width - 2 * wheel.theme.wheelThumbInset
            height: wheel.theme.hairline
            color: wheel.theme.faint
        }
        Rectangle {
            id: thumb
            x: wheel.theme.wheelThumbInset
            width: slot.width - 2 * wheel.theme.wheelThumbInset
            height: wheel.theme.wheelThumbHeight
            radius: wheel.theme.radiusMd
            y: wheel.theme.wheelThumbInset + (1 - wheel.position) * (slot.height - height - 2 * wheel.theme.wheelThumbInset)
            color: Qt.rgba(wheel.accent.r, wheel.accent.g, wheel.accent.b,
                           area.pressed ? wheel.theme.wheelThumbPressedAlpha : wheel.theme.wheelThumbAlpha)
            border.color: wheel.accent
            Behavior on y {
                enabled: !area.pressed
                NumberAnimation { duration: wheel.theme.wheelReturnDuration; easing.type: Easing.OutCubic }
            }
        }
        MouseArea {
            id: area
            anchors.fill: parent
            enabled: wheel.controller.editable
            preventStealing: true
            cursorShape: Qt.SizeVerCursor
            onPressed: mouse => {
                wheel.forceActiveFocus()
                wheel.originPosition = wheel.position
                wheel.originY = mouse.y
                wheel.controller.begin_gesture(wheel.parameterId)
            }
            onPositionChanged: mouse => {
                if (pressed)
                    wheel.controller.set_normalised(wheel.parameterId, Math.max(0, Math.min(1,
                        wheel.originPosition + (wheel.originY - mouse.y)
                            / (slot.height - wheel.theme.wheelThumbHeight - 2 * wheel.theme.wheelThumbInset))))
            }
            onReleased: {
                if (wheel.springy)
                    wheel.controller.reset_to_default(wheel.parameterId)
                wheel.controller.end_gesture(wheel.parameterId)
            }
            onCanceled: wheel.controller.cancel_gesture(wheel.parameterId)
        }
    }
    Text {
        y: slot.height + wheel.theme.spaceXs
        width: parent.width
        horizontalAlignment: Text.AlignHCenter
        text: wheel.label
        color: wheel.theme.dim
        font.pixelSize: wheel.theme.typeCaption
        font.bold: true
        font.letterSpacing: wheel.theme.wheelLabelSpacing
    }
    Text {
        y: slot.height + wheel.theme.wheelValueY
        width: parent.width
        horizontalAlignment: Text.AlignHCenter
        text: wheel.controller.revision >= 0 ? wheel.controller.display(wheel.parameterId) : ""
        color: wheel.accent
        font.pixelSize: wheel.theme.typeCaption
    }
}
