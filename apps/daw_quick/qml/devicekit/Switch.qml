import QtQuick
import "MacroPalette.js" as MacroPalette

// A compact Off/On button with an LED, for two-choice parameters. Click,
// Space or Return flips it; Delete resets. Each flip is one undo step. The
// theme's switch tokens give it its look (a panel button or a pill).
Rectangle {
    id: toggle
    required property var controller
    required property KitTheme theme
    required property string parameterId
    property color accent: theme.accent
    property string label: info.name !== undefined ? info.name : ""
    readonly property var info: controller.parameter(parameterId)
    readonly property bool on: controller.revision >= 0 && controller.choice(parameterId) === 1
    readonly property bool pressed: toggleArea.pressed
    property string hint: "Click, Space or Return flips it · Delete resets"

    objectName: theme.prefix + "Toggle:" + parameterId
    implicitWidth: caption.implicitWidth + theme.switchPad
    implicitHeight: theme.switchHeight
    radius: theme.switchRadius < 0 ? height / 2 : theme.switchRadius
    gradient: Gradient {
        GradientStop { position: 0; color: toggle.theme.switchFill(toggle, 0) }
        GradientStop { position: 1; color: toggle.theme.switchFill(toggle, 1) }
    }
    border.color: theme.switchEdge(toggle)
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
    Keys.onDeletePressed: { controller.reset_to_default(parameterId); theme.lastTouched = parameterId }

    Rectangle {
        x: toggle.theme.switchLedX
        anchors.verticalCenter: parent.verticalCenter
        width: toggle.theme.switchLedSize
        height: toggle.theme.switchLedSize
        radius: width / 2
        color: toggle.theme.switchLed(toggle)
        border.width: toggle.theme.switchLedRim ? 1 : 0
        border.color: toggle.theme.switchLedEdge(toggle)
    }
    Text {
        id: caption
        x: toggle.theme.switchTextX
        anchors.verticalCenter: parent.verticalCenter
        text: toggle.label
        color: toggle.theme.switchInk(toggle)
        font.pixelSize: toggle.theme.switchTextSize
        font.bold: true
        font.letterSpacing: toggle.theme.switchLetterSpacing
    }
    Tip {
        theme: toggle.theme
        shown: toggleArea.containsMouse && !toggleArea.pressed
        text: toggle.label + "  " + (toggle.on ? "On" : "Off")
        detail: MacroPalette.tipLine(toggle.controller, toggle.parameterId) + toggle.hint
    }
    MouseArea {
        id: toggleArea
        anchors.fill: parent
        // At least the kit's least hit size, however small the control draws.
        anchors.leftMargin: -toggle.theme.hitOutset(parent.width)
        anchors.rightMargin: -toggle.theme.hitOutset(parent.width)
        anchors.topMargin: -toggle.theme.hitOutset(parent.height)
        anchors.bottomMargin: -toggle.theme.hitOutset(parent.height)
        enabled: toggle.controller.editable
        hoverEnabled: true
        onClicked: {
            toggle.forceActiveFocus()
            toggle.flip()
        }
    }
}
