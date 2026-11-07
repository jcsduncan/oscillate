pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "MacroPalette.js" as MacroPalette

// A value well with step arrows, for stepped values (tune, voices, bend,
// length). Arrows, wheel and keys step; double-click or Delete resets.
// Each step is one undo step.
Rectangle {
    id: stepper
    required property var controller
    required property KitTheme theme
    required property string parameterId
    property color accent: theme.stepperAccent
    property string prefix: ""
    property string label: ""
    // Show the bare number (a count, not a signed amount).
    property bool plain: false
    readonly property var info: controller.parameter(parameterId)
    readonly property string valueText: controller.revision >= 0 ? controller.display(parameterId) : ""

    objectName: theme.prefix + "Stepper:" + parameterId
    implicitWidth: theme.stepperWellWidth
    implicitHeight: theme.stepperWellHeight
    radius: theme.radiusSm
    color: theme.stepperFill
    border.color: activeFocus ? accent : theme.stepperEdge
    activeFocusOnTab: controller.editable
    opacity: controller.editable ? 1 : theme.stepperDisabledOpacity
    Accessible.role: Accessible.SpinBox
    Accessible.name: (info.group !== undefined ? info.group + " " : "") + (info.name !== undefined ? info.name : "")
    Accessible.description: valueText

    function stepBy(direction) {
        if (!controller.editable)
            return
        controller.step(parameterId, direction, false)
        theme.lastTouched = parameterId
    }
    Keys.onUpPressed: stepBy(1)
    Keys.onRightPressed: stepBy(1)
    Keys.onDownPressed: stepBy(-1)
    Keys.onLeftPressed: stepBy(-1)
    Keys.onDeletePressed: { controller.reset_to_default(parameterId); theme.lastTouched = parameterId }

    Text {
        anchors.centerIn: parent
        text: stepper.prefix + (stepper.plain && stepper.controller.revision >= 0
            ? Format.number(stepper.controller.value(stepper.parameterId), 0) : stepper.valueText)
        color: stepper.theme.stepperInk
        font.family: stepper.theme.mono
        font.pixelSize: stepper.theme.typeBody
        font.bold: true
    }
    Tip {
        theme: stepper.theme
        shown: wellArea.containsMouse && !wellArea.pressed
        text: stepper.Accessible.name + "  " + stepper.valueText
        detail: MacroPalette.tipLine(stepper.controller, stepper.parameterId)
            + "Arrows, wheel or keys step · double-click resets"
    }
    MouseArea {
        id: wellArea
        anchors.fill: parent
        enabled: stepper.controller.editable
        hoverEnabled: true
        onDoubleClicked: {
            stepper.controller.reset_to_default(stepper.parameterId)
            stepper.theme.lastTouched = stepper.parameterId
        }
        onPressed: stepper.forceActiveFocus()
        onWheel: wheel => {
            if (wheel.angleDelta.y !== 0)
                stepper.stepBy(wheel.angleDelta.y > 0 ? 1 : -1)
            wheel.accepted = true
        }
    }
    component Arrow: Text {
        id: arrow
        required property int direction
        width: stepper.theme.stepperArrowWidth
        height: stepper.height
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        text: direction < 0 ? "◀" : "▶"
        color: arrowArea.pressed ? stepper.theme.stepperInk : stepper.accent
        font.pixelSize: stepper.theme.stepperArrowSize
        MouseArea {
            id: arrowArea
            anchors.fill: parent
            enabled: stepper.controller.editable
            onClicked: {
                stepper.forceActiveFocus()
                stepper.stepBy(arrow.direction)
            }
        }
    }
    Arrow {
        objectName: stepper.theme.prefix + "StepperDown"
        x: stepper.theme.hairline
        direction: -1
    }
    Arrow {
        objectName: stepper.theme.prefix + "StepperUp"
        x: stepper.width - width - stepper.theme.hairline
        direction: 1
    }
    Text {
        visible: stepper.label !== ""
        anchors.horizontalCenter: parent.horizontalCenter
        y: stepper.height + stepper.theme.spaceXs
        text: stepper.label.toUpperCase()
        color: stepper.theme.inkDim
        font.pixelSize: stepper.theme.typeCaption
        font.bold: true
        font.letterSpacing: stepper.theme.stepperLabelSpacing
    }
}
