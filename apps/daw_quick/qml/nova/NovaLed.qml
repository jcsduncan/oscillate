pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// An on/off LED bound to an [Off, On] choice parameter.
Rectangle {
    id: led
    required property var panel
    required property string parameterId
    readonly property bool on: panel.controller.revision >= 0 && panel.controller.choice(parameterId) === 1
    // Lit in its module's accent.
    property color accent: panel.accentFor(parameterId)

    objectName: "novaLed:" + parameterId
    width: panel.theme.ledSize
    height: panel.theme.ledSize
    radius: panel.theme.radiusMd
    color: on ? accent : panel.theme.wellFill
    border.color: on ? accent : panel.theme.line2
    activeFocusOnTab: panel.controller.editable
    Accessible.role: Accessible.CheckBox
    Kit.HoverTip { theme: led.panel.theme }
    Accessible.name: panel.controller.parameter(parameterId).group + " on"
    Accessible.checked: on
    function toggle() {
        if (!panel.controller.editable)
            return
        panel.controller.set_choice(parameterId, on ? 0 : 1)
        panel.touchId(parameterId, panel.controller.parameter(parameterId).group.toUpperCase())
    }
    Keys.onSpacePressed: toggle()
    Keys.onReturnPressed: toggle()

    // The glow of a lit LED.
    Rectangle {
        visible: led.on
        anchors.centerIn: parent
        width: parent.width + led.panel.theme.ledGlowGrow
        height: width
        radius: width / 2
        color: led.panel.theme.soft(led.accent, led.panel.theme.glowAlpha)
        z: -1
    }
    MouseArea {
        anchors.fill: parent
        anchors.margins: -led.panel.theme.ledHitOutset
        onClicked: led.toggle()
    }
}
