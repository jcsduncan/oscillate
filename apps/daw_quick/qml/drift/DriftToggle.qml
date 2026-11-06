pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// A large glowing switch for an Off/On parameter (Freeze, Sync). Click,
// Space or Return toggles; Delete resets.
Rectangle {
    id: toggle
    required property var controller
    required property DriftTheme theme
    required property string parameterId
    property string title: ""
    property string caption: ""
    property color tone: theme.accent
    readonly property var info: controller.parameter(parameterId)
    readonly property bool on: controller.revision >= 0 && controller.choice(parameterId) === 1

    objectName: "driftToggle:" + parameterId
    implicitWidth: theme.toggleWidth
    implicitHeight: theme.toggleHeight
    radius: theme.toggleRadius
    activeFocusOnTab: controller.editable
    opacity: controller.editable ? 1 : theme.toggleDisabledOpacity
    color: on ? Qt.rgba(tone.r, tone.g, tone.b, theme.toggleOnAlpha) : area.containsMouse ? theme.buttonHi : theme.button
    border.color: on || activeFocus ? tone : theme.wellEdge
    border.width: on ? theme.litEdgeWidth : theme.hairline
    Accessible.role: Accessible.CheckBox
    Kit.HoverTip { theme: toggle.theme }
    Accessible.name: (info.group !== undefined ? info.group + " " : "") + title
    Accessible.checked: on
    Accessible.description: on ? "On" : "Off"

    function flip() {
        if (!controller.editable)
            return
        theme.lastTouched = parameterId
        controller.set_choice(parameterId, on ? 0 : 1)
    }
    Keys.onSpacePressed: flip()
    Keys.onReturnPressed: flip()
    Keys.onDeletePressed: { controller.reset_to_default(parameterId); theme.lastTouched = parameterId }

    // A halo behind the switch while it is on.
    Rectangle {
        anchors.fill: parent
        anchors.margins: -toggle.theme.toggleHaloSpread
        z: -1
        radius: parent.radius + toggle.theme.toggleHaloSpread
        color: "transparent"
        border.width: toggle.theme.toggleHaloSpread
        border.color: Qt.rgba(toggle.tone.r, toggle.tone.g, toggle.tone.b, toggle.on ? toggle.theme.toggleHaloAlpha : 0)
    }
    Rectangle {
        x: toggle.theme.toggleLedX
        anchors.verticalCenter: parent.verticalCenter
        width: toggle.theme.toggleLedSize
        height: width
        radius: width / 2
        color: toggle.on ? toggle.tone : toggle.theme.ledOff
        Rectangle {
            anchors.centerIn: parent
            width: toggle.theme.toggleLedRingSize
            height: width
            radius: width / 2
            color: "transparent"
            border.width: toggle.theme.toggleLedRingWidth
            border.color: Qt.rgba(toggle.tone.r, toggle.tone.g, toggle.tone.b, toggle.on ? toggle.theme.toggleLedRingAlpha : 0)
        }
    }
    Column {
        x: toggle.theme.toggleTextX
        anchors.verticalCenter: parent.verticalCenter
        spacing: toggle.theme.spaceXxs
        Text {
            text: toggle.title.toUpperCase()
            color: toggle.on ? toggle.tone : toggle.theme.ink
            font.pixelSize: toggle.theme.toggleTitleSize
            font.weight: Font.Black
            font.letterSpacing: toggle.theme.toggleTitleSpacing
        }
        Text {
            text: toggle.caption !== "" ? toggle.caption : (toggle.on ? "ON" : "OFF")
            color: toggle.theme.inkFaint
            font.pixelSize: toggle.theme.captionSize
            font.bold: true
            font.letterSpacing: toggle.theme.captionSpacing
        }
    }
    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        enabled: toggle.controller.editable
        onClicked: {
            toggle.forceActiveFocus()
            toggle.flip()
        }
    }
}
