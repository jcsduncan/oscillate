pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Rectangle {
    id: action
    required property var bridge
    required property string laneId
    required property string timebase
    required property color accentColor
    property bool available: laneId.length > 0 && bridge.audioEditEnabled
    width: label.implicitWidth + Theme.spaceXl
    height: Theme.automationTimebaseHeight
    radius: Theme.radiusMd
    color: available ? Theme.automationTimebaseFill : Theme.automationTimebaseUnavailable
    border.color: available ? accentColor : Theme.automationTimebaseUnavailableEdge
    border.width: Theme.hairline
    activeFocusOnTab: available
    Accessible.role: Accessible.Button
    Accessible.name: "Automation timebase: " + timebase + ". Activate to switch."
    Accessible.description: "Preserves current point times; later tempo changes follow the selected timebase."
    Accessible.onPressAction: if (available) bridge.trackAutomation.toggle_timebase(laneId)

    Text {
        id: label
        anchors.centerIn: parent
        text: action.timebase.toUpperCase()
        color: action.available ? action.accentColor : Theme.automationTimebaseUnavailableText
        font.pixelSize: Theme.typeCaption
        font.bold: true
    }
    MouseArea {
        Tip {}
        anchors.fill: parent
        enabled: action.available
        cursorShape: Qt.PointingHandCursor
        onClicked: action.bridge.trackAutomation.toggle_timebase(action.laneId)
    }
    Keys.onReturnPressed: if (available) bridge.trackAutomation.toggle_timebase(laneId)
    Keys.onEnterPressed: if (available) bridge.trackAutomation.toggle_timebase(laneId)
    Keys.onSpacePressed: if (available) bridge.trackAutomation.toggle_timebase(laneId)
}
